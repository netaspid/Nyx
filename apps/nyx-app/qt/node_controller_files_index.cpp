#include "node_controller.hpp"

#include "android_platform.hpp"
#include "host_env.hpp"

#include "nyx/file_index.hpp"
#include "nyx/group.hpp"
#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMetaObject>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <thread>

QString NodeController::pickFolder() {
#if defined(Q_OS_ANDROID)

  const QString base =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/shares");
  QDir().mkpath(base);
  bool ok = false;
  const QString name = QInputDialog::getText(
      nullptr,
      QStringLiteral("Папка для обмена"),
      QStringLiteral("Создайте папку в хранилище Nyx и выберите файлы "
                     "(Documents, Downloads и т.п.) для копирования в неё.\n\n"
                     "Имя папки:"),
      QLineEdit::Normal,
      QStringLiteral("Documents"),
      &ok);
  if (!ok)
    return {};
  QString clean = name.trimmed();
  clean.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));
  if (clean.isEmpty())
    clean = QStringLiteral("shared");
  const QString path = QDir(base).filePath(clean);
  if (!QDir().mkpath(path)) {
    showToast(QStringLiteral("Не удалось создать папку"), true);
    return {};
  }

  const QList<QUrl> urls =
      QFileDialog::getOpenFileUrls(nullptr,
                                   QStringLiteral("Файлы в папку «%1»").arg(clean),
                                   QUrl(),
                                   QStringLiteral("Все файлы (*.*)"));
  int copied = 0;
  for (const QUrl& url : urls) {
    QString source;
    QString file_name;
    bool temporary = false;
    if (url.scheme() == QLatin1String("content")) {
      const auto info = nyx_android::storage_document_info(url.toString());
      file_name = QFileInfo(info.name).fileName();
      if (file_name.isEmpty()) {
        file_name = QStringLiteral("file-%1").arg(++copied);
      }
      source = QDir(path).filePath(QString::number(QDateTime::currentMSecsSinceEpoch()) +
                                   QLatin1Char('-') + file_name);
      if (!nyx_android::copy_content_uri(url.toString(), source))
        continue;

      const QString final_path = QDir(path).filePath(file_name);
      if (final_path != source) {
        QFile::remove(final_path);
        QFile::rename(source, final_path);
      }
      ++copied;
      continue;
    }
    source = url.toLocalFile();
    file_name = QFileInfo(source).fileName();
    if (file_name.isEmpty() || source.isEmpty())
      continue;
    const QString dest = QDir(path).filePath(file_name);
    QFile::remove(dest);
    if (QFile::copy(source, dest))
      ++copied;
  }
  showToast(copied > 0 ? QStringLiteral("В папку скопировано файлов: %1").arg(copied)
                       : QStringLiteral("Папка создана — можно добавить файлы позже"));
  return path;
#else
  const QString dir = QFileDialog::getExistingDirectory(
      nullptr, QStringLiteral("Выберите папку для индекса"), QDir::homePath());
  return dir;
#endif
}

QString NodeController::pickSaveFile(const QString& suggestedFileName) {
  const QString name = suggestedFileName.trimmed().isEmpty() ? QStringLiteral("download")
                                                             : suggestedFileName.trimmed();
#if defined(Q_OS_ANDROID)
  const QString downloads = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                            QStringLiteral("/downloads");
  QDir().mkpath(downloads);
  return QDir(downloads).filePath(name);
#else
  const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  const QString base = downloads.isEmpty() ? QDir::homePath() : downloads;
  const QString suggested = QDir(base).filePath(name);
  return QFileDialog::getSaveFileName(
      nullptr, QStringLiteral("Сохранить файл"), suggested, QStringLiteral("Все файлы (*.*)"));
#endif
}

QString NodeController::pickSaveFolder() {
#if defined(Q_OS_ANDROID)
  const QString downloads = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                            QStringLiteral("/downloads");
  QDir().mkpath(downloads);
  return downloads;
#else
  const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  const QString start = downloads.isEmpty() ? QDir::homePath() : downloads;
  return QFileDialog::getExistingDirectory(
      nullptr, QStringLiteral("Выберите папку для сохранения"), start);
#endif
}

