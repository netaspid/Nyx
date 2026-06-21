#include "nyx/blob_store.hpp"

#include <filesystem>

namespace nyx {

BlobReader::BlobReader(std::string path) : path_(std::move(path)) {}

bool BlobReader::open() {
  file_.open(path_, std::ios::binary);
  if (!file_) return false;
  std::error_code ec;
  size_ = static_cast<uint64_t>(std::filesystem::file_size(path_, ec));
  return !ec;
}

std::size_t BlobReader::read_at(uint64_t offset, ByteBuffer& out, std::size_t max_len) {
  if (!file_) return 0;
  file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  out.resize(max_len);
  file_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(max_len));
  const auto got = static_cast<std::size_t>(file_.gcount());
  out.resize(got);
  return got;
}

BlobWriter::BlobWriter(std::string path) : path_(std::move(path)) {}

bool BlobWriter::open() {
  file_.open(path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
  if (file_) return true;
  file_.open(path_, std::ios::binary | std::ios::out | std::ios::trunc);
  return static_cast<bool>(file_);
}

bool BlobWriter::write_at(uint64_t offset, const ByteBuffer& data) {
  if (!file_) return false;
  file_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  file_.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(file_);
}

bool BlobWriter::close() {
  if (file_.is_open()) {
    file_.flush();
    file_.close();
  }
  return true;
}

}  // namespace nyx
