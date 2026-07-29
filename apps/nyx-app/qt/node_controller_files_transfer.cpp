#include "node_controller.hpp"

#include "android_platform.hpp"
#include "host_env.hpp"
#include "node_controller_media.hpp"

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
#include <QMimeDatabase>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <limits>

void NodeController::refreshRemoteFileList() {

  files_ui_.file_resources_root_.clear();
  files_ui_.file_remote_browse_path_.clear();
  syncRemoteBrowseCrumbs();
  if (!service_.request_remote_files_at(files_ui_.file_scope_group_id_.toStdString(), {}, {})) {
    refreshRemoteFileModel();
    files_ui_.notifyFilesChanged();
    showToast(fileExchangeHint().isEmpty() ? QStringLiteral("Не удалось запросить файлы")
                                           : fileExchangeHint());
    return;
  }
  files_ui_.notifyFilesChanged();
}

void NodeController::downloadFile(const QString& hashHex,
                                  const QString& fileName,
                                  const QString& rootPath,
                                  const QString& relativePath) {
  if (!fileExchangeReady()) {
    showToast(fileExchangeHint().isEmpty()
                  ? QStringLiteral("Подключитесь к полю или чату для скачивания")
                  : fileExchangeHint());
    return;
  }
  if (!canFileDownloadAt(rootPath, relativePath)) {
    showToast(QStringLiteral("Нет права скачивать файлы"));
    return;
  }
  const QString hash = hashHex.trimmed();
  if (hash.size() != 64) {
    showToast(QStringLiteral("Неверный hash файла"));
    return;
  }
  const QString suggested =
      fileName.trimmed().isEmpty() ? QStringLiteral("download") : fileName.trimmed();
  const QString dest = pickSaveFile(suggested);
  if (dest.isEmpty())
    return;
  if (!service_.download_file(hash.toStdString(), dest.toStdString())) {
    showToast(QStringLiteral("Не удалось скачать файл"));
    return;
  }
  showToast(QStringLiteral("Скачивание…"));
}

void NodeController::downloadRemoteFolder(const QString& rootPath, const QString& relativePath) {
  if (!canFileDownloadAt(rootPath, relativePath)) {
    showToast(QStringLiteral("Нет права скачивать файлы"));
    return;
  }
  const QString destDir = pickSaveFolder();
  if (destDir.isEmpty())
    return;
  const QString canonical = resolveAccessRootPath(rootPath);
  std::string root = canonical.toStdString();
  for (const auto& e : service_.remote_files()) {
    if (!shareRootPathsEqual(canonical, QString::fromStdString(e.root_path)))
      continue;
    root = e.root_path;
    break;
  }
  const std::size_t queued =
      service_.enqueue_folder_downloads(root, relativePath.toStdString(), destDir.toStdString());
  if (queued == 0) {
    showToast(QStringLiteral("В папке нет файлов для скачивания"));
    return;
  }
  showToast(QStringLiteral("Скачивание %1 файлов…").arg(static_cast<qulonglong>(queued)));
}

void NodeController::sendFileByHash(const QString& hashHex) {
  if (!canFileUpload()) {
    showToast(QStringLiteral("Нет права отправлять файлы"));
    return;
  }
  if (!service_.send_file(hashHex.trimmed().toStdString())) {
    showToast(QStringLiteral("Не удалось отправить файл"));
  }
}

QString NodeController::fileLocalPath(const QString& hashHex) const {
  return mediaLocalPath(hashHex);
}

void NodeController::ensureFileAvailable(const QString& hashHex, const QString& fileName) {
  const QString hex = hashHex.trimmed().toLower();
  if (hex.size() != 64 || !fileLocalPath(hex).isEmpty())
    return;
  QString safe_name = QFileInfo(fileName).fileName();
  if (safe_name.isEmpty())
    safe_name = hex;
  const QString dir = QString::fromStdString(nyx::default_downloads_dir());
  QDir().mkpath(dir);
  const QString dest = QDir(dir).filePath(hex.left(12) + QLatin1Char('-') + safe_name);
  if (!service_.download_file(hex.toStdString(), dest.toStdString())) {
    showToast(QStringLiteral("Нет доступного источника файла"), true);
  }
}

