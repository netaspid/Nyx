#include "node_controller.hpp"

#include "android_platform.hpp"
#include "node_controller_media.hpp"

#include "nyx/file_hash.hpp"
#include "nyx/file_index.hpp"
#include "nyx/group.hpp"
#include "nyx/identity.hpp"
#include "nyx/markdown_format.hpp"
#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <limits>

QString NodeController::chatMediaDir() {
  return QString::fromStdString(nyx::data_dir() + "/chat_media");
}

QString NodeController::chatCaptureStagingPath(const QString& extension) const {
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                      QStringLiteral("/chat-capture");
  QDir().mkpath(dir);
  QString ext = extension.trimmed().toLower();
  if (ext.startsWith(QLatin1Char('.')))
    ext = ext.mid(1);
  if (ext.isEmpty())
    ext = QStringLiteral("bin");
  return QDir(dir).filePath(
      QStringLiteral("cap-%1.%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(ext));
}

void NodeController::removeStagingMedia(const QString& path) const {
  const QString staging = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                               QStringLiteral("/chat-capture"))
                              .canonicalPath();
  const QFileInfo info(path);
  const QString parent = info.dir().canonicalPath();
  if (!staging.isEmpty() && parent == staging)
    QFile::remove(info.absoluteFilePath());
}

void NodeController::requestChatCapturePermissions(bool needCamera) {
  nyx_android::request_call_permissions(
      needCamera, &NodeController::chatCapturePermissionCallback, this);
}

void NodeController::chatCapturePermissionCallback(bool micOk, bool cameraOk, void* ctx) {
  auto* self = static_cast<NodeController*>(ctx);
  if (!self)
    return;
  QMetaObject::invokeMethod(
      self,
      [self, micOk, cameraOk]() { emit self->chatCapturePermissionResult(micOk && cameraOk); },
      Qt::QueuedConnection);
}

QString NodeController::userDisplayName(const QString& userIdHex) const {
  const QString uid = userIdHex.trimmed().toLower();
  if (uid.isEmpty())
    return {};
  if (uid == profile_user_id_hex_.trimmed().toLower()) {
    const QString nick = profile_nickname_.trimmed();
    return nick.isEmpty() ? QStringLiteral("Вы") : nick;
  }
  for (const QVariant& v : contact_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("userId")).toString().trimmed().toLower() == uid) {
      const QString nick = m.value(QStringLiteral("nickname")).toString().trimmed();
      if (!nick.isEmpty())
        return nick;
      break;
    }
  }
  for (const QVariant& v : group_list_) {
    const QVariantMap g = v.toMap();
    const QVariantList members = g.value(QStringLiteral("members")).toList();
    for (const QVariant& mv : members) {
      const QVariantMap m = mv.toMap();
      if (m.value(QStringLiteral("userId")).toString().trimmed().toLower() != uid)
        continue;
      const QString nick = m.value(QStringLiteral("nickname")).toString().trimmed();
      if (!nick.isEmpty())
        return nick;
    }
  }
  return uid.left(8) + QStringLiteral("…");
}

void NodeController::openInAppMedia(const QString& path,
                                    const QString& mime,
                                    const QString& title) {
  if (path.trimmed().isEmpty() || !QFileInfo::exists(path)) {
    showToast(QStringLiteral("Файл ещё не загружен"), true);
    return;
  }
  document_viewer_.close();
  files_ui_.in_app_media_path_ = path;
  files_ui_.in_app_media_mime_ = mime;
  files_ui_.in_app_media_title_ = title;
  files_ui_.in_app_media_open_ = true;
  emit inAppMediaChanged();
}

void NodeController::closeInAppMedia() {
  if (!files_ui_.in_app_media_open_ && files_ui_.in_app_media_path_.isEmpty())
    return;
  files_ui_.in_app_media_open_ = false;
  files_ui_.in_app_media_path_.clear();
  files_ui_.in_app_media_mime_.clear();
  files_ui_.in_app_media_title_.clear();
  emit inAppMediaChanged();
}

void NodeController::ensureChatMediaRootIndexed() {
  const QString dir = chatMediaDir();
  QDir().mkpath(dir);
  QString scope;
  if (active_chat_kind_ == 1)
    scope = active_chat_ref_id_;
  service_.index_folder(dir.toStdString(), scope.toStdString());
}

