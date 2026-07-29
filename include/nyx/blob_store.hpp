#pragma once

/** @file blob_store.hpp
 *  Chunked file reads from disk.
 */

#include "nyx/types.hpp"

#include <cstdint>
#include <fstream>
#include <string>

namespace nyx {

/** Sequential file reader for network transfer. */
class BlobReader {
 public:
  explicit BlobReader(std::string path);

  bool open();
  uint64_t size() const { return size_; }

  /** Reads up to max_len bytes at offset. @return 0 on EOF or error. */
  std::size_t read_at(uint64_t offset, ByteBuffer& out, std::size_t max_len);

 private:
  std::string path_;
  std::ifstream file_;
  uint64_t size_ = 0;
};

/** Writes a received file to disk. */
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

}  // namespace nyx
