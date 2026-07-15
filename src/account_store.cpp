#include "nyx/account_store.hpp"

#include "nyx/identity.hpp"
#include "nyx/paths.hpp"
#include "nyx/profile_crypto.hpp"
#include "nyx/recovery_phrase.hpp"
#include "nyx/util.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "Crypt32.lib")
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace nyx {
namespace {

std::string active_id;
Profile session_profile;
std::array<uint8_t, 32> session_derived_key{};
bool session_has_key = false;
bool session_open = false;

constexpr char kRememberMagic[4] = {'N', 'Y', 'X', 'R'};

std::string trim_ascii(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  return s;
}

uint64_t now_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string account_id_from_profile(const Profile& profile) {
  return to_hex(profile.public_key.data(), profile.public_key.size());
}

std::string profile_path_for(const std::string& account_id) {
  return account_data_dir(account_id) + "/" + kEncryptedProfileFilename;
}

std::string recovery_path_for(const std::string& account_id) {
  return account_data_dir(account_id) + "/" + recovery_vault_filename();
}

std::string remember_path_for(const std::string& account_id) {
  return account_data_dir(account_id) + "/unlock.token";
}

std::string auth_prefs_path() { return data_root() + "/auth_prefs.json"; }

std::string lock_file_path(const std::string& account_id) {
  return account_data_dir(account_id) + "/.session.lock";
}

bool process_alive(std::uint32_t pid) {
  if (pid == 0) return false;
#ifdef _WIN32
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (!h) return false;
  DWORD code = 0;
  const BOOL ok = GetExitCodeProcess(h, &code);
  CloseHandle(h);
  return ok && code == STILL_ACTIVE;
#else
  return kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

std::uint32_t current_pid() {
#ifdef _WIN32
  return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
  return static_cast<std::uint32_t>(getpid());
#endif
}

bool is_account_locked(const std::string& account_id) {
  std::ifstream in(path_from_utf8(lock_file_path(account_id)));
  if (!in) return false;
  std::uint32_t pid = 0;
  in >> pid;
  if (!in || pid == 0) return false;
  if (pid == current_pid()) return false;
  return process_alive(pid);
}

bool write_session_lock(const std::string& account_id) {
  std::ofstream out(path_from_utf8(lock_file_path(account_id)), std::ios::trunc);
  if (!out) return false;
  out << current_pid();
  return static_cast<bool>(out);
}

void remove_session_lock(const std::string& account_id) {
  if (account_id.empty()) return;
  std::error_code ec;
  std::filesystem::remove(path_from_utf8(lock_file_path(account_id)), ec);
}

bool protect_bytes(const uint8_t* data, std::size_t len, std::vector<uint8_t>& out) {
#ifdef _WIN32
  DATA_BLOB in{};
  in.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(data));
  in.cbData = static_cast<DWORD>(len);
  DATA_BLOB protected_blob{};
  if (!CryptProtectData(&in, L"NyxUnlock", nullptr, nullptr, nullptr, 0, &protected_blob)) {
    return false;
  }
  out.assign(protected_blob.pbData, protected_blob.pbData + protected_blob.cbData);
  LocalFree(protected_blob.pbData);
  return true;
#else
  // Portable fallback: XOR with machine-local mask (не равно DPAPI, но закрывает токен на диске).
  std::array<uint8_t, 32> mask{};
  const std::string seed = "nyx-remember-v1|" + std::to_string(geteuid());
  for (std::size_t i = 0; i < mask.size(); ++i) {
    mask[i] = static_cast<uint8_t>(seed[i % seed.size()] ^ (37 + i));
  }
  out.resize(len);
  for (std::size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(data[i] ^ mask[i % mask.size()]);
  return true;
#endif
}

bool unprotect_bytes(const uint8_t* data, std::size_t len, std::vector<uint8_t>& out) {
#ifdef _WIN32
  DATA_BLOB in{};
  in.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(data));
  in.cbData = static_cast<DWORD>(len);
  DATA_BLOB plain{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &plain)) return false;
  out.assign(plain.pbData, plain.pbData + plain.cbData);
  LocalFree(plain.pbData);
  return true;
#else
  std::array<uint8_t, 32> mask{};
  const std::string seed = "nyx-remember-v1|" + std::to_string(geteuid());
  for (std::size_t i = 0; i < mask.size(); ++i) {
    mask[i] = static_cast<uint8_t>(seed[i % seed.size()] ^ (37 + i));
  }
  out.resize(len);
  for (std::size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(data[i] ^ mask[i % mask.size()]);
  return true;
#endif
}

bool write_remember_token(const std::string& account_id, const std::array<uint8_t, 32>& key) {
  std::vector<uint8_t> protected_key;
  if (!protect_bytes(key.data(), key.size(), protected_key)) return false;

  const uint64_t expires =
      now_ms() + static_cast<uint64_t>(kRememberMeDays) * 24ull * 60ull * 60ull * 1000ull;

  std::vector<uint8_t> blob;
  blob.insert(blob.end(), kRememberMagic, kRememberMagic + 4);
  write_u64_le(blob, expires);
  write_u32_le(blob, static_cast<uint32_t>(protected_key.size()));
  blob.insert(blob.end(), protected_key.begin(), protected_key.end());

  std::ofstream out(path_from_utf8(remember_path_for(account_id)), std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
  return static_cast<bool>(out);
}

bool read_remember_token(const std::string& account_id, std::array<uint8_t, 32>& key_out,
                         uint64_t* expires_out) {
  std::ifstream in(path_from_utf8(remember_path_for(account_id)), std::ios::binary);
  if (!in) return false;
  std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (blob.size() < 4 + 8 + 4) return false;
  if (std::memcmp(blob.data(), kRememberMagic, 4) != 0) return false;
  const uint64_t expires = read_u64_le(blob.data() + 4);
  if (expires_out) *expires_out = expires;
  if (now_ms() >= expires) return false;
  const uint32_t plen = read_u32_le(blob.data() + 12);
  if (blob.size() < 16 + plen) return false;
  std::vector<uint8_t> plain;
  if (!unprotect_bytes(blob.data() + 16, plen, plain) || plain.size() != 32) return false;
  std::memcpy(key_out.data(), plain.data(), 32);
  return true;
}

bool load_registry(std::vector<AccountMeta>& out) {
  out.clear();
  std::ifstream file(path_from_utf8(registry_path()));
  if (!file) return true;

  std::ostringstream ss;
  ss << file.rdbuf();
  const std::string json = ss.str();
  std::size_t pos = 0;
  while ((pos = json.find("\"id\":\"", pos)) != std::string::npos) {
    pos += 6;
    const auto id_end = json.find('"', pos);
    if (id_end == std::string::npos) break;
    AccountMeta meta;
    meta.id = json.substr(pos, id_end - pos);
    pos = id_end;

    const auto nick_key = json.find("\"nickname\":\"", pos);
    if (nick_key == std::string::npos) break;
    const auto nick_start = nick_key + 12;
    const auto nick_end = json.find('"', nick_start);
    if (nick_end == std::string::npos) break;
    meta.nickname = json.substr(nick_start, nick_end - nick_start);
    meta.created_ms = 0;
    const auto created_key = json.find("\"created_ms\":", nick_end);
    if (created_key != std::string::npos && created_key < nick_end + 80) {
      try {
        meta.created_ms = static_cast<uint64_t>(std::stoull(json.substr(created_key + 13)));
      } catch (...) {
      }
    }
    meta.locked = is_account_locked(meta.id);
    meta.has_recovery = account_has_recovery(meta.id);
    meta.remember_active = account_remember_active(meta.id);
    out.push_back(std::move(meta));
  }
  return true;
}

bool save_registry(const std::vector<AccountMeta>& accounts) {
  std::error_code ec;
  std::filesystem::create_directories(path_from_utf8(data_root()), ec);
  std::ofstream file(path_from_utf8(registry_path()), std::ios::trunc);
  if (!file) return false;
  file << "{\"v\":1,\"accounts\":[";
  for (std::size_t i = 0; i < accounts.size(); ++i) {
    if (i) file << ',';
    const auto& a = accounts[i];
    file << "{\"id\":\"" << a.id << "\",\"nickname\":\"" << a.nickname
         << "\",\"created_ms\":" << a.created_ms << "}";
  }
  file << "]}\n";
  return static_cast<bool>(file);
}

bool ensure_account_registered(const AccountMeta& meta) {
  auto accounts = list_accounts();
  for (auto& a : accounts) {
    if (a.id == meta.id) {
      a.nickname = meta.nickname;
      if (meta.created_ms != 0) a.created_ms = meta.created_ms;
      return save_registry(accounts);
    }
  }
  accounts.push_back(meta);
  return save_registry(accounts);
}

bool open_account_session(const std::string& account_id, const Profile& profile,
                          const std::array<uint8_t, 32>* derived_key, std::string* err) {
  if (is_account_locked(account_id)) {
    if (err) *err = "этот аккаунт уже открыт в другом окне Nyx";
    return false;
  }
  set_account_data_dir(account_data_dir(account_id));
  ensure_data_dir();
  if (!write_session_lock(account_id)) {
    clear_account_data_dir();
    if (err) *err = "не удалось заблокировать сессию";
    return false;
  }
  active_id = account_id;
  session_profile = profile;
  session_open = true;
  session_has_key = derived_key != nullptr;
  if (derived_key) session_derived_key = *derived_key;
  set_last_account_id(account_id);
  return true;
}

bool persist_session_profile(std::string* err) {
  if (!session_open || !session_has_key) return false;
  return save_encrypted_profile_with_key(profile_path_for(active_id), session_profile,
                                         session_derived_key, err);
}

bool write_recovery_vault(const std::string& account_id, const Profile& profile,
                          const std::string& normalized_phrase, std::string* err) {
  return save_encrypted_profile(recovery_path_for(account_id), profile, normalized_phrase, err);
}

bool provision_account(const Profile& profile, const std::string& password,
                       std::string* recovery_phrase_out, AccountMeta* created, std::string* err) {
  const std::string id = account_id_from_profile(profile);
  const std::string dir = account_data_dir(id);
  std::error_code ec;
  std::filesystem::create_directories(path_from_utf8(dir), ec);
  if (ec) {
    if (err) *err = "не удалось создать каталог аккаунта";
    return false;
  }

  std::array<uint8_t, 32> key{};
  if (!save_encrypted_profile(profile_path_for(id), profile, password, err, &key)) return false;

  const std::string phrase = generate_recovery_phrase();
  std::string normalized;
  if (!normalize_recovery_phrase(phrase, &normalized, err)) return false;
  if (!write_recovery_vault(id, profile, normalized, err)) return false;

  AccountMeta meta;
  meta.id = id;
  meta.nickname = profile.nickname;
  meta.created_ms = now_ms();
  meta.has_recovery = true;
  if (!ensure_account_registered(meta)) {
    if (err) *err = "не удалось обновить реестр аккаунтов";
    return false;
  }
  if (created) *created = meta;
  if (recovery_phrase_out) *recovery_phrase_out = normalized;
  return open_account_session(id, profile, &key, err);
}

}  // namespace

std::string account_data_dir(const std::string& account_id) {
  return accounts_root() + "/" + account_id;
}

std::vector<AccountMeta> list_accounts() {
  std::vector<AccountMeta> accounts;
  load_registry(accounts);
  return accounts;
}

std::string last_account_id() {
  std::ifstream in(path_from_utf8(auth_prefs_path()));
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  const auto key = json.find("\"last_account_id\":\"");
  if (key == std::string::npos) return {};
  const auto start = key + 19;
  const auto end = json.find('"', start);
  if (end == std::string::npos) return {};
  return json.substr(start, end - start);
}

void set_last_account_id(const std::string& account_id) {
  std::error_code ec;
  std::filesystem::create_directories(path_from_utf8(data_root()), ec);
  std::ofstream out(path_from_utf8(auth_prefs_path()), std::ios::trunc);
  if (!out) return;
  out << "{\"last_account_id\":\"" << account_id << "\"}\n";
}

bool account_has_recovery(const std::string& account_id) {
  std::error_code ec;
  return std::filesystem::is_regular_file(path_from_utf8(recovery_path_for(account_id)), ec);
}

bool account_remember_active(const std::string& account_id) {
  std::array<uint8_t, 32> key{};
  return read_remember_token(account_id, key, nullptr);
}

void clear_remember_token(const std::string& account_id) {
  if (account_id.empty()) return;
  std::error_code ec;
  std::filesystem::remove(path_from_utf8(remember_path_for(account_id)), ec);
}

bool enable_remember_me(std::string* err) {
  if (!session_open || !session_has_key) {
    if (err) *err = "сессия не активна";
    return false;
  }
  if (!write_remember_token(active_id, session_derived_key)) {
    if (err) *err = "не удалось сохранить «запомнить меня»";
    return false;
  }
  return true;
}

bool create_account(const std::string& nickname, const std::string& password,
                    std::string* recovery_phrase_out, AccountMeta* created, std::string* err) {
  const std::string trimmed = trim_ascii(nickname);
  if (trimmed.empty()) {
    if (err) *err = "укажите никнейм";
    return false;
  }
  if (password.size() < kMinAccountPasswordLen) {
    if (err) *err = "пароль должен быть не короче 8 символов";
    return false;
  }
  return provision_account(generate_profile(trimmed), password, recovery_phrase_out, created, err);
}

bool unlock_account(const std::string& account_id, const std::string& password, bool remember_me,
                    Profile* profile_out, std::string* err) {
  Profile profile;
  std::array<uint8_t, 32> key{};
  if (!load_encrypted_profile(profile_path_for(account_id), profile, password, err, &key)) {
    return false;
  }
  if (account_id_from_profile(profile) != account_id) {
    if (err) *err = "идентификатор аккаунта не совпадает";
    return false;
  }
  if (!open_account_session(account_id, profile, &key, err)) return false;
  if (remember_me) {
    if (!write_remember_token(account_id, key) && err && err->empty()) {
      *err = "не удалось сохранить «запомнить меня»";
    }
  } else {
    clear_remember_token(account_id);
  }
  if (profile_out) *profile_out = profile;
  return true;
}

bool try_unlock_remembered(const std::string& account_id, Profile* profile_out, std::string* err) {
  std::array<uint8_t, 32> key{};
  if (!read_remember_token(account_id, key, nullptr)) {
    if (err) *err = "нет активной сохранённой сессии";
    return false;
  }

  Profile profile;
  std::string local_err;
  if (!load_encrypted_profile_with_key(profile_path_for(account_id), profile, key, &local_err)) {
    clear_remember_token(account_id);
    if (err) *err = local_err.empty() ? "сохранённая сессия устарела" : local_err;
    return false;
  }
  if (account_id_from_profile(profile) != account_id) {
    clear_remember_token(account_id);
    if (err) *err = "идентификатор аккаунта не совпадает";
    return false;
  }
  if (!open_account_session(account_id, profile, &key, err)) return false;
  if (profile_out) *profile_out = profile;
  return true;
}

bool reset_password_with_recovery(const std::string& account_id,
                                  const std::string& recovery_phrase,
                                  const std::string& new_password, std::string* err) {
  if (new_password.size() < kMinAccountPasswordLen) {
    if (err) *err = "пароль должен быть не короче 8 символов";
    return false;
  }
  std::string normalized;
  if (!normalize_recovery_phrase(recovery_phrase, &normalized, err)) return false;
  if (!account_has_recovery(account_id)) {
    if (err) *err = "для этого аккаунта нет recovery-фразы";
    return false;
  }

  Profile profile;
  if (!load_encrypted_profile(recovery_path_for(account_id), profile, normalized, err)) {
    if (err && err->find("пароль") != std::string::npos) *err = "неверная recovery-фраза";
    return false;
  }
  if (account_id_from_profile(profile) != account_id) {
    if (err) *err = "идентификатор аккаунта не совпадает";
    return false;
  }

  std::array<uint8_t, 32> key{};
  if (!save_encrypted_profile(profile_path_for(account_id), profile, new_password, err, &key)) {
    return false;
  }
  if (!write_recovery_vault(account_id, profile, normalized, err)) return false;
  clear_remember_token(account_id);
  return true;
}

void lock_session(bool clear_remember) {
  if (!session_open) return;
  const std::string id = active_id;
  remove_session_lock(id);
  if (clear_remember) clear_remember_token(id);
  active_id.clear();
  session_profile = Profile{};
  session_has_key = false;
  std::fill(session_derived_key.begin(), session_derived_key.end(), 0);
  session_open = false;
  clear_account_data_dir();
}

std::string active_account_id() { return session_open ? active_id : std::string{}; }

bool active_profile(Profile& out) {
  if (!session_open) return false;
  out = session_profile;
  return true;
}

bool legacy_profile_pending() {
  std::error_code ec;
  return std::filesystem::is_regular_file(path_from_utf8(legacy_profile_path()), ec);
}

bool import_legacy_profile(const std::string& password, std::string* recovery_phrase_out,
                           AccountMeta* created, std::string* err) {
  Profile profile;
  if (!load_profile(legacy_profile_path(), profile)) {
    if (err) *err = "старый profile.json не найден или повреждён";
    return false;
  }
  if (!provision_account(profile, password, recovery_phrase_out, created, err)) return false;

  std::error_code ec;
  const std::string backup = legacy_profile_path() + ".migrated";
  std::filesystem::rename(path_from_utf8(legacy_profile_path()), path_from_utf8(backup), ec);
  return true;
}

bool update_session_nickname(const std::string& nickname, std::string* err) {
  if (!session_open) {
    if (err) *err = "сессия не активна";
    return false;
  }
  session_profile.nickname = nickname;
  if (!persist_session_profile(err)) return false;
  if (account_has_recovery(active_id)) {
    // recovery.nyx содержит старый nickname — обновляем только profile.nyx достаточно
    // для входа; nickname в recovery обновится при следующем reset. Для консистентности
    // перезаписывать recovery без фразы нельзя.
  }
  auto accounts = list_accounts();
  for (auto& a : accounts) {
    if (a.id == active_id) a.nickname = nickname;
  }
  return save_registry(accounts);
}

}  // namespace nyx
