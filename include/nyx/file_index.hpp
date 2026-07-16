#pragma once

/** @file file_index.hpp
 *  Индекс файлов в shared-папках: hash, размер, политика share (фаза 4–5).
 */

#include "nyx/chat_id.hpp"
#include "nyx/file_hash.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
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
  /** Имя для отображения в списке (последний сегмент пути). */
  std::string leaf_name() const;
  /** Маркер папки в списке (не скачивается). */
  bool is_directory() const { return mime == "application/x-nyx-directory"; }
};

/** Сканирование и хранение метаданных файлов. */
class FileIndex {
 public:
  /** progress(path, files_scanned, finished). */
  using ScanProgressFn =
      std::function<void(const std::string& path, int files_scanned, bool finished)>;

  FileIndex();

  /** Сбрасывает индекс в памяти (без записи на диск). */
  void clear();

  /** Добавляет корень и сканирует файлы (потокобезопасно). group_id=nullptr/zero → личка. */
  bool add_root(const std::string& root_path, const GroupId* group_id = nullptr,
                ScanProgressFn progress = nullptr);

  /** Удаляет корень и его файлы; повторный add_root того же пути допустим. */
  bool remove_root(const std::string& root_path, const GroupId* group_id = nullptr);

  /** Корни, видимые в области (личка или поле). */
  std::vector<ShareRoot> roots_for_session(const GroupId& session_group) const;
  /** Копия корней (потокобезопасно относительно сканирования). */
  std::vector<ShareRoot> share_roots() const;
  /** Копия записей индекса. */
  std::vector<FileEntry> entries() const;

  /** Файлы, видимые в текущей сессии (личка или конкретное поле). */
  std::vector<FileEntry> entries_for_session(const GroupId& session_group) const;

  /** Файлы + маркеры папок (size = число файлов внутри). */
  std::vector<FileEntry> listing_for_session(const GroupId& session_group) const;

  /** Один уровень дерева внутри share root (parent_rel "" = корень папки). */
  static std::vector<FileEntry> listing_level(const std::vector<FileEntry>& source,
                                              const std::string& share_root_path,
                                              const std::string& parent_rel);

  std::vector<FileEntry> listing_level_for_root(const GroupId& session_group,
                                                const std::string& share_root_path,
                                                const std::string& parent_rel) const;

  /** Содержимое уровня по абсолютному пути share root (без фильтра scope). */
  std::vector<FileEntry> listing_at_root(const std::string& share_root_path,
                                         const std::string& parent_rel) const;

  /** Число проиндексированных файлов в корне. */
  int count_in_root(const std::string& root_path) const;

  /** Пересканировать существующий корень. */
  bool rescan_root(const std::string& root_path, const GroupId* group_id = nullptr,
                   ScanProgressFn progress = nullptr);

  static FileEntry make_directory_marker(const ShareRoot& root, int file_count,
                                         const std::string& label_prefix = {});

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

  /** Перечитывает индекс из data_dir() активного аккаунта. */
  bool reload() { return load(); }

  static std::string index_path();

 private:
  bool scan_directory(const ShareRoot& root, ScanProgressFn progress = nullptr);

  mutable std::recursive_mutex mutex_;
  std::vector<ShareRoot> share_roots_;
  std::vector<FileEntry> entries_;
};

}  // namespace nyx