QString NodeController::fileTextPreview(const QString& hashHex) const {
  const QString path = fileLocalPath(hashHex);
  if (path.isEmpty() || path.endsWith(QLatin1String(".part")))
    return {};

  if (!service_.find_file_object(hashHex.trimmed().toLower().toStdString()) &&
      !path.contains(QStringLiteral("/objects/")) && !path.contains(QStringLiteral("/library/")) &&
      !path.contains(QStringLiteral("/chat_media/")) &&
      !path.contains(QStringLiteral("/downloads/"))) {
    return {};
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  const QByteArray data = file.read(256 * 1024 + 1);
  if (data.size() > 256 * 1024 || data.contains('\0'))
    return {};
  return QString::fromUtf8(data);
}

bool NodeController::openLocalFile(const QString& path, const QString& mime) {
  const QString local = path.trimmed();
  if (local.isEmpty() || local.endsWith(QLatin1String(".part"))) {
    showToast(QStringLiteral("Файл ещё не готов"), true);
    return false;
  }
  if (!QFileInfo::exists(local)) {
    showToast(QStringLiteral("Файл не найден"), true);
    return false;
  }
  QString use_mime = mime.trimmed();
  if (use_mime.isEmpty()) {

    use_mime = QMimeDatabase().mimeTypeForFile(local, QMimeDatabase::MatchExtension).name();
  }
  if (use_mime.startsWith(QLatin1String("image/")) ||
      use_mime.startsWith(QLatin1String("audio/")) ||
      use_mime.startsWith(QLatin1String("video/"))) {
    openInAppMedia(local, use_mime, QFileInfo(local).fileName());
    return true;
  }
  if (DocumentViewer::canHandle(local, use_mime)) {
    closeInAppMedia();
    return document_viewer_.openDocument(local, use_mime, QFileInfo(local).fileName());
  }
#if defined(Q_OS_ANDROID)
  if (nyx_android::open_file(local, use_mime))
    return true;
  showToast(QStringLiteral("Не удалось открыть файл"), true);
  return false;
#elif defined(Q_OS_LINUX)
  const QString abs = QFileInfo(local).absoluteFilePath();
  const QProcessEnvironment env = nyx_app::host_process_environment();

  auto launch = [&](const QString& program, const QStringList& args) -> bool {
    QProcess proc;
    proc.setProcessEnvironment(env);
    proc.setProgram(program);
    proc.setArguments(args);
    proc.setWorkingDirectory(QFileInfo(abs).absolutePath());
    return proc.startDetached();
  };
  if (launch(QStringLiteral("/usr/bin/xdg-open"), {abs}))
    return true;
  if (launch(QStringLiteral("xdg-open"), {abs}))
    return true;
  if (launch(QStringLiteral("/usr/bin/gio"), {QStringLiteral("open"), abs}))
    return true;

  if (QDesktopServices::openUrl(QUrl::fromLocalFile(abs)))
    return true;
  showToast(QStringLiteral("Не удалось открыть файл"), true);
  return false;
#else
  if (QDesktopServices::openUrl(QUrl::fromLocalFile(local)))
    return true;
  showToast(QStringLiteral("Не удалось открыть файл"), true);
  return false;
#endif
}

void NodeController::openFileByHash(const QString& hashHex,
                                    const QString& fileName,
                                    const QString& mime,
                                    const QString& rootPath,
                                    const QString& relativePath) {
  const QString hex = hashHex.trimmed().toLower();
  if (hex.size() != 64) {
    showToast(QStringLiteral("Некорректный hash файла"), true);
    return;
  }
  QString path = fileLocalPath(hex);

  if (path.isEmpty() && !rootPath.trimmed().isEmpty() && !relativePath.trimmed().isEmpty()) {
    const QString candidate = QDir(rootPath.trimmed()).filePath(relativePath.trimmed());
    if (QFileInfo::exists(candidate) && QFileInfo(candidate).isFile()) {
      path = candidate;
    }
  }
  if (path.isEmpty()) {
    ensureFileAvailable(hex, fileName);
    showToast(QStringLiteral("Файл загружается — откроется, когда будет готов"));

    QTimer::singleShot(1200, this, [this, hex, fileName, mime, rootPath, relativePath]() {
      QString ready = fileLocalPath(hex);
      if (ready.isEmpty() && !rootPath.trimmed().isEmpty() && !relativePath.trimmed().isEmpty()) {
        const QString candidate = QDir(rootPath.trimmed()).filePath(relativePath.trimmed());
        if (QFileInfo::exists(candidate) && QFileInfo(candidate).isFile())
          ready = candidate;
      }
      if (ready.isEmpty())
        return;
      QString use_mime = mime;
      if (use_mime.isEmpty()) {
        use_mime = QMimeDatabase().mimeTypeForFile(ready, QMimeDatabase::MatchExtension).name();
      }
      openLocalFile(ready, use_mime);
    });
    return;
  }
  QString use_mime = mime;
  if (use_mime.isEmpty()) {
    use_mime = QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchExtension).name();
  }
  openLocalFile(path, use_mime);
}

