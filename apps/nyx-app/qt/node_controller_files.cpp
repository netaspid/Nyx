#include "node_controller.hpp"

#include "android_platform.hpp"
#include "host_env.hpp"
#include "node_controller_media.hpp"

#include "nyx/file_access.hpp"
#include "nyx/file_hash.hpp"
#include "nyx/file_index.hpp"
#include "nyx/group.hpp"
#include "nyx/identity.hpp"
#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMetaObject>
#include <QMimeDatabase>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

namespace {

QString formatFileSizeLabel(quint64 bytes, bool is_directory) {
  if (is_directory) {
    if (bytes == 0)
      return QStringLiteral("пустая папка");
    if (bytes == 1)
      return QStringLiteral("1 файл");
    return QStringLiteral("%1 файлов").arg(bytes);
  }
  if (bytes < 1024)
    return QString::number(bytes) + QStringLiteral(" B");
  if (bytes < 1024 * 1024)
    return QString::number(bytes / 1024.0, 'f', 1) + QStringLiteral(" KB");
  if (bytes < 1024ULL * 1024 * 1024) {
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MB");
  }
  return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 1) + QStringLiteral(" GB");
}

QString utf8q(const std::string& s) {
  if (s.empty())
    return {};
  const auto n = static_cast<qsizetype>(
      std::min(s.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
  return QString::fromUtf8(s.data(), n);
}

} // namespace

QString NodeController::joinFileRelPath(const QString& browseRel, const QString& entryRel) const {
  QString rel = entryRel.trimmed();
  rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
  const QString browse = browseRel.trimmed();
  if (browse.isEmpty())
    return rel;
  if (rel.isEmpty())
    return browse;
  if (rel.startsWith(browse + QLatin1Char('/')))
    return rel;
  return browse + QLatin1Char('/') + rel;
}

QVariantList NodeController::entriesToVariant(const std::vector<nyx::FileEntry>& entries,
                                              bool remote) const {
  QVariantList list;
  const QString browse_rel =
      remote ? files_ui_.file_remote_browse_path_ : files_ui_.file_browse_path_;
  for (const auto& e : entries) {
    QVariantMap m;
    const std::string leaf = e.leaf_name();
    const std::string rel = e.relative_path;
    QString display = utf8q(leaf);
    if (display.isEmpty() && !rel.empty())
      display = utf8q(rel);
    const bool is_dir = e.is_directory();
    QString full_rel;
    if (is_dir) {
      full_rel = utf8q(rel);
      full_rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
    } else {
      full_rel = joinFileRelPath(browse_rel, utf8q(rel));
    }
    const QString root = utf8q(e.root_path);
    QString owner_hex;
    if (!std::all_of(e.owner_id.begin(), e.owner_id.end(), [](uint8_t b) { return b == 0; })) {
      owner_hex = QString::fromStdString(nyx::to_hex(e.owner_id.data(), e.owner_id.size()));
    } else if (is_dir && display.size() == 64) {

      bool hex_ok = true;
      for (const QChar c : display) {
        if (!c.isDigit() && (c.toLower() < QLatin1Char('a') || c.toLower() > QLatin1Char('f'))) {
          hex_ok = false;
          break;
        }
      }
      if (hex_ok)
        owner_hex = display.toLower();
    } else {
      const QString posix = full_rel;
      const int slash = posix.indexOf(QLatin1Char('/'));
      const QString head = slash > 0 ? posix.left(slash) : QString {};
      if (head.size() == 64)
        owner_hex = head.toLower();
    }
    QString owner_label;
    if (!owner_hex.isEmpty()) {
      owner_label = userDisplayName(owner_hex);
      if (is_dir && display.toLower() == owner_hex)
        display = owner_label;
    }
    m.insert(QStringLiteral("name"), display);
    m.insert(QStringLiteral("navPath"), utf8q(rel));
    m.insert(QStringLiteral("fullRelPath"), full_rel);
    m.insert(QStringLiteral("rootPath"), root);
    m.insert(QStringLiteral("hash"), utf8q(nyx::hash_hex(e.hash)));
    const auto size = static_cast<qulonglong>(e.size);
    m.insert(QStringLiteral("size"), size);
    m.insert(QStringLiteral("sizeLabel"), formatFileSizeLabel(size, is_dir));
    m.insert(QStringLiteral("mime"), utf8q(e.mime));
    m.insert(QStringLiteral("isRemote"), remote);
    m.insert(QStringLiteral("isDirectory"), is_dir);
    m.insert(QStringLiteral("ownerId"), owner_hex);
    m.insert(QStringLiteral("ownerLabel"), owner_label);
    if (remote) {
      const bool can_dl =
          is_dir ? canDownloadFolderAt(root, full_rel) : canFileDownloadAt(root, full_rel);
      m.insert(QStringLiteral("canDownload"), can_dl);
      m.insert(QStringLiteral("canOpenRemote"), !is_dir && canFileOpenRemoteAt(root, full_rel));
    }
    list.append(m);
  }
  return list;
}

void NodeController::resetFileBrowse() {
  files_ui_.file_browse_path_.clear();
  syncFileBrowseCrumbs();
}

QString NodeController::normalizeShareRootPath(const QString& path) const {
  QString p = path.trimmed();
  if (p.isEmpty())
    return p;
  p.replace(QLatin1Char('\\'), QLatin1Char('/'));
#ifdef Q_OS_WIN
  return p.toLower();
#else
  return p;
#endif
}

bool NodeController::shareRootPathsEqual(const QString& a, const QString& b) const {
  return normalizeShareRootPath(a) == normalizeShareRootPath(b);
}

void NodeController::setFileScopeGroupId(const QString& groupIdHex) {
  const QString gid = groupIdHex.trimmed().toLower();
  if (files_ui_.file_scope_group_id_ == gid)
    return;
  files_ui_.file_scope_group_id_ = gid;
  files_ui_.file_selected_share_root_.clear();
  resetFileBrowse();
  syncFileScopeLabel();
  service_.save_files_scope_group_id(gid.toStdString());
  service_.save_files_selected_root({});
  if (!gid.isEmpty()) {
    service_.set_active_session(QStringLiteral("group:%1").arg(gid).toStdString());
  }
  refreshFileLists();
  refreshFileAccessLists();
  if (fileExchangeReady())
    refreshRemoteFileList();
  emit filesChanged();
  emit fileAccessChanged();
}

bool NodeController::fileExchangeReady() const {
  if (!files_ui_.file_scope_group_id_.isEmpty()) {
    return !service_.file_exchange_session_id(files_ui_.file_scope_group_id_.toStdString()).empty();
  }
  return service_.can_request_remote_files();
}

QString NodeController::fileExchangeHint() const {
  return QString::fromStdString(service_.file_exchange_hint());
}

QString NodeController::scopeLabelForGroupId(const QString& groupIdHex) const {
  if (groupIdHex.isEmpty())
    return QStringLiteral("Личные");
  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("groupId")).toString() == groupIdHex) {
      return m.value(QStringLiteral("name")).toString();
    }
  }
  return groupIdHex.left(8) + QStringLiteral("…");
}