QString NodeController::importChatMediaMarkdown(const QString& localPath,
                                                const QString& mimeHint,
                                                const QString& displayName) {
  const QString src = localPath.trimmed();
  if (src.isEmpty() || !QFileInfo::exists(src)) {
    showToast(QStringLiteral("Файл захвата не найден"), true);
    return {};
  }

  QString mime = mimeHint.trimmed();
  if (mime.isEmpty()) {
    mime = QMimeDatabase().mimeTypeForFile(src).name();
  }
  QString name = displayName.trimmed();
  if (name.isEmpty())
    name = QFileInfo(src).fileName();
  if (name.isEmpty())
    name = QStringLiteral("media");

  QString scope;
  if (active_chat_kind_ == 1)
    scope = active_chat_ref_id_;
  else if (!files_ui_.file_scope_group_id_.isEmpty())
    scope = files_ui_.file_scope_group_id_;

  const auto object = service_.import_file_object(src.toStdString(),
                                                  name.toStdString(),
                                                  mime.toStdString(),
                                                  scope.toStdString(),
                                                  profile_user_id_hex_.toStdString());
  if (!object) {
    showToast(QStringLiteral("Не удалось сохранить медиа в библиотеку"), true);
    return {};
  }

  const QString hex = QString::fromStdString(nyx::hash_hex(object->hash));
  QDir().mkpath(chatMediaDir());
  const QString ext = QFileInfo(name).suffix().toLower();
  const QString dest = chatMediaDir() + QLatin1Char('/') + hex +
                       (ext.isEmpty() ? QString {} : (QLatin1Char('.') + ext));
  if (QFile::exists(dest))
    QFile::remove(dest);
  QFile::copy(src, dest);
  ensureChatMediaRootIndexed();
  refreshFileLists();

  QString safe_name = name;
  safe_name.replace(QLatin1Char(']'), QLatin1Char('_'));
  QString safe_mime = mime;
  safe_mime.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9.+/_-]")));
  if (safe_mime.isEmpty())
    safe_mime = QStringLiteral("application/octet-stream");
  return QStringLiteral("[%1](nyx-file:%2;size=%3;mime=%4)")
      .arg(safe_name, hex, QString::number(static_cast<qulonglong>(object->size)), safe_mime);
}

bool NodeController::sendCapturedMedia(const QString& localPath,
                                       const QString& mimeHint,
                                       const QString& displayName,
                                       const QString& mediaKind) {
  if (!canSendMessage()) {
    showToast(QStringLiteral("Нет связи — медиа не отправлено"), true);
    return false;
  }
  const QString src = localPath.trimmed();
  const QFileInfo source_info(src);
  if (!source_info.isFile() || source_info.size() <= 0) {
    showToast(QStringLiteral("Запись не создана или пуста"), true);
    return false;
  }

  QString mime = mimeHint.trimmed();
  if (mime.isEmpty())
    mime = QMimeDatabase().mimeTypeForFile(src).name();
  QString name = displayName.trimmed();
  if (name.isEmpty())
    name = source_info.fileName();
  if (name.isEmpty())
    name = QStringLiteral("media");

  QString scope;
  if (active_chat_kind_ == 1)
    scope = active_chat_ref_id_;
  const QString chat_key = active_chat_key_;
  const QString owner = profile_user_id_hex_;
  const QString relative_dir =
      (mediaKind == QLatin1String("voice") || mediaKind == QLatin1String("circle"))
          ? nyxMediaRelativeDir(chat_key, peer_title_, mediaKind)
          : QString {};

  QPointer<NodeController> guard(this);
  QThreadPool::globalInstance()->start(
      [guard, src, mime, name, scope, owner, relative_dir, chat_key]() {
        if (!guard)
          return;
        const auto object = guard->service_.import_file_object(src.toStdString(),
                                                               name.toStdString(),
                                                               mime.toStdString(),
                                                               scope.toStdString(),
                                                               owner.toStdString(),
                                                               relative_dir.toStdString());
        QFile::remove(src);

        QString markdown;
        if (object) {
          QString safe_name = name;
          safe_name.replace(QLatin1Char(']'), QLatin1Char('_'));
          QString safe_mime = mime;
          safe_mime.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9.+/_-]")));
          if (safe_mime.isEmpty())
            safe_mime = QStringLiteral("application/octet-stream");
          markdown = QStringLiteral("[%1](nyx-file:%2;size=%3;mime=%4)")
                         .arg(safe_name,
                              QString::fromStdString(nyx::hash_hex(object->hash)),
                              QString::number(static_cast<qulonglong>(object->size)),
                              safe_mime);
        }

        QMetaObject::invokeMethod(
            guard,
            [guard, markdown, chat_key]() {
              if (!guard)
                return;
              guard->refreshFileLists();
              if (markdown.isEmpty()) {
                guard->showToast(QStringLiteral("Не удалось сохранить запись"), true);
                return;
              }
              if (guard->active_chat_key_ != chat_key || !guard->canSendMessage()) {
                guard->showToast(QStringLiteral("Запись сохранена в Файлах, но чат уже закрыт"),
                                 true);
                return;
              }
              guard->sendMessage(markdown);
            },
            Qt::QueuedConnection);
      });
  return true;
}

