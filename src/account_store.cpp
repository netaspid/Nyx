#include "nyx/account_store.hpp"

#include "nyx/identity.hpp"
#include "nyx/paths.hpp"
#include "nyx/profile_crypto.hpp"
#include "nyx/util.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
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

bool is_account_locked(const std::string& account_id) {
  const std::string path = lock_file_path(account_id);
  std::ifstream in(path);
  if (!in) return false;
  std::uint32_t pid = 0;
  in >> pid;
  if (!in || pid == 0) return false;
#ifdef _WIN32
  if (pid == static_cast<std::uint32_t>(GetCurrentProcessId())) return false;
#else
  if (pid == static_cast<std::uint32_t>(getpid())) return false;
#endif
  return process_alive(pid);
}

bool write_session_lock(const std::string& account_id) {
  const std::string path = lock_file_path(account_id);
  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;
#ifdef _WIN32
  out << GetCurrentProcessId();
#else
  out << getpid();
#endif
  return static_cast<bool>(out);
}

void remove_session_lock(const std::string& account_id) {
  if (account_id.empty()) return;
  std::error_code ec;
  std::filesystem::remove(lock_file_path(account_id), ec);
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

bool load_registry(std::vector<AccountMeta>& out) {
  out.clear();
  std::ifstream file(registry_path());
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
        meta.created_ms = static_cast<uint64_t>(
            std::stoull(json.substr(created_key + 13)));
      } catch (...) {
      }
    }
    meta.locked = is_account_locked(meta.id);
    out.push_back(std::move(meta));
  }
  return true;
}

bool save_registry(const std::vector<AccountMeta>& accounts) {
  std::error_code ec;
  std::filesystem::create_directories(data_root(), ec);
  std::ofstream file(registry_path(), std::ios::trunc);
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
  for (const auto& a : accounts) {
    if (a.id == meta.id) return true;
  }
  accounts.push_back(meta);
  return save_registry(accounts);
}

bool open_account_session(const std::string& account_id, const Profile& profile,
                          const std::array<uint8_t, 32>* derived_key,
                          std::string* err) {
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
  return true;
}

bool persist_session_profile(std::string* err) {
  if (!session_open || !session_has_key) return false;
  const std::string path = account_data_dir(active_id) + "/" + kEncryptedProfileFilename;
  return save_encrypted_profile_with_key(path, session_profile, session_derived_key, err);
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

bool create_account(const std::string& nickname, const std::string& password,
                    AccountMeta* created, std::string* err) {
  auto trimmed = nickname;
  while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
    trimmed.erase(trimmed.begin());
  while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
    trimmed.pop_back();
  if (trimmed.empty()) {
    if (err) *err = "укажите никнейм";
    return false;
  }
  Profile profile = generate_profile(trimmed);
  const std::string id = account_id_from_profile(profile);
  const std::string dir = account_data_dir(id);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    if (err) *err = "не удалось создать каталог аккаунта";
    return false;
  }

  const std::string profile_path = dir + "/" + kEncryptedProfileFilename;
  if (!save_encrypted_profile(profile_path, profile, password, err)) return false;

  AccountMeta meta;
  meta.id = id;
  meta.nickname = profile.nickname;
  meta.created_ms = now_ms();
  if (!ensure_account_registered(meta)) {
    if (err) *err = "не удалось обновить реестр аккаунтов";
    return false;
  }
  if (created) *created = meta;
  std::array<uint8_t, 32> key{};
  Profile loaded;
  if (!load_encrypted_profile(profile_path, loaded, password, err, &key)) return false;
  return open_account_session(id, loaded, &key, err);
}

bool unlock_account(const std::string& account_id, const std::string& password,
                    Profile* profile_out, std::string* err) {
  const std::string profile_path = account_data_dir(account_id) + "/" + kEncryptedProfileFilename;
  Profile profile;
  std::array<uint8_t, 32> key{};
  if (!load_encrypted_profile(profile_path, profile, password, err, &key)) return false;
  if (account_id_from_profile(profile) != account_id) {
    if (err) *err = "идентификатор аккаунта не совпадает";
    return false;
  }
  if (!open_account_session(account_id, profile, &key, err)) return false;
  if (profile_out) *profile_out = profile;
  return true;
}

void lock_session() {
  if (!session_open) return;
  remove_session_lock(active_id);
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
  return std::filesystem::is_regular_file(legacy_profile_path(), ec);
}

bool import_legacy_profile(const std::string& password, AccountMeta* created,
                           std::string* err) {
  Profile profile;
  if (!load_profile(legacy_profile_path(), profile)) {
    if (err) *err = "старый profile.json не найден или повреждён";
    return false;
  }
  const std::string id = account_id_from_profile(profile);
  const std::string dir = account_data_dir(id);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);

  const std::string enc_path = dir + "/" + kEncryptedProfileFilename;
  if (!save_encrypted_profile(enc_path, profile, password, err)) return false;

  AccountMeta meta;
  meta.id = id;
  meta.nickname = profile.nickname;
  meta.created_ms = now_ms();
  if (!ensure_account_registered(meta)) {
    if (err) *err = "не удалось обновить реестр";
    return false;
  }

  const std::string backup = legacy_profile_path() + ".migrated";
  std::filesystem::rename(legacy_profile_path(), backup, ec);

  if (created) *created = meta;
  std::array<uint8_t, 32> key{};
  Profile loaded;
  if (!load_encrypted_profile(enc_path, loaded, password, err, &key)) return false;
  return open_account_session(id, loaded, &key, err);
}

bool update_session_nickname(const std::string& nickname, std::string* err) {
  if (!session_open) {
    if (err) *err = "сессия не активна";
    return false;
  }
  session_profile.nickname = nickname;
  if (!persist_session_profile(err)) return false;
  auto accounts = list_accounts();
  for (auto& a : accounts) {
    if (a.id == active_id) a.nickname = nickname;
  }
  return save_registry(accounts);
}

}  // namespace nyx