bool NodeController::canRemoveShareRoot(const nyx::ShareRoot& root) const {
  if (root.is_personal())
    return true;
  const QString gid =
      QString::fromStdString(nyx::FileIndex::group_id_hex(root.group_id)).trimmed().toLower();
  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("groupId")).toString() != gid)
      continue;
    if (m.value(QStringLiteral("isOwner")).toBool())
      return true;
    break;
  }
  const uint32_t perms = service_.my_file_permissions(gid.toStdString(), {});
  return nyx::FileAccessStore::has_permission(perms, nyx::FilePermission::ManageShares);
}

void NodeController::resetFilesUiState() {
  files_ui_.file_scope_group_id_.clear();
  files_ui_.file_selected_share_root_.clear();
  files_ui_.file_share_roots_.clear();
  files_ui_.file_role_list_.clear();
  files_ui_.file_member_access_.clear();
  files_ui_.file_browse_path_.clear();
  files_ui_.file_browse_crumbs_.clear();
  files_ui_.file_resources_root_.clear();
  files_ui_.file_remote_browse_path_.clear();
  files_ui_.file_remote_browse_crumbs_.clear();
  files_ui_.local_file_list_.clear();
  files_ui_.remote_file_list_.clear();
  files_ui_.file_scope_label_ = QStringLiteral("Личные файлы");
}

