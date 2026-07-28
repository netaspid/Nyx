#pragma once

/** Android-specific helpers: Wi‑Fi multicast lock, LAN IPv4, call permissions, call notify. */

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <string>
#include <vector>

namespace nyx_android {

/** Hold / refresh WifiManager MulticastLock for LAN discovery (no-op off Android). */
void acquire_multicast_lock();
void release_multicast_lock();

/** Wi‑Fi IPv4 from ConnectivityManager LinkProperties (empty if not on Wi‑Fi). */
std::string wifi_ipv4();

/**
 * Request RECORD_AUDIO and optionally CAMERA.
 * done(mic_ok, cam_ok, ctx) — cam_ok is true when camera was not requested.
 * Off Android, calls done(true, true).
 */
void request_call_permissions(bool need_camera,
                              void (*done)(bool mic_ok, bool cam_ok, void* ctx), void* ctx);

/** Request POST_NOTIFICATIONS on API 33+ (no-op / granted elsewhere). */
void request_notification_permission();

/** VoIP AudioManager mode + AudioFocus while call is active. */
void set_voip_audio_mode(bool active);

/** Speakerphone on/off (Android). No-op elsewhere. */
void set_speakerphone(bool on);
bool speakerphone();

/** Keep CPU awake briefly so background invite can be processed/notified. */
void acquire_call_wake_lock();
void release_call_wake_lock();

/** Incoming call heads-up / full-screen intent notification. */
void show_incoming_call_notification(const std::string& peer_title);
/** Ongoing notification while call is active (keeps process more visible). */
void show_active_call_notification(const std::string& peer_title, bool video);
void cancel_call_notifications();

/** Bring QtActivity to foreground if possible. */
void bring_app_to_foreground();

/** Quiet FGS so UDP / call invites survive when the activity is backgrounded. */
void start_keepalive_service();
void stop_keepalive_service();

/** Push Camera2 preview SurfaceView behind Qt so QML controls stay tappable. */
void mark_camera_surface_baseline();
void suppress_camera_surface_overlays();

/**
 * Native Camera2 + ImageReader (no Activity SurfaceView).
 * Callbacks are invoked on the Qt GUI thread via QueuedConnection.
 */
using NativeCameraJpegFn = void (*)(const QByteArray& jpeg, bool front, void* ctx);
using NativeCameraErrorFn = void (*)(const QString& message, void* ctx);
using NativeCameraStartedFn = void (*)(bool front, const QString& camera_id, void* ctx);
void set_native_camera_callbacks(NativeCameraJpegFn on_jpeg, NativeCameraErrorFn on_error,
                                 NativeCameraStartedFn on_started, void* ctx);
void native_camera_start(bool prefer_front);
void native_camera_stop();
void native_camera_switch_facing();
bool native_camera_has_front_and_back();

using NativeRecordingStartedFn = void (*)(const QString& path, void* ctx);
using NativeRecordingStoppedFn = void (*)(const QString& path, bool success,
                                          void* ctx);
using NativeRecordingErrorFn = void (*)(const QString& message, void* ctx);
void set_native_recording_callbacks(NativeRecordingStartedFn on_started,
                                    NativeRecordingStoppedFn on_stopped,
                                    NativeRecordingErrorFn on_error, void* ctx);
void native_camera_start_recording(const QString& path);
void native_camera_stop_recording();

/** Register hangup callback invoked from native Android hangup UI / notification. */
void set_hangup_handler(void (*fn)());

/** Show/hide a native DecorView hangup button above any camera SurfaceView. */
void show_native_hangup_overlay(bool show);

/** Called from JNI on the Qt GUI thread. */
void invoke_hangup_handler();

/** Stop ringtone/vibration without tearing down call notifications. */
void stop_ringtone();

/** Android VoIP playback via AudioTrack (VOICE_COMMUNICATION / MEDIA). No-op elsewhere. */
void voice_playback_start(int sample_rate, int channels);
void voice_playback_write(const int16_t* samples, int count);
void voice_playback_stop();

/** Android mic via AudioRecord (VOICE_COMMUNICATION). No-op elsewhere. */
bool voice_capture_start(int sample_rate, int channels);
/** Non-blocking read into samples; returns sample count (not bytes). */
int voice_capture_read(int16_t* samples, int max_samples);
void voice_capture_stop();

/** Short audible beep for settings speaker check (Android MEDIA stream). */
void play_test_tone(int sample_rate = 48000, int duration_ms = 700);

struct StorageDocument {
  QString uri;
  QString name;
  QString mime;
  qint64 size = -1;
};
StorageDocument storage_document_info(const QString& uri);
bool copy_content_uri(const QString& uri, const QString& destination);
bool export_file(const QString& path, const QString& display_name,
                 const QString& mime);
/** Open local file with system viewer via FileProvider (Android). */
bool open_file(const QString& path, const QString& mime);

}  // namespace nyx_android
