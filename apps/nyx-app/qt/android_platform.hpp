#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <string>
#include <vector>

namespace nyx_android {

void acquire_multicast_lock();
void release_multicast_lock();

std::string wifi_ipv4();

void request_call_permissions(bool need_camera,
                              void (*done)(bool mic_ok, bool cam_ok, void* ctx),
                              void* ctx);

void request_notification_permission();

void set_voip_audio_mode(bool active);

void set_speakerphone(bool on);
bool speakerphone();

void acquire_call_wake_lock();
void release_call_wake_lock();

void show_incoming_call_notification(const std::string& peer_title);

void show_active_call_notification(const std::string& peer_title, bool video);
void cancel_call_notifications();

void bring_app_to_foreground();

void start_keepalive_service();
void stop_keepalive_service();

void mark_camera_surface_baseline();
void suppress_camera_surface_overlays();

using NativeCameraJpegFn = void (*)(const QByteArray& jpeg, bool front, void* ctx);
using NativeCameraErrorFn = void (*)(const QString& message, void* ctx);
using NativeCameraStartedFn = void (*)(bool front, const QString& camera_id, void* ctx);
void set_native_camera_callbacks(NativeCameraJpegFn on_jpeg,
                                 NativeCameraErrorFn on_error,
                                 NativeCameraStartedFn on_started,
                                 void* ctx);
void native_camera_start(bool prefer_front);
void native_camera_stop();
void native_camera_switch_facing();
bool native_camera_has_front_and_back();

using NativeRecordingStartedFn = void (*)(const QString& path, void* ctx);
using NativeRecordingStoppedFn = void (*)(const QString& path, bool success, void* ctx);
using NativeRecordingErrorFn = void (*)(const QString& message, void* ctx);
void set_native_recording_callbacks(NativeRecordingStartedFn on_started,
                                    NativeRecordingStoppedFn on_stopped,
                                    NativeRecordingErrorFn on_error,
                                    void* ctx);
void native_camera_start_recording(const QString& path);
void native_camera_stop_recording();

void set_hangup_handler(void (*fn)());

void show_native_hangup_overlay(bool show);

void invoke_hangup_handler();

void stop_ringtone();

void voice_playback_start(int sample_rate, int channels);
void voice_playback_write(const int16_t* samples, int count);
void voice_playback_stop();

bool voice_capture_start(int sample_rate, int channels);

int voice_capture_read(int16_t* samples, int max_samples);
void voice_capture_stop();

void play_test_tone(int sample_rate = 48000, int duration_ms = 700);

struct StorageDocument {
  QString uri;
  QString name;
  QString mime;
  qint64 size = -1;
};
StorageDocument storage_document_info(const QString& uri);
bool copy_content_uri(const QString& uri, const QString& destination);
bool export_file(const QString& path, const QString& display_name, const QString& mime);

bool open_file(const QString& path, const QString& mime);

} // namespace nyx_android
