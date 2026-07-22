#include "android_platform.hpp"

#if defined(Q_OS_ANDROID)

#include <QCameraPermission>
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QMicrophonePermission>
#include <QPermissions>
#include <QtCore/qnativeinterface.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace nyx_android {
namespace {

std::mutex g_lock_mutex;
QJniObject g_multicast_lock;
bool g_multicast_held = false;

QJniObject android_context() {
  return QNativeInterface::QAndroidApplication::context();
}

struct PermState {
  int pending = 0;
  bool ok = true;
  void (*done)(bool, void*) = nullptr;
  void* user = nullptr;
};

void finish_one(PermState* s, bool granted) {
  s->ok = s->ok && granted;
  if (--s->pending > 0) return;
  auto* done = s->done;
  void* user = s->user;
  const bool ok = s->ok;
  delete s;
  done(ok, user);
}

}  // namespace

void acquire_multicast_lock() {
  std::lock_guard lock(g_lock_mutex);
  if (g_multicast_held) return;
  QJniObject ctx = android_context();
  if (!ctx.isValid()) return;
  QJniObject wifi_service = QJniObject::getStaticObjectField(
      "android/content/Context", "WIFI_SERVICE", "Ljava/lang/String;");
  QJniObject wifi = ctx.callObjectMethod(
      "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", wifi_service.object<jstring>());
  if (!wifi.isValid()) return;
  QJniObject name = QJniObject::fromString(QStringLiteral("nyx-discovery"));
  QJniObject mlock = wifi.callObjectMethod(
      "createMulticastLock", "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;",
      name.object<jstring>());
  if (!mlock.isValid()) return;
  mlock.callMethod<void>("setReferenceCounted", "(Z)V", jboolean(false));
  mlock.callMethod<void>("acquire");
  g_multicast_lock = mlock;
  g_multicast_held = true;
}

void release_multicast_lock() {
  std::lock_guard lock(g_lock_mutex);
  if (!g_multicast_held || !g_multicast_lock.isValid()) return;
  g_multicast_lock.callMethod<void>("release");
  g_multicast_lock = QJniObject();
  g_multicast_held = false;
}

std::string wifi_ipv4() {
  QJniObject ctx = android_context();
  if (!ctx.isValid()) return {};
  QJniObject wifi_service = QJniObject::getStaticObjectField(
      "android/content/Context", "WIFI_SERVICE", "Ljava/lang/String;");
  QJniObject wifi = ctx.callObjectMethod(
      "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", wifi_service.object<jstring>());
  if (!wifi.isValid()) return {};
  QJniObject dhcp = wifi.callObjectMethod("getDhcpInfo", "()Landroid/net/DhcpInfo;");
  if (dhcp.isValid()) {
    const jint ip = dhcp.getField<jint>("ipAddress");
    if (ip != 0) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                    static_cast<unsigned>(ip) & 0xffu,
                    (static_cast<unsigned>(ip) >> 8) & 0xffu,
                    (static_cast<unsigned>(ip) >> 16) & 0xffu,
                    (static_cast<unsigned>(ip) >> 24) & 0xffu);
      return buf;
    }
  }
  QJniObject info = wifi.callObjectMethod("getConnectionInfo", "()Landroid/net/wifi/WifiInfo;");
  if (!info.isValid()) return {};
  const jint ip = info.callMethod<jint>("getIpAddress");
  if (ip == 0) return {};
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                static_cast<unsigned>(ip) & 0xffu,
                (static_cast<unsigned>(ip) >> 8) & 0xffu,
                (static_cast<unsigned>(ip) >> 16) & 0xffu,
                (static_cast<unsigned>(ip) >> 24) & 0xffu);
  return buf;
}

void request_call_permissions(bool need_camera, void (*done)(bool granted, void* ctx), void* ctx) {
  auto* s = new PermState{need_camera ? 2 : 1, true, done, ctx};

  QMicrophonePermission mic;
  if (qApp->checkPermission(mic) == Qt::PermissionStatus::Granted) {
    finish_one(s, true);
  } else {
    qApp->requestPermission(mic, qApp, [s](const QPermission& perm) {
      finish_one(s, perm.status() == Qt::PermissionStatus::Granted);
    });
  }

  if (!need_camera) return;

  QCameraPermission cam;
  if (qApp->checkPermission(cam) == Qt::PermissionStatus::Granted) {
    finish_one(s, true);
  } else {
    qApp->requestPermission(cam, qApp, [s](const QPermission& perm) {
      finish_one(s, perm.status() == Qt::PermissionStatus::Granted);
    });
  }
}

