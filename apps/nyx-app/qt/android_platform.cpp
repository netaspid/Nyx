#include "android_platform.hpp"

#include <QtGlobal>

#if defined(Q_OS_ANDROID)

#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QPermissions>
#include <QtCore/qnativeinterface.h>

#include "nyx/log.hpp"

#include <cstdio>
#include <mutex>
#include <string>

// Native camera callbacks (JNI → Qt GUI). File scope for JNI exports.
static nyx_android::NativeCameraJpegFn g_cam_jpeg = nullptr;
static nyx_android::NativeCameraErrorFn g_cam_error = nullptr;
static nyx_android::NativeCameraStartedFn g_cam_started = nullptr;
static void* g_cam_ctx = nullptr;

namespace nyx_android {
namespace {

std::mutex g_lock_mutex;
QJniObject g_multicast_lock;
bool g_multicast_held = false;
bool g_speakerphone = true;
void (*g_hangup_fn)() = nullptr;

QJniObject android_context() {
  return QNativeInterface::QAndroidApplication::context();
}

struct PermState {
  int pending = 0;
  bool mic_ok = true;
  bool cam_ok = true;
  bool track_cam = false;
  void (*done)(bool, bool, void*) = nullptr;
  void* user = nullptr;
};

void finish_one(PermState* s, bool is_cam, bool granted) {
  if (is_cam)
    s->cam_ok = granted;
  else
    s->mic_ok = granted;
  if (--s->pending > 0) return;
  auto* done = s->done;
  void* user = s->user;
  const bool mic = s->mic_ok;
  const bool cam = s->track_cam ? s->cam_ok : true;
  delete s;
  done(mic, cam, user);
}

std::string ipv4_from_link_properties(const QJniObject& lp) {
  if (!lp.isValid()) return {};
  QJniObject addrs = lp.callObjectMethod("getLinkAddresses", "()Ljava/util/List;");
  if (!addrs.isValid()) return {};
  const jint n = addrs.callMethod<jint>("size");
  for (jint i = 0; i < n; ++i) {
    QJniObject la = addrs.callObjectMethod("get", "(I)Ljava/lang/Object;", i);
    if (!la.isValid()) continue;
    QJniObject inet = la.callObjectMethod("getAddress", "()Ljava/net/InetAddress;");
    if (!inet.isValid()) continue;
    if (inet.callMethod<jboolean>("isLoopbackAddress")) continue;
    // Prefer Inet4Address
    QJniEnvironment env;
    jclass v4 = env.findClass("java/net/Inet4Address");
    if (!v4 || !env->IsInstanceOf(inet.object<jobject>(), v4)) continue;
    QJniObject host = inet.callObjectMethod("getHostAddress", "()Ljava/lang/String;");
    if (!host.isValid()) continue;
    const QString s = host.toString();
    if (s.contains(QLatin1Char(':'))) continue;  // IPv6 literal
    return s.toStdString();
  }
  return {};
}

std::string wifi_ipv4_from_connectivity() {
  QJniObject ctx = android_context();
  if (!ctx.isValid()) return {};
  QJniObject cm_name = QJniObject::getStaticObjectField(
      "android/content/Context", "CONNECTIVITY_SERVICE", "Ljava/lang/String;");
  QJniObject cm = ctx.callObjectMethod("getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;",
                                       cm_name.object<jstring>());
  if (!cm.isValid()) return {};

  const jint transport_wifi =
      QJniObject::getStaticField<jint>("android/net/NetworkCapabilities", "TRANSPORT_WIFI");

  QJniObject networks = cm.callObjectMethod("getAllNetworks", "()[Landroid/net/Network;");
  if (!networks.isValid()) return {};

  QJniEnvironment env;
  jobjectArray arr = networks.object<jobjectArray>();
  if (!arr) return {};
  const jsize count = env->GetArrayLength(arr);
  for (jsize i = 0; i < count; ++i) {
    jobject net_obj = env->GetObjectArrayElement(arr, i);
    if (!net_obj) continue;
    QJniObject net(net_obj);
    env->DeleteLocalRef(net_obj);
    QJniObject caps = cm.callObjectMethod(
        "getNetworkCapabilities",
        "(Landroid/net/Network;)Landroid/net/NetworkCapabilities;", net.object<jobject>());
    if (!caps.isValid()) continue;
    if (!caps.callMethod<jboolean>("hasTransport", "(I)Z", transport_wifi)) continue;
    QJniObject lp = cm.callObjectMethod(
        "getLinkProperties", "(Landroid/net/Network;)Landroid/net/LinkProperties;",
        net.object<jobject>());
    const std::string ip = ipv4_from_link_properties(lp);
    if (!ip.empty()) return ip;
  }
  return {};
}

std::string wifi_ipv4_legacy_dhcp() {
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

}  // namespace

void acquire_multicast_lock() {
  std::lock_guard lock(g_lock_mutex);
  QJniObject ctx = android_context();
  if (!ctx.isValid()) {
    nyx::log_write(nyx::LogLevel::Warn, "MulticastLock: no Android context");
    return;
  }
  // Refresh: if already held, keep; else create. Force release when not held.
  if (g_multicast_held && g_multicast_lock.isValid()) {
    if (g_multicast_lock.callMethod<jboolean>("isHeld")) return;
    g_multicast_lock.callMethod<void>("release");
    g_multicast_lock = QJniObject();
    g_multicast_held = false;
  }

  QJniObject wifi_service = QJniObject::getStaticObjectField(
      "android/content/Context", "WIFI_SERVICE", "Ljava/lang/String;");
  QJniObject wifi = ctx.callObjectMethod(
      "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", wifi_service.object<jstring>());
  if (!wifi.isValid()) {
    nyx::log_write(nyx::LogLevel::Warn, "MulticastLock: WifiManager unavailable");
    return;
  }
  QJniObject name = QJniObject::fromString(QStringLiteral("nyx-discovery"));
  QJniObject mlock = wifi.callObjectMethod(
      "createMulticastLock", "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;",
      name.object<jstring>());
  if (!mlock.isValid()) {
    nyx::log_write(nyx::LogLevel::Warn, "MulticastLock: create failed");
    return;
  }
  mlock.callMethod<void>("setReferenceCounted", "(Z)V", jboolean(false));
  mlock.callMethod<void>("acquire");
  g_multicast_lock = mlock;
  g_multicast_held = true;
  nyx::log_write(nyx::LogLevel::Info, "MulticastLock acquired");
}

void release_multicast_lock() {
  std::lock_guard lock(g_lock_mutex);
  if (!g_multicast_held || !g_multicast_lock.isValid()) return;
  g_multicast_lock.callMethod<void>("release");
  g_multicast_lock = QJniObject();
  g_multicast_held = false;
}

std::string wifi_ipv4() {
  std::string ip = wifi_ipv4_from_connectivity();
  if (ip.empty()) ip = wifi_ipv4_legacy_dhcp();
  return ip;
}

void request_call_permissions(bool need_camera,
                              void (*done)(bool mic_ok, bool cam_ok, void* ctx), void* ctx) {
  auto* s = new PermState{need_camera ? 2 : 1, true, true, need_camera, done, ctx};

  QMicrophonePermission mic;
  if (qApp->checkPermission(mic) == Qt::PermissionStatus::Granted) {
    finish_one(s, false, true);
  } else {
    qApp->requestPermission(mic, qApp, [s](const QPermission& perm) {
      finish_one(s, false, perm.status() == Qt::PermissionStatus::Granted);
    });
  }

  if (!need_camera) return;

  QCameraPermission cam;
  if (qApp->checkPermission(cam) == Qt::PermissionStatus::Granted) {
    finish_one(s, true, true);
  } else {
    qApp->requestPermission(cam, qApp, [s](const QPermission& perm) {
      finish_one(s, true, perm.status() == Qt::PermissionStatus::Granted);
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
  if (active) set_speakerphone(g_speakerphone);
}

void set_speakerphone(bool on) {
  g_speakerphone = on;
  QJniObject ctx = android_context();
  if (!ctx.isValid()) return;
  QJniObject::callStaticMethod<void>("org/nyx/app/NyxCallNotify", "setSpeakerphone",
                                     "(Landroid/content/Context;Z)V", ctx.object<jobject>(),
                                     jboolean(on));
}

bool speakerphone() { return g_speakerphone; }

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

void start_keepalive_service() {
  QJniObject ctx = android_context();
  if (!ctx.isValid()) {
    nyx::log_write(nyx::LogLevel::Warn, "KeepAlive: no Android context");
    return;
  }
  nyx::log_write(nyx::LogLevel::Info, "KeepAlive: starting service");
  QJniObject::callStaticMethod<void>("org/nyx/app/NyxKeepAliveService", "start",
                                     "(Landroid/content/Context;)V", ctx.object<jobject>());
}

void stop_keepalive_service() {
  QJniObject ctx = android_context();
  if (!ctx.isValid()) return;
  QJniObject::callStaticMethod<void>("org/nyx/app/NyxKeepAliveService", "stop",
                                     "(Landroid/content/Context;)V", ctx.object<jobject>());
}

void suppress_camera_surface_overlays() {
  QJniObject ctx = android_context();
  if (!ctx.isValid()) {
    nyx::log_write(nyx::LogLevel::Warn, "CameraSurfaces: no Android context");
    return;
  }
  QJniObject::callStaticMethod<void>("org/nyx/app/NyxCameraSurfaces", "suppressOverlays",
                                     "(Landroid/content/Context;)V", ctx.object<jobject>());
}

void mark_camera_surface_baseline() {
  QJniObject ctx = android_context();
  if (!ctx.isValid()) return;
  QJniObject::callStaticMethod<void>("org/nyx/app/NyxCameraSurfaces", "markBaseline",
                                     "(Landroid/content/Context;)V", ctx.object<jobject>());
}

void set_hangup_handler(void (*fn)()) { g_hangup_fn = fn; }

void show_native_hangup_overlay(bool /*show*/) {
  // DecorView overlays break Qt touch dispatch on Android; hangup is via
  // notification action + QML controls instead.
}

void invoke_hangup_handler() {
  if (g_hangup_fn) g_hangup_fn();
}

void voice_playback_start(int sample_rate, int channels) {
  QJniObject::callStaticMethod<void>("org/nyx/app/NyxCallAudio", "startVoicePlayback", "(II)V",
                                     jint(sample_rate), jint(channels));
}

void voice_playback_write(const int16_t* samples, int count) {
  if (!samples || count <= 0) return;
  QJniEnvironment env;
  const jsize nbytes = static_cast<jsize>(count * static_cast<int>(sizeof(int16_t)));
  jbyteArray arr = env->NewByteArray(nbytes);
  if (!arr) return;
  env->SetByteArrayRegion(arr, 0, nbytes, reinterpret_cast<const jbyte*>(samples));
  QJniObject::callStaticMethod<jint>("org/nyx/app/NyxCallAudio", "writeVoicePlayback", "([BII)I",
                                     arr, jint(0), jint(nbytes));
  env->DeleteLocalRef(arr);
}

void voice_playback_stop() {
  QJniObject::callStaticMethod<void>("org/nyx/app/NyxCallAudio", "stopVoicePlayback", "()V");
}

void set_native_camera_callbacks(NativeCameraJpegFn on_jpeg, NativeCameraErrorFn on_error,
                                 NativeCameraStartedFn on_started, void* ctx) {
  g_cam_jpeg = on_jpeg;
  g_cam_error = on_error;
  g_cam_started = on_started;
  g_cam_ctx = ctx;
}

void native_camera_start(bool prefer_front) {
  QJniObject ctx = android_context();
  if (!ctx.isValid()) {
    nyx::log_write(nyx::LogLevel::Warn, "native_camera_start: no context");
    return;
  }
  QJniObject::callStaticMethod<void>("org/nyx/app/NyxCameraCapture", "start",
                                     "(Landroid/content/Context;Z)V", ctx.object<jobject>(),
                                     jboolean(prefer_front ? 1 : 0));
}

void native_camera_stop() {
  QJniObject::callStaticMethod<void>("org/nyx/app/NyxCameraCapture", "stop", "()V");
}

void native_camera_switch_facing() {
  QJniObject::callStaticMethod<void>("org/nyx/app/NyxCameraCapture", "switchFacing", "()V");
}

bool native_camera_has_front_and_back() {
  QJniObject ctx = android_context();
  if (!ctx.isValid()) return false;
  return QJniObject::callStaticMethod<jboolean>("org/nyx/app/NyxCameraCapture", "hasFrontAndBack",
                                                "(Landroid/content/Context;)Z",
                                                ctx.object<jobject>());
}

}  // namespace nyx_android

#include <QMetaObject>
#include <jni.h>

extern "C" JNIEXPORT void JNICALL Java_org_nyx_app_NyxCallNotify_nativeHangup(JNIEnv*, jclass) {
  QMetaObject::invokeMethod(
      qApp,
      []() { nyx_android::invoke_hangup_handler(); },
      Qt::QueuedConnection);
}

extern "C" JNIEXPORT void JNICALL
Java_org_nyx_app_NyxCameraCapture_nativeOnJpeg(JNIEnv* env, jclass, jbyteArray jpeg, jint,
                                               jint, jboolean front) {
  if (!jpeg || !g_cam_jpeg) return;
  const jsize n = env->GetArrayLength(jpeg);
  QByteArray bytes;
  bytes.resize(n);
  env->GetByteArrayRegion(jpeg, 0, n, reinterpret_cast<jbyte*>(bytes.data()));
  auto* fn = g_cam_jpeg;
  void* ctx = g_cam_ctx;
  const bool is_front = front;
  QMetaObject::invokeMethod(
      qApp,
      [fn, ctx, bytes = std::move(bytes), is_front]() mutable {
        if (fn) fn(bytes, is_front, ctx);
      },
      Qt::QueuedConnection);
}

extern "C" JNIEXPORT void JNICALL
Java_org_nyx_app_NyxCameraCapture_nativeOnError(JNIEnv* env, jclass, jstring message) {
  QString msg;
  if (message) {
    const char* utf = env->GetStringUTFChars(message, nullptr);
    if (utf) {
      msg = QString::fromUtf8(utf);
      env->ReleaseStringUTFChars(message, utf);
    }
  }
  auto* fn = g_cam_error;
  void* ctx = g_cam_ctx;
  QMetaObject::invokeMethod(
      qApp,
      [fn, ctx, msg]() {
        if (fn) fn(msg, ctx);
      },
      Qt::QueuedConnection);
}

extern "C" JNIEXPORT void JNICALL
Java_org_nyx_app_NyxCameraCapture_nativeOnStarted(JNIEnv* env, jclass, jboolean front,
                                                  jstring cameraId) {
  QString id;
  if (cameraId) {
    const char* utf = env->GetStringUTFChars(cameraId, nullptr);
    if (utf) {
      id = QString::fromUtf8(utf);
      env->ReleaseStringUTFChars(cameraId, utf);
    }
  }
  auto* fn = g_cam_started;
  void* ctx = g_cam_ctx;
  const bool is_front = front;
  QMetaObject::invokeMethod(
      qApp,
      [fn, ctx, is_front, id]() {
        if (fn) fn(is_front, id, ctx);
      },
      Qt::QueuedConnection);
}

#else

namespace nyx_android {

void acquire_multicast_lock() {}
void release_multicast_lock() {}
std::string wifi_ipv4() { return {}; }

void request_call_permissions(bool /*need_camera*/,
                              void (*done)(bool mic_ok, bool cam_ok, void* ctx), void* ctx) {
  done(true, true, ctx);
}

void request_notification_permission() {}
void set_voip_audio_mode(bool) {}
void set_speakerphone(bool) {}
bool speakerphone() { return true; }
void acquire_call_wake_lock() {}
void release_call_wake_lock() {}
void show_incoming_call_notification(const std::string&) {}
void show_active_call_notification(const std::string&, bool) {}
void cancel_call_notifications() {}
void bring_app_to_foreground() {}
void start_keepalive_service() {}
void stop_keepalive_service() {}
void mark_camera_surface_baseline() {}
void suppress_camera_surface_overlays() {}
void set_native_camera_callbacks(NativeCameraJpegFn, NativeCameraErrorFn, NativeCameraStartedFn,
                                 void*) {}
void native_camera_start(bool) {}
void native_camera_stop() {}
void native_camera_switch_facing() {}
bool native_camera_has_front_and_back() { return false; }
void set_hangup_handler(void (*)()) {}
void show_native_hangup_overlay(bool) {}
void invoke_hangup_handler() {}
void voice_playback_start(int, int) {}
void voice_playback_write(const int16_t*, int) {}
void voice_playback_stop() {}

}  // namespace nyx_android

#endif