void NodeController::syncFileScopeFromSavedOrRoots() {
  const auto roots = service_.all_share_roots();
  if (roots.empty())
    return;

  if (!files_ui_.file_selected_share_root_.isEmpty()) {
    const auto scope_roots =
        service_.share_roots_for_scope(files_ui_.file_scope_group_id_.toStdString());
    for (const auto& r : scope_roots) {
      const QString path = QString::fromStdString(r.path);
      if (!shareRootPathsEqual(files_ui_.file_selected_share_root_, path))
        continue;
      files_ui_.file_selected_share_root_ = path;
      service_.save_files_selected_root(path.toStdString());
      return;
    }
    files_ui_.file_selected_share_root_.clear();
    service_.save_files_selected_root({});
  }

  const auto scope_roots =
      service_.share_roots_for_scope(files_ui_.file_scope_group_id_.toStdString());
  if (!scope_roots.empty())
    return;
}

void NodeController::setFilesSection(int section) {
  if (section < 0)
    section = 0;
  if (section > 2)
    section = 2;
  if (files_ui_.files_section_ == section && section != 2)
    return;
  files_ui_.files_section_ = section;
  if (files_ui_.files_section_ == 1 && fileExchangeReady())
    refreshRemoteFileList();
  if (files_ui_.files_section_ == 2) {
    refreshGroupList();
    refreshFileAccessLists();
  }
  emit filesChanged();
  emit fileAccessChanged();
}

std::vector<nyx::FileEntry>
NodeController::remoteRootsCatalog(const std::vector<nyx::FileEntry>& all) const {
  nyx::GroupId scope {};
  if (!files_ui_.file_scope_group_id_.isEmpty()) {
    nyx::GroupStore::group_id_from_hex(files_ui_.file_scope_group_id_.toStdString(), scope);
  }

  std::map<std::string, int> file_counts;
  for (const auto& e : all) {
    if (e.is_directory())
      continue;
    file_counts[nyx::normalize_utf8_path(e.root_path)]++;
  }

  std::map<std::string, nyx::FileEntry> by_root;
  for (const auto& e : all) {
    if (!e.is_directory())
      continue;
    const std::string norm = nyx::normalize_utf8_path(e.root_path);
    nyx::FileEntry marker = e;
    marker.root_path = norm;
    const auto it = file_counts.find(norm);
    if (it != file_counts.end())
      marker.size = static_cast<uint64_t>(it->second);
    by_root[norm] = std::move(marker);
  }

  for (const auto& [path, count] : file_counts) {
    if (by_root.count(path))
      continue;
    nyx::ShareRoot sr;
    sr.path = path;
    sr.group_id = scope;
    by_root[path] = nyx::FileIndex::make_directory_marker(sr, count);
  }

  std::vector<nyx::FileEntry> out;
  out.reserve(by_root.size());
  for (auto& [_, entry] : by_root)
    out.push_back(std::move(entry));
  return out;
}

