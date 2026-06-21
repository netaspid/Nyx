#pragma once

/** @file blob_store.hpp
 *  Чтение файлов с диска чанками (фаза 4).
 */

#include "nyx/types.hpp"

#include <cstdint>
#include <fstream>
#include <string>

namespace nyx {

/** Последовательное чтение файла для передачи по сети. */
class BlobReader {
 public:
  explicit BlobReader(std::string path);

  bool open();
  bool is_open() const { return file_.is_open(); }
  uint64_t size() const { return size_; }

  /** Читает до max_len байт с offset. @return 0 при EOF или ошибке. */
  std::size_t read_at(uint64_t offset, ByteBuffer& out, std::size_t max_len);

 private:
  std::string path_;
  std::ifstream file_;
  uint64_t size_ = 0;
};

/** Запись принимаемого файла на диск. */
class BlobWriter {
 public:
  explicit BlobWriter(std::string path);

  bool open();
  bool write_at(uint64_t offset, const ByteBuffer& data);
  bool close();

 private:
  std::string path_;
  std::fstream file_;
};

}  // namespace nyx
