#include "node_controller.hpp"

#include "nyx/file_index.hpp"
#include "nyx/util.hpp"

#include <QDir>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <limits>

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
  const QString browse_rel = remote ? file_remote_browse_path_ : file_browse_path_;
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
  file_browse_path_.clear();
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