void NodeController::syncFileBrowseCrumbs() {
  files_ui_.file_browse_crumbs_.clear();
  if (files_ui_.file_selected_share_root_.isEmpty())
    return;

  const QFileInfo rootInfo(files_ui_.file_selected_share_root_);
  QVariantMap rootCrumb;
  rootCrumb.insert(QStringLiteral("label"),
                   rootInfo.fileName().isEmpty() ? files_ui_.file_selected_share_root_
                                                 : rootInfo.fileName());
  rootCrumb.insert(QStringLiteral("path"), QString());
  files_ui_.file_browse_crumbs_.append(rootCrumb);

  QString rel = files_ui_.file_browse_path_;
  if (rel.isEmpty())
    return;
  rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
  int from = 0;
  QString built;
  while (from < rel.length()) {
    const int slash = rel.indexOf(QLatin1Char('/'), from);
    const QString segment = slash < 0 ? rel.mid(from) : rel.mid(from, slash - from);
    if (!segment.isEmpty()) {
      built = built.isEmpty() ? segment : built + QLatin1Char('/') + segment;
      QVariantMap crumb;
      crumb.insert(QStringLiteral("label"), segment);
      crumb.insert(QStringLiteral("path"), built);
      files_ui_.file_browse_crumbs_.append(crumb);
    }
    if (slash < 0)
      break;
    from = slash + 1;
  }
}

void NodeController::syncRemoteBrowseCrumbs() {
  files_ui_.file_remote_browse_crumbs_.clear();

  QVariantMap top;
  top.insert(QStringLiteral("label"), QStringLiteral("Ресурсы"));
  top.insert(QStringLiteral("path"), QString());
  top.insert(QStringLiteral("isRoots"), true);
  files_ui_.file_remote_browse_crumbs_.append(top);

  if (files_ui_.file_resources_root_.isEmpty())
    return;

  const QFileInfo rootInfo(files_ui_.file_resources_root_);
  QVariantMap rootCrumb;
  rootCrumb.insert(QStringLiteral("label"),
                   rootInfo.fileName().isEmpty() ? files_ui_.file_resources_root_
                                                 : rootInfo.fileName());
  rootCrumb.insert(QStringLiteral("path"), QString());
  rootCrumb.insert(QStringLiteral("isRoots"), false);
  files_ui_.file_remote_browse_crumbs_.append(rootCrumb);

  QString rel = files_ui_.file_remote_browse_path_;
  if (rel.isEmpty())
    return;
  rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
  int from = 0;
  QString built;
  while (from < rel.length()) {
    const int slash = rel.indexOf(QLatin1Char('/'), from);
    const QString segment = slash < 0 ? rel.mid(from) : rel.mid(from, slash - from);
    if (!segment.isEmpty()) {
      built = built.isEmpty() ? segment : built + QLatin1Char('/') + segment;
      QVariantMap crumb;
      crumb.insert(QStringLiteral("label"), segment);
      crumb.insert(QStringLiteral("path"), built);
      crumb.insert(QStringLiteral("isRoots"), false);
      files_ui_.file_remote_browse_crumbs_.append(crumb);
    }
    if (slash < 0)
      break;
    from = slash + 1;
  }
}

void NodeController::setFileSelectedShareRoot(const QString& path) {
  const QString p = path.trimmed();
  if (p.isEmpty())
    return;

  QString canonical;
  const auto scope_roots =
      service_.share_roots_for_scope(files_ui_.file_scope_group_id_.toStdString());
  for (const auto& r : scope_roots) {
    const QString rp = QString::fromStdString(r.path);
    if (!shareRootPathsEqual(p, rp))
      continue;
    canonical = rp;
    break;
  }
  if (canonical.isEmpty())
    return;
  if (files_ui_.file_selected_share_root_ == canonical)
    return;

  files_ui_.file_selected_share_root_ = canonical;
  resetFileBrowse();
  refreshLocalFileModel();
  service_.save_files_selected_root(canonical.toStdString());
  emit filesChanged();
}