void NodeController::runIndexJob(const QString& path, const QString& scopeGroupId, bool rescan) {
  const QString p = path.trimmed();
  if (p.isEmpty())
    return;
  bool expected = false;
  if (!files_ui_.file_index_busy_.compare_exchange_strong(expected, true)) {
    showToast(QStringLiteral("Индексация уже выполняется"));
    return;
  }

  files_ui_.file_index_progress_visible_ = true;
  files_ui_.file_index_progress_percent_ = 5;
  files_ui_.file_index_progress_label_ = QStringLiteral("Подготовка сканирования…");
  files_ui_.file_index_files_scanned_ = 0;
  files_ui_.notifyFileIndexProgressChanged();

  const QString scope = scopeGroupId;
  if (files_ui_.file_index_thread_.joinable())
    files_ui_.file_index_thread_.join();
  files_ui_.file_index_thread_ = std::thread([this, p, scope, rescan]() {
    const bool ok = rescan ? service_.rescan_share_root(p.toStdString(), scope.toStdString())
                           : service_.index_folder(p.toStdString(), scope.toStdString());
    const int count = ok ? service_.file_count_in_root(p.toStdString(), scope.toStdString()) : 0;
    QMetaObject::invokeMethod(
        this,
        [this, ok, count, p, rescan]() {
          files_ui_.file_index_busy_.store(false);
          files_ui_.file_index_progress_visible_ = true;
          files_ui_.file_index_progress_percent_ = 100;
          if (!ok) {
            files_ui_.file_index_progress_label_ = QStringLiteral("Ошибка индексации");
            showToast(status_text_.isEmpty()
                          ? (rescan ? QStringLiteral("Не удалось переиндексировать")
                                    : QStringLiteral("Не удалось проиндексировать папку"))
                          : status_text_);
          } else {
            files_ui_.file_index_progress_label_ = QStringLiteral("Готово: %1 файлов").arg(count);
            refreshFileLists();
            if (!rescan) {
              setFileSelectedShareRoot(p);
              if (count == 0) {
                showToast(QStringLiteral(
                    "Папка добавлена (0 файлов). Положите файлы и нажмите «Переиндексировать»."));
              } else {
                showToast(QStringLiteral("Папка проиндексирована: %1 файлов").arg(count));
              }
            } else {
              showToast(QStringLiteral("Переиндексировано: %1 файлов").arg(count));
            }
          }
          files_ui_.notifyFileIndexProgressChanged();
          QTimer::singleShot(1400, this, [this]() {
            if (!files_ui_.file_index_busy_.load()) {
              files_ui_.file_index_progress_visible_ = false;
              files_ui_.notifyFileIndexProgressChanged();
            }
          });
        },
        Qt::QueuedConnection);
  });
}

void NodeController::addIndexedFolder(const QString& path) {
  if (!canAddShareFolder()) {
    showToast(QStringLiteral("Нет права добавлять папки в эту область"));
    return;
  }
  QString p = path.trimmed();
  if (p.isEmpty()) {
    p = pickFolder();
    if (p.isEmpty())
      return;
  }
  if (p.startsWith(QStringLiteral("file:///")))
    p = QUrl(p).toLocalFile();
  runIndexJob(p, files_ui_.file_scope_group_id_, false);
}

void NodeController::removeIndexedFolder(const QString& path) {
  if (path.trimmed().isEmpty())
    return;

  QString scope = files_ui_.file_scope_group_id_;
  for (const auto& r : service_.all_share_roots()) {
    if (!shareRootPathsEqual(path, QString::fromStdString(r.path)))
      continue;
    const QString root_scope =
        r.is_personal()
            ? QString()
            : QString::fromStdString(nyx::FileIndex::group_id_hex(r.group_id)).trimmed().toLower();
    if (root_scope != scope)
      continue;
    if (!canRemoveShareRoot(r)) {
      showToast(QStringLiteral("Нет права убирать эту папку"));
      return;
    }
    break;
  }
  if (!service_.remove_share_root(path.toStdString(), scope.toStdString())) {
    showToast(status_text_.isEmpty() ? QStringLiteral("Не удалось убрать папку") : status_text_);
    return;
  }
  if (shareRootPathsEqual(files_ui_.file_selected_share_root_, path)) {
    files_ui_.file_selected_share_root_.clear();
    resetFileBrowse();
    service_.save_files_selected_root({});
  }
  if (shareRootPathsEqual(files_ui_.file_resources_root_, path)) {
    files_ui_.file_resources_root_.clear();
    files_ui_.file_remote_browse_path_.clear();
  }
  refreshFileLists();
  if (files_ui_.files_section_ == 1 && fileExchangeReady())
    refreshRemoteFileList();
  showToast(QStringLiteral("Папка убрана из индекса"));
}

void NodeController::rescanIndexedFolder(const QString& path) {
  if (path.trimmed().isEmpty())
    return;
  QString scope = files_ui_.file_scope_group_id_;
  for (const auto& r : service_.all_share_roots()) {
    if (!shareRootPathsEqual(path, QString::fromStdString(r.path)))
      continue;
    const QString root_scope =
        r.is_personal()
            ? QString()
            : QString::fromStdString(nyx::FileIndex::group_id_hex(r.group_id)).trimmed().toLower();
    if (root_scope != scope)
      continue;
    break;
  }
  runIndexJob(path.trimmed(), scope, true);
}