int NodeController::fileSyncState(const QString& hashHex) const {

  const QString hex = hashHex.trimmed().toLower();
  if (hex.size() != 64)
    return 0;
  if (!fileLocalPath(hex).isEmpty())
    return 2;
  for (const auto& item : files_ui_.transfer_queue_) {
    const QVariantMap m = item.toMap();
    if (m.value(QStringLiteral("hash")).toString() != hex)
      continue;
    const QString state = m.value(QStringLiteral("state")).toString();
    if (state == QLatin1String("active") || state == QLatin1String("queued")) {
      return 1;
    }
  }
  return 0;
}

void NodeController::linkFileToChat(const QString& hashHex,
                                    const QString& fileName,
                                    const QString& mime,
                                    qulonglong size) {
  const QString hash = hashHex.trimmed().toLower();
  if (hash.size() != 64) {
    showToast(QStringLiteral("Некорректный hash файла"), true);
    return;
  }
  QString name = fileName;
  name.replace(QLatin1Char(']'), QLatin1Char('_'));
  QString safe_mime = mime.trimmed();
  safe_mime.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9.+/_-]")));
  if (safe_mime.isEmpty()) {
    safe_mime = QStringLiteral("application/octet-stream");
  }
  const QString markdown = QStringLiteral("[%1](nyx-file:%2;size=%3;mime=%4)")
                               .arg(name, hash, QString::number(size), safe_mime);
  showChatView();
  QTimer::singleShot(0, this, [this, markdown]() { emit fileLinkReady(markdown); });
}

void NodeController::linkFolderToChat(const QString& hashHex,
                                      const QString& folderName,
                                      const QString& rootPath,
                                      const QString& relativePath,
                                      qulonglong size) {
  const QString hash = hashHex.trimmed().toLower();
  if (hash.size() != 64) {
    showToast(QStringLiteral("Некорректный hash папки"), true);
    return;
  }
  QString name = folderName.trimmed();
  name.replace(QLatin1Char(']'), QLatin1Char('_'));
  if (name.isEmpty())
    name = QStringLiteral("Папка");
  QString root = rootPath.trimmed();
  root.replace(QLatin1Char(')'), QLatin1Char('_'));
  QString rel = relativePath.trimmed();
  rel.replace(QLatin1Char(')'), QLatin1Char('_'));
  const QString markdown =
      QStringLiteral("[%1](nyx-file:%2;size=%3;mime=application/x-nyx-directory;root=%4;rel=%5)")
          .arg(name,
               hash,
               QString::number(size),
               QString::fromUtf8(root.toUtf8().toPercentEncoding()),
               QString::fromUtf8(rel.toUtf8().toPercentEncoding()));
  showChatView();
  QTimer::singleShot(0, this, [this, markdown]() { emit fileLinkReady(markdown); });
}

