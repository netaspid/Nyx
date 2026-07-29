#pragma once

#include "nyx/types.hpp"

#include <cstdint>
#include <fstream>
#include <string>

namespace nyx {

class BlobReader {
public:
  explicit BlobReader(std::string path);

  bool open();
  uint64_t size() const { return size_; }

  std::size_t read_at(uint64_t offset, ByteBuffer& out, std::size_t max_len);

private:
  std::string path_;
  std::ifstream file_;
  uint64_t size_ = 0;
};

class BlobWriter {
public:
  explicit BlobWriter(std::string path);

  bool open(bool truncate = true);
  bool write_at(uint64_t offset, const ByteBuffer& data);
  bool close();

private:
  std::string path_;
  std::fstream file_;
};

} // namespace nyx