void NodeController::browseIntoFolder(const QString& navPath, const QString& itemRootPath) {
  if (files_ui_.files_section_ == 1) {
    if (files_ui_.file_resources_root_.isEmpty()) {
      QString root = itemRootPath.trimmed();
      if (root.isEmpty())
        root = navPath.trimmed();
      if (root.isEmpty())
        return;
      for (const auto& e : service_.remote_files()) {
        if (shareRootPathsEqual(root, QString::fromStdString(e.root_path))) {
          root = QString::fromStdString(e.root_path);
          break;
        }
      }
      files_ui_.file_resources_root_ = root;
      files_ui_.file_remote_browse_path_.clear();
    } else {
      QString rel = navPath.trimmed();
      rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
      files_ui_.file_remote_browse_path_ = rel;
    }
    syncRemoteBrowseCrumbs();

    service_.request_remote_files_at(files_ui_.file_scope_group_id_.toStdString(),
                                     files_ui_.file_resources_root_.toStdString(),
                                     files_ui_.file_remote_browse_path_.toStdString());
    refreshRemoteFileModel();
    emit filesChanged();
    return;
  }

  if (navPath.trimmed().isEmpty())
    return;
  QString rel = navPath.trimmed();
  rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
  files_ui_.file_browse_path_ = rel;
  syncFileBrowseCrumbs();
  refreshLocalFileModel();
  emit filesChanged();
}

void NodeController::browseUp() {
  if (files_ui_.files_section_ == 1) {
    if (!files_ui_.file_remote_browse_path_.isEmpty()) {
      QString rel = files_ui_.file_remote_browse_path_;
      rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
      const int slash = rel.lastIndexOf(QLatin1Char('/'));
      files_ui_.file_remote_browse_path_ = slash < 0 ? QString() : rel.left(slash);
      syncRemoteBrowseCrumbs();
      service_.request_remote_files_at(files_ui_.file_scope_group_id_.toStdString(),
                                       files_ui_.file_resources_root_.toStdString(),
                                       files_ui_.file_remote_browse_path_.toStdString());
      refreshRemoteFileModel();
    } else if (!files_ui_.file_resources_root_.isEmpty()) {
      files_ui_.file_resources_root_.clear();
      syncRemoteBrowseCrumbs();
      if (fileExchangeReady()) {
        service_.request_remote_files_at(files_ui_.file_scope_group_id_.toStdString(), {}, {});
      }
      refreshRemoteFileModel();
    }
    emit filesChanged();
    return;
  }

  if (!files_ui_.file_browse_path_.isEmpty()) {
    QString rel = files_ui_.file_browse_path_;
    rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const int slash = rel.lastIndexOf(QLatin1Char('/'));
    files_ui_.file_browse_path_ = slash < 0 ? QString() : rel.left(slash);
    syncFileBrowseCrumbs();
    refreshLocalFileModel();
    emit filesChanged();
    return;
  }

  if (!files_ui_.file_selected_share_root_.isEmpty()) {
    files_ui_.file_selected_share_root_.clear();
    resetFileBrowse();
    service_.save_files_selected_root({});
    refreshLocalFileModel();
    emit filesChanged();
  }
}

void NodeController::browseToCrumb(int index) {
  if (files_ui_.files_section_ == 1) {
    if (index < 0 || index >= files_ui_.file_remote_browse_crumbs_.size())
      return;
    const QVariantMap crumb = files_ui_.file_remote_browse_crumbs_.at(index).toMap();
    if (crumb.value(QStringLiteral("isRoots")).toBool() || index == 0) {
      files_ui_.file_resources_root_.clear();
      files_ui_.file_remote_browse_path_.clear();
      syncRemoteBrowseCrumbs();
      if (fileExchangeReady()) {
        service_.request_remote_files_at(files_ui_.file_scope_group_id_.toStdString(), {}, {});
      }
      refreshRemoteFileModel();
      emit filesChanged();
      return;
    }
    if (index == 1) {
      files_ui_.file_remote_browse_path_.clear();
    } else {
      files_ui_.file_remote_browse_path_ = crumb.value(QStringLiteral("path")).toString();
    }
    syncRemoteBrowseCrumbs();
    if (!files_ui_.file_resources_root_.isEmpty() && fileExchangeReady()) {
      service_.request_remote_files_at(files_ui_.file_scope_group_id_.toStdString(),
                                       files_ui_.file_resources_root_.toStdString(),
                                       files_ui_.file_remote_browse_path_.toStdString());
    }
    refreshRemoteFileModel();
    emit filesChanged();
    return;
  }

  if (index < 0 || index >= files_ui_.file_browse_crumbs_.size())
    return;
  files_ui_.file_browse_path_ =
      files_ui_.file_browse_crumbs_.at(index).toMap().value(QStringLiteral("path")).toString();
  syncFileBrowseCrumbs();
  refreshLocalFileModel();
  emit filesChanged();
}