void NodeController::openFolderInResources(const QString& hashHex,
                                           const QString& rootPath,
                                           const QString& relativePath) {
  setMainViewMode(1);
  setFilesSection(1);
  QString root = QUrl::fromPercentEncoding(rootPath.toUtf8()).trimmed();
  QString rel = QUrl::fromPercentEncoding(relativePath.toUtf8()).trimmed();
  if (root.isEmpty()) {
    const QString hex = hashHex.trimmed().toLower();
    for (const auto& e : service_.remote_files()) {
      if (nyx::hash_hex(e.hash) != hex.toStdString())
        continue;
      root = QString::fromStdString(e.root_path);
      rel = QString::fromStdString(e.relative_path);
      break;
    }
    if (root.isEmpty()) {
      for (const auto& e :
           service_.local_files_for_scope(files_ui_.file_scope_group_id_.toStdString())) {
        if (nyx::hash_hex(e.hash) != hex.toStdString())
          continue;
        root = QString::fromStdString(e.root_path);
        rel = QString::fromStdString(e.relative_path);
        break;
      }
    }
  }
  if (root.isEmpty()) {
    showToast(QStringLiteral("Папка не найдена в ресурсах"), true);
    return;
  }
  files_ui_.file_resources_root_ = root;

  files_ui_.file_remote_browse_path_ = rel;
  if (fileExchangeReady()) {
    service_.request_remote_files_at(
        files_ui_.file_scope_group_id_.toStdString(), root.toStdString(), rel.toStdString());
  }
  refreshRemoteFileModel();
  files_ui_.notifyFilesChanged();
}

void NodeController::pauseFileTransfer(const QString& hashHex, bool paused) {
  service_.pause_transfer(hashHex.toStdString(), paused);
}

void NodeController::cancelFileTransfer(const QString& hashHex) {
  service_.cancel_transfer(hashHex.toStdString());
}

void NodeController::retryFileTransfer(const QString& hashHex) {
  service_.retry_transfer(hashHex.toStdString());
}

void NodeController::moveFileTransfer(const QString& hashHex, int delta) {
  service_.move_transfer(hashHex.toStdString(), delta);
}

void NodeController::importFiles() {
  const QList<QUrl> urls = QFileDialog::getOpenFileUrls(
      nullptr, QStringLiteral("Импортировать файлы"), QUrl(), QStringLiteral("Все файлы (*.*)"));
  if (urls.isEmpty())
    return;
  int imported = 0;
  for (const QUrl& url : urls) {
    QString source;
    QString name;
    QString mime = QStringLiteral("application/octet-stream");
    bool temporary = false;
#if defined(Q_OS_ANDROID)
    if (url.scheme() == QLatin1String("content")) {
      const auto info = nyx_android::storage_document_info(url.toString());
      name = QFileInfo(info.name).fileName();
      if (name.isEmpty())
        name = QStringLiteral("imported-file");
      mime = info.mime;
      const QString staging = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                              QStringLiteral("/import-staging");
      QDir().mkpath(staging);
      source = QDir(staging).filePath(QString::number(QDateTime::currentMSecsSinceEpoch()) +
                                      QLatin1Char('-') + name);
      if (!nyx_android::copy_content_uri(url.toString(), source))
        continue;
      temporary = true;
    } else
#endif
    {
      source = url.toLocalFile();
      name = QFileInfo(source).fileName();
      mime = QMimeDatabase().mimeTypeForFile(source).name();
    }
    const auto object = service_.import_file_object(source.toStdString(),
                                                    name.toStdString(),
                                                    mime.toStdString(),
                                                    files_ui_.file_scope_group_id_.toStdString());
    if (temporary)
      QFile::remove(source);
    if (object)
      ++imported;
  }
  refreshFileLists();
  showToast(QStringLiteral("Импортировано файлов: %1").arg(imported), imported == 0);
}

void NodeController::exportFile(const QString& hashHex,
                                const QString& fileName,
                                const QString& mime) {
  Q_UNUSED(mime);
  const QString source = fileLocalPath(hashHex);
  if (source.isEmpty()) {
    showToast(QStringLiteral("Файл ещё не загружен"), true);
    return;
  }
#if defined(Q_OS_ANDROID)
  if (!nyx_android::export_file(source, fileName, mime)) {
    showToast(QStringLiteral("Не удалось экспортировать файл"), true);
  }
#else
  const QString destination =
      QFileDialog::getSaveFileName(nullptr, QStringLiteral("Экспортировать файл"), fileName);
  if (destination.isEmpty())
    return;
  QFile::remove(destination);
  if (!QFile::copy(source, destination)) {
    showToast(QStringLiteral("Не удалось экспортировать файл"), true);
  }
#endif
}