QString NodeController::mediaLocalPath(const QString& hashHex) const {
  const QString hex = hashHex.trimmed().toLower();
  if (hex.size() != 64)
    return {};

  auto is_verified_path = [&](const QString& path) -> bool {
    if (path.isEmpty() || path.endsWith(QLatin1String(".part")))
      return false;
    if (!QFileInfo::exists(path))
      return false;
    return true;
  };

  if (const auto object = service_.find_file_object(hex.toStdString())) {
    const QString path = QString::fromStdString(object->absolute_path());
    if (is_verified_path(path))
      return path;
  }

  const QDir dir(chatMediaDir());
  const QStringList matches = dir.entryList(
      QStringList {hex + QStringLiteral(".*"), hex.left(12) + QStringLiteral("-*")}, QDir::Files);
  for (const QString& name : matches) {
    const QString path = dir.filePath(name);
    if (is_verified_path(path))
      return path;
  }

  const QString dl = QString::fromStdString(nyx::default_downloads_dir());
  const QDir dld(dl);
  const QStringList dlm = dld.entryList(
      QStringList {hex + QStringLiteral(".*"), hex.left(12) + QStringLiteral("-*")}, QDir::Files);
  for (const QString& name : dlm) {
    const QString path = dld.filePath(name);
    if (is_verified_path(path))
      return path;
  }
  return {};
}

bool NodeController::isImageMedia(const QString& hashHex) const {
  const QString path = mediaLocalPath(hashHex);
  if (path.isEmpty())
    return true;
  const QString suf = QFileInfo(path).suffix().toLower();
  return suf == QLatin1String("jpg") || suf == QLatin1String("jpeg") ||
         suf == QLatin1String("png") || suf == QLatin1String("webp") ||
         suf == QLatin1String("gif") || suf == QLatin1String("bmp");
}

void NodeController::ensureMediaAvailable(const QString& hashHex) {
  const QString hex = hashHex.trimmed().toLower();
  if (hex.size() != 64)
    return;
  if (!mediaLocalPath(hex).isEmpty())
    return;
  const QString dest = chatMediaDir() + QLatin1Char('/') + hex;
  QDir().mkpath(chatMediaDir());
  if (!service_.download_file(hex.toStdString(), dest.toStdString())) {
    showToast(QStringLiteral("Не удалось запросить медиа"), true);
  }
}

QString NodeController::pickChatMediaMarkdown() {
  const QString src = QFileDialog::getOpenFileName(
      nullptr,
      QStringLiteral("Фото или видео в сообщение"),
      QString(),
      QStringLiteral("Media (*.png *.jpg *.jpeg *.webp *.gif *.bmp *.mp4 *.webm *.mov *.mkv *.m4a "
                     "*.ogg *.mp3 *.wav)"));
  if (src.isEmpty())
    return {};

  QString work = src;
  QString ext = QFileInfo(src).suffix().toLower();
  const bool is_image = ext == QLatin1String("jpg") || ext == QLatin1String("jpeg") ||
                        ext == QLatin1String("png") || ext == QLatin1String("webp") ||
                        ext == QLatin1String("gif") || ext == QLatin1String("bmp");
  QTemporaryFile tmp;
  if (is_image) {
    QImage img(src);
    if (img.isNull()) {
      showToast(QStringLiteral("Не удалось открыть изображение"), true);
      return {};
    }
    if (img.width() > 1600 || img.height() > 1600)
      img = img.scaled(1600, 1600, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    tmp.setFileTemplate(QDir::temp().filePath(QStringLiteral("nyx-media-XXXXXX.jpg")));
    tmp.setAutoRemove(false);
    if (!tmp.open()) {
      showToast(QStringLiteral("Не удалось сохранить медиа"), true);
      return {};
    }
    const QString tmp_path = tmp.fileName();
    tmp.close();
    if (!img.save(tmp_path, "JPG", 82)) {
      QFile::remove(tmp_path);
      showToast(QStringLiteral("Не удалось сжать изображение"), true);
      return {};
    }
    const qint64 sz = QFileInfo(tmp_path).size();
    if (sz > 2 * 1024 * 1024) {
      showToast(QStringLiteral("Изображение больше 2 МБ — лучше отправить файлом"), true);
    }
    work = tmp_path;
    ext = QStringLiteral("jpg");
  } else {
    const qint64 sz = QFileInfo(src).size();
    if (sz > 50 * 1024 * 1024) {
      showToast(QStringLiteral("Видео больше 50 МБ — отправьте через Файлы"), true);
      return {};
    }
  }

  const QString mime = QMimeDatabase().mimeTypeForFile(work).name();
  QString name = QFileInfo(src).fileName();
  if (is_image && !name.endsWith(QLatin1String(".jpg"), Qt::CaseInsensitive)) {
    name = QFileInfo(src).completeBaseName() + QStringLiteral(".jpg");
  }
  const QString md = importChatMediaMarkdown(work, mime, name);
  if (work != src)
    QFile::remove(work);
  return md;
}