void NodeController::addDroppedUrls(const QVariantList& urls) {
  if (urls.isEmpty())
    return;
  if (!canAddShareFolder()) {
    showToast(QStringLiteral("Нет права добавлять папки в эту область"));
    return;
  }
  if (files_ui_.file_index_busy_.load()) {
    showToast(QStringLiteral("Индексация уже выполняется"));
    return;
  }
  QStringList dirs;
  for (const QVariant& u : urls) {
    QString p = u.toString().trimmed();
    if (p.startsWith(QStringLiteral("file:///")))
      p = QUrl(p).toLocalFile();
    if (p.isEmpty())
      continue;
    QFileInfo info(p);
    if (info.isDir())
      dirs.push_back(info.absoluteFilePath());
    else if (info.isFile())
      dirs.push_back(info.absolutePath());
  }
  if (dirs.isEmpty()) {
    showToast(QStringLiteral("Не удалось добавить из перетаскивания"));
    return;
  }
  runIndexJob(dirs.front(), files_ui_.file_scope_group_id_, false);
  for (int i = 1; i < dirs.size(); ++i) {

    Q_UNUSED(i);
  }
  if (dirs.size() > 1) {
    showToast(QStringLiteral("Индексируется первая папка; остальные добавьте по очереди"));
  }
}

void NodeController::syncFileScopeLabel() {
  if (files_ui_.file_scope_group_id_.isEmpty()) {
    files_ui_.file_scope_label_ = QStringLiteral("Личные файлы");
    return;
  }
  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("groupId")).toString() == files_ui_.file_scope_group_id_) {
      files_ui_.file_scope_label_ = m.value(QStringLiteral("name")).toString();
      return;
    }
  }
  files_ui_.file_scope_label_ = files_ui_.file_scope_group_id_.left(8) + QStringLiteral("…");
}

void NodeController::refreshFileShareRoots() {
  files_ui_.file_share_roots_.clear();
  const auto roots = service_.share_roots_for_scope(files_ui_.file_scope_group_id_.toStdString());
  for (const auto& r : roots) {
    const QString path = QString::fromStdString(r.path);
    const QString scopeId =
        r.is_personal()
            ? QString()
            : QString::fromStdString(nyx::FileIndex::group_id_hex(r.group_id)).trimmed().toLower();
    QVariantMap m;
    m.insert(QStringLiteral("path"), path);
    const QFileInfo fi(path);
    m.insert(QStringLiteral("displayName"), fi.fileName().isEmpty() ? path : fi.fileName());
    m.insert(QStringLiteral("isPersonal"), r.is_personal());
    m.insert(QStringLiteral("scopeGroupId"), scopeId);
    m.insert(QStringLiteral("scopeLabel"), scopeLabelForGroupId(scopeId));
    m.insert(QStringLiteral("fileCount"),
             service_.file_count_in_root(r.path, files_ui_.file_scope_group_id_.toStdString()));
    m.insert(QStringLiteral("canRemove"), canRemoveShareRoot(r));
    files_ui_.file_share_roots_.append(m);
  }

  if (!files_ui_.file_selected_share_root_.isEmpty()) {
    bool found = false;
    for (const QVariant& v : files_ui_.file_share_roots_) {
      const QString rp = v.toMap().value(QStringLiteral("path")).toString();
      if (shareRootPathsEqual(rp, files_ui_.file_selected_share_root_)) {
        files_ui_.file_selected_share_root_ = rp;
        found = true;
        break;
      }
    }
    if (!found)
      files_ui_.file_selected_share_root_.clear();
  }
  if (files_ui_.file_selected_share_root_.isEmpty() && !files_ui_.file_share_roots_.isEmpty()) {
    files_ui_.file_selected_share_root_ =
        files_ui_.file_share_roots_.first().toMap().value(QStringLiteral("path")).toString();
    resetFileBrowse();
  }
}

