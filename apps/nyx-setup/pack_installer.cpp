/** Pack directory into NYXI blob and append to setup stub (+ NYXF footer). */
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr char kMagic[4] = {'N', 'Y', 'X', 'I'};
constexpr char kFooterMagic[4] = {'N', 'Y', 'X', 'F'};

void write_u32(std::ostream& out, std::uint32_t v) {
  out.put(static_cast<char>(v & 0xFF));
  out.put(static_cast<char>((v >> 8) & 0xFF));
  out.put(static_cast<char>((v >> 16) & 0xFF));
  out.put(static_cast<char>((v >> 24) & 0xFF));
}

void write_u64(std::ostream& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) out.put(static_cast<char>((v >> (8 * i)) & 0xFF));
}

bool copy_file(const fs::path& from, const fs::path& to) {
  std::ifstream in(from, std::ios::binary);
  std::ofstream out(to, std::ios::binary | std::ios::trunc);
  if (!in || !out) return false;
  out << in.rdbuf();
  return static_cast<bool>(out);
}

std::string rel_path(const fs::path& root, const fs::path& file) {
  std::string rel = file.lexically_relative(root).generic_string();
  return rel;
}

}  // namespace

int main(int argc, char** argv) {
  std::string stub_path;
  std::string src_dir;
  std::string out_path;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--stub" && i + 1 < argc) stub_path = argv[++i];
    else if (arg == "--srcdir" && i + 1 < argc) src_dir = argv[++i];
    else if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
  }
  if (stub_path.empty() || src_dir.empty() || out_path.empty()) {
    std::cerr << "usage: pack --stub stub.exe --srcdir staging/ --out NyxSetup.exe\n";
    return 1;
  }

  const fs::path root(src_dir);
  if (!fs::is_directory(root)) {
    std::cerr << "srcdir not found: " << src_dir << '\n';
    return 1;
  }

  std::vector<fs::path> files;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (entry.is_regular_file()) files.push_back(entry.path());
  }

  if (!copy_file(stub_path, out_path)) {
    std::cerr << "cannot copy stub\n";
    return 1;
  }

  const std::uint64_t payload_offset = static_cast<std::uint64_t>(fs::file_size(out_path));

  std::ofstream out(out_path, std::ios::binary | std::ios::app);
  if (!out) {
    std::cerr << "cannot open output for append\n";
    return 1;
  }

  out.write(kMagic, 4);
  write_u32(out, 1);
  write_u32(out, static_cast<std::uint32_t>(files.size()));

  for (const auto& path : files) {
    const std::string rel = rel_path(root, path);
    const auto bytes = fs::file_size(path);
    write_u32(out, static_cast<std::uint32_t>(rel.size()));
    out.write(rel.data(), static_cast<std::streamsize>(rel.size()));
    write_u64(out, bytes);

    std::ifstream in(path, std::ios::binary);
    if (!in) {
      std::cerr << "cannot read " << path << '\n';
      return 1;
    }
    out << in.rdbuf();
  }

  write_u64(out, payload_offset);
  out.write(kFooterMagic, 4);
  if (!out) {
    std::cerr << "write failed\n";
    return 1;
  }

  std::cout << "installer: " << out_path << " (" << files.size() << " files)\n";
  return 0;
}
