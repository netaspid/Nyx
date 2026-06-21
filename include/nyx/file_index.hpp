#pragma once

/** @file file_index.hpp
 *  Индекс файлов в shared-папках: hash, размер, политика share (фаза 4–5).
 */

#include "nyx/chat_id.hpp"
#include "nyx/file_hash.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nyx {

/** Корень индекса с областью видимости. group_id = 0 → только личка 1:1. */
struct ShareRoot {
  std::string path;
  GroupId group_id{};

  bool is_personal() const {
    return std::all_of(group_id.begin(), group_id.end(), [](uint8_t b) { return b == 0; });
  }
};

/** Запись в индексе. */
struct FileEntry {
  FileHash hash{};
  uint64_t size = 0;
  uint64_t mtime_ms = 0;
  std::string root_path;
  std::string relative_path;
  std::string mime;
  GroupId share_group{};

  std::string absolute_path() const;
  std::string display_name() const { return relative_path; }
};

/** Сканирование и хранение метаданных файлов. */
class FileIndex {
 public:
  FileIndex();

  /** Добавляет корень. group_id=nullptr или zero → личка; иначе только это поле. */
  bool add_root(const std::string& root_path, const GroupId* group_id = nullptr);

  const std::vector<FileEntry>& entries() const { return entries_; }
  const std::vector<ShareRoot>& share_roots() const { return share_roots_; }

  /** Файлы, видимые в текущей сессии (личка или конкретное поле). */
  std::vector<FileEntry> entries_for_session(const GroupId& session_group) const;

  std::optional<FileEntry> find_by_hash(const FileHash& hash) const;
  std::optional<FileEntry> find_by_hash_hex(const std::string& hex) const;

  /** find + проверка share policy для сессии. */
  std::optional<FileEntry> find_for_session(const FileHash& hash,
                                            const GroupId& session_group) const;

  static std::string guess_mime(const std::string& path);
  static std::string group_id_hex(const GroupId& id);
  static bool group_id_from_hex(const std::string& hex, GroupId& out);

  bool load();
  bool save() const;

  static std::string index_path();

 private:
  bool scan_directory(const ShareRoot& root);

  std::vector<ShareRoot> share_roots_;
  std::vector<FileEntry> entries_;
};

}  // namespace nyx