void NodeController::refreshLocalFileModel() {
  if (files_ui_.file_selected_share_root_.isEmpty()) {

    const auto all = service_.local_files_for_scope(files_ui_.file_scope_group_id_.toStdString());
    const std::string objects_prefix = nyx::normalize_utf8_path(nyx::data_dir() + "/objects") + "/";
    const std::string library_prefix =
        nyx::normalize_utf8_path(nyx::FileIndex::library_root_path([&] {
          nyx::GroupId scope {};
          if (!files_ui_.file_scope_group_id_.isEmpty()) {
            nyx::GroupStore::group_id_from_hex(files_ui_.file_scope_group_id_.toStdString(), scope);
          }
          return scope;
        }()));
    std::vector<nyx::FileEntry> managed;
    managed.reserve(all.size());
    for (const auto& e : all) {
      if (e.is_directory())
        continue;
      const std::string root = nyx::normalize_utf8_path(e.root_path);
      if (root.rfind(objects_prefix, 0) == 0 || root == library_prefix ||
          root.rfind(library_prefix + "/", 0) == 0) {
        managed.push_back(e);
      }
    }
    files_ui_.local_file_list_ = entriesToVariant(managed, false);
    return;
  }
  std::string root_path = files_ui_.file_selected_share_root_.toStdString();
  for (const auto& r : service_.all_share_roots()) {
    if (shareRootPathsEqual(files_ui_.file_selected_share_root_, QString::fromStdString(r.path))) {
      root_path = r.path;
      break;
    }
  }
  const auto entries = service_.local_files_at_root(root_path,
                                                    files_ui_.file_browse_path_.toStdString(),
                                                    files_ui_.file_scope_group_id_.toStdString());
  files_ui_.local_file_list_ = entriesToVariant(entries, false);
}

void NodeController::reconcileRemoteBrowsePath(const std::vector<nyx::FileEntry>& catalog) {
  if (files_ui_.file_resources_root_.isEmpty())
    return;
  bool found = false;
  for (const auto& e : catalog) {
    if (e.root_path.empty())
      continue;
    if (shareRootPathsEqual(files_ui_.file_resources_root_, QString::fromStdString(e.root_path))) {
      found = true;
      break;
    }
  }
  if (!found) {
    for (const auto& e : remoteRootsCatalog(catalog)) {
      if (shareRootPathsEqual(files_ui_.file_resources_root_,
                              QString::fromStdString(e.root_path))) {
        found = true;
        break;
      }
    }
  }
  if (!found) {
    files_ui_.file_resources_root_.clear();
    files_ui_.file_remote_browse_path_.clear();
  }
}

void NodeController::refreshRemoteFileModel() {
  refreshRemoteFileModel(service_.remote_files());
}

void NodeController::refreshRemoteFileModel(const std::vector<nyx::FileEntry>& entries) {
  reconcileRemoteBrowsePath(entries);
  std::vector<nyx::FileEntry> level;
  if (files_ui_.file_resources_root_.isEmpty()) {
    level = remoteRootsCatalog(entries);
  } else {
    std::string root_path = files_ui_.file_resources_root_.toStdString();
    for (const auto& e : entries) {
      if (shareRootPathsEqual(files_ui_.file_resources_root_,
                              QString::fromStdString(e.root_path))) {
        root_path = e.root_path;
        break;
      }
    }
    level = nyx::FileIndex::listing_level(
        entries, root_path, files_ui_.file_remote_browse_path_.toStdString());
  }
  files_ui_.remote_file_list_ = entriesToVariant(level, true);
  syncRemoteBrowseCrumbs();
}

void NodeController::refreshFileLists() {
  refreshFileShareRoots();
  refreshLocalFileModel();
  refreshRemoteFileModel();
  emit filesChanged();
}