void request_notification_permission() {
  QJniObject ctx = android_context();
  if (!ctx.isValid()) return;
  QJniObject::callStaticMethod<void>("org/nyx/app/NyxCallNotify", "ensureChannel",
                                     "(Landroid/content/Context;)V", ctx.object<jobject>());

  if (QNativeInterface::QAndroidApplication::sdkVersion() < 33) return;

  QJniObject perm = QJniObject::fromString(QStringLiteral("android.permission.POST_NOTIFICATIONS"));
  QJniEnvironment env;
  jclass stringClass = env.findClass("java/lang/String");
  if (!stringClass) return;
  jobjectArray arr = env->NewObjectArray(1, stringClass, nullptr);
  if (!arr) return;
  env->SetObjectArrayElement(arr, 0, perm.object<jstring>());
  ctx.callMethod<void>("requestPermissions", "([Ljava/lang/String;I)V", arr, jint(7103));
}

void set_voip_audio_mode(bool active) {
  QJniObject ctx = android_context();
  if (!ctx.isValid()) return;
  QJniObject::callStaticMethod<void>("org/nyx/app/NyxCallNotify", "setVoipAudioMode",
                                     "(Landroid/content/Context;Z)V", ctx.object<jobject>(),
                                     jboolean(active));
}

void acquire_call_wake_lock() {
  QJniObject ctx = android_context();
  if (!ctx.isValid()) return;
  QJniObject::callStaticMethod<void>("org/nyx/app/NyxCallNotify", "acquireWakeLock",
                                     "(Landroid/content/Context;)V", ctx.object<jobject>());
}

void release_call_wake_lock() {
  QJniObject::callStaticMethod<void>("org/nyx/app/NyxCallNotify", "releaseWakeLock", "()V");
}

void show_incoming_call_notification(const std::string& peer_title) {
  QJniObject ctx = android_context();
  if (!ctx.isValid()) return;
  const QString title = QString::fromStdString(peer_title).isEmpty()
                            ? QStringLiteral("Nyx")
                            : QString::fromStdString(peer_title);
  QJniObject jtitle = QJniObject::fromString(title);
  QJniObject jbody = QJniObject::fromString(QStringLiteral("Входящий звонок — нажмите, чтобы ответить"));
  QJniObject::callStaticMethod<void>(
      "org/nyx/app/NyxCallNotify", "showIncoming",
      "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)V", ctx.object<jobject>(),
      jtitle.object<jstring>(), jbody.object<jstring>());
}

void show_active_call_notification(const std::string& peer_title, bool video) {
  QJniObject ctx = android_context();
  if (!ctx.isValid()) return;
  const QString title = QString::fromStdString(peer_title).isEmpty()
                            ? QStringLiteral("Nyx")
                            : QString::fromStdString(peer_title);
  QJniObject jtitle = QJniObject::fromString(title);
  QJniObject jbody = QJniObject::fromString(video ? QStringLiteral("Видеозвонок")
                                                   : QStringLiteral("Аудиозвонок"));
  QJniObject::callStaticMethod<void>(
      "org/nyx/app/NyxCallNotify", "showActive",
      "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)V", ctx.object<jobject>(),
      jtitle.object<jstring>(), jbody.object<jstring>());
}

void cancel_call_notifications() {
  QJniObject ctx = android_context();
  if (!ctx.isValid()) return;
  QJniObject::callStaticMethod<void>("org/nyx/app/NyxCallNotify", "cancelAll",
                                     "(Landroid/content/Context;)V", ctx.object<jobject>());
}

void bring_app_to_foreground() {
  QJniObject ctx = android_context();
  if (!ctx.isValid()) return;
  QJniObject intent("android/content/Intent", "()V");
  QJniObject activity = QJniObject::fromString(
      QStringLiteral("org.qtproject.qt.android.bindings.QtActivity"));
  intent.callObjectMethod(
      "setClassName",
      "(Landroid/content/Context;Ljava/lang/String;)Landroid/content/Intent;", ctx.object<jobject>(),
      activity.object<jstring>());
  intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;",
                          jint(0x10000000));  // FLAG_ACTIVITY_NEW_TASK
  intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;",
                          jint(0x20000000));  // FLAG_ACTIVITY_SINGLE_TOP
  ctx.callMethod<void>("startActivity", "(Landroid/content/Intent;)V", intent.object<jobject>());
}

}  // namespace nyx_android

#else

namespace nyx_android {

void acquire_multicast_lock() {}
void release_multicast_lock() {}
std::string wifi_ipv4() { return {}; }

void request_call_permissions(bool /*need_camera*/, void (*done)(bool granted, void* ctx), void* ctx) {
  done(true, ctx);
}

void request_notification_permission() {}
void set_voip_audio_mode(bool) {}
void acquire_call_wake_lock() {}
void release_call_wake_lock() {}
void show_incoming_call_notification(const std::string&) {}
void show_active_call_notification(const std::string&, bool) {}
void cancel_call_notifications() {}
void bring_app_to_foreground() {}

}  // namespace nyx_android

#endif
