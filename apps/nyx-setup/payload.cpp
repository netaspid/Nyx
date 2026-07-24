#include "payload.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace nyx_setup {

namespace {

constexpr char kMagic[4] = {'N', 'Y', 'X', 'I'};
constexpr char kFooterMagic[4] = {'N', 'Y', 'X', 'F'};
constexpr std::size_t kFooterSize = 12;

std::uint32_t read_u32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

std::uint64_t read_u64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  return v;
}

bool ensure_parent_dirs(const std::filesystem::path& file_path) {
  const auto parent = file_path.parent_path();
  if (parent.empty()) return true;
  std::error_code ec;
  std::filesystem::create_directories(parent, ec);
  return !ec;
}

bool write_file_bytes(const std::filesystem::path& path, const void* data, std::size_t size,
                      std::string* err) {
  const auto temp = path.string() + ".nyx.tmp";
  for (int attempt = 0; attempt < 10; ++attempt) {
    std::error_code ec;
    std::filesystem::remove(temp, ec);
    if (attempt > 0) std::filesystem::remove(path, ec);

    {
      std::ofstream out(temp, std::ios::binary | std::ios::trunc);
      if (!out) {
#ifdef _WIN32
        Sleep(200 * static_cast<DWORD>(attempt + 1));
#else
        usleep(200000 * static_cast<useconds_t>(attempt + 1));
#endif
        continue;
      }
      out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
      if (!out) {
        std::filesystem::remove(temp, ec);
#ifdef _WIN32
        Sleep(200 * static_cast<DWORD>(attempt + 1));
#else
        usleep(200000 * static_cast<useconds_t>(attempt + 1));
#endif
        continue;
      }
    }

    std::filesystem::rename(temp, path, ec);
    if (!ec) return true;
    std::filesystem::remove(temp, ec);
#ifdef _WIN32
    Sleep(200 * static_cast<DWORD>(attempt + 1));
#else
    usleep(200000 * static_cast<useconds_t>(attempt + 1));
#endif
  }

  if (err) *err = "Cannot write: " + path.string();
  return false;
}

}  // namespace

bool read_self_payload(std::vector<std::uint8_t>& out) {
#ifdef _WIN32
  wchar_t self[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return false;
  std::ifstream in(self, std::ios::binary);
#else
  char self[4096];
  const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
  if (n <= 0) return false;
  self[n] = '\0';
  std::ifstream in(self, std::ios::binary);
#endif
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < static_cast<std::streamoff>(kFooterSize + 12)) return false;
  std::vector<std::uint8_t> exe(static_cast<size_t>(size));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(exe.data()), size);
  if (!in) return false;

  const std::size_t footer_at = static_cast<std::size_t>(size) - kFooterSize;
  if (std::memcmp(exe.data() + footer_at + 8, kFooterMagic, 4) != 0) return false;

  const std::uint64_t payload_off = read_u64(exe.data() + footer_at);
  if (payload_off + 12 > footer_at) return false;
  if (std::memcmp(exe.data() + payload_off, kMagic, 4) != 0) return false;

  out.assign(exe.begin() + static_cast<std::ptrdiff_t>(payload_off),
             exe.begin() + static_cast<std::ptrdiff_t>(footer_at));
  return true;
}

bool parse_payload(const std::vector<std::uint8_t>& blob, std::vector<PayloadFile>& files) {
  files.clear();
  if (blob.size() < 12 || std::memcmp(blob.data(), kMagic, 4) != 0) return false;
  const std::uint32_t version = read_u32(blob.data() + 4);
  if (version != 1) return false;
  const std::uint32_t count = read_u32(blob.data() + 8);
  size_t off = 12;
  files.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (off + 4 > blob.size()) return false;
    const std::uint32_t path_len = read_u32(blob.data() + off);
    off += 4;
    if (off + path_len + 8 > blob.size()) return false;
    PayloadFile f;
    f.relative_path.assign(reinterpret_cast<const char*>(blob.data() + off), path_len);
    off += path_len;
    const std::uint64_t data_len = read_u64(blob.data() + off);
    off += 8;
    if (off + data_len > blob.size()) return false;
    f.data.assign(blob.begin() + off, blob.begin() + off + static_cast<size_t>(data_len));
    off += static_cast<size_t>(data_len);
    files.push_back(std::move(f));
  }
  return true;
}

bool extract_payload(const std::vector<std::uint8_t>& blob, const std::string& target_dir,
                     ProgressFn progress, std::string* err) {
  std::vector<PayloadFile> files;
  if (!parse_payload(blob, files)) {
    if (err) *err = "Invalid installer payload";
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(target_dir, ec);
  const size_t total = files.size();
  for (size_t i = 0; i < total; ++i) {
    const auto& f = files[i];
    std::filesystem::path full = std::filesystem::path(target_dir) / f.relative_path;
    if (!ensure_parent_dirs(full)) {
      if (err) *err = "Cannot create folder: " + full.string();
      return false;
    }
    if (f.data.empty()) {
      std::filesystem::create_directories(full, ec);
    } else {
      if (!write_file_bytes(full, f.data.data(), f.data.size(), err)) return false;
#ifndef _WIN32
      const auto base = full.filename().string();
      if (base == "nyx-app" || base == "nyx-node" || base == "nyx-rendezvous" ||
          base == "nyx-uninstall" || base == "nyx-app-wrapper.sh") {
        std::filesystem::permissions(full, std::filesystem::perms::owner_exec |
                                                 std::filesystem::perms::group_exec |
                                                 std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::add, ec);
      }
#endif
    }
    if (progress) {
      const int pct = static_cast<int>((100 * (i + 1)) / total);
      progress(pct, f.relative_path);
    }
  }
  return true;
}

}  // namespace nyx_setup
