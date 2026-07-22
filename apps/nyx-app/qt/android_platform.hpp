#pragma once

/** Android-specific helpers: Wi‑Fi multicast lock, LAN IPv4, call permissions, call notify. */

#include <string>

namespace nyx_android {

/** Hold a WifiManager MulticastLock for LAN discovery (no-op off Android). */
void acquire_multicast_lock();
void release_multicast_lock();

/** Wi‑Fi IPv4 from WifiManager (empty if not on Wi‑Fi). No-op off Android. */
std::string wifi_ipv4();

/**
 * Request RECORD_AUDIO and optionally CAMERA, then invoke done(granted).
 * Off Android, calls done(true) immediately.
 */
void request_call_permissions(bool need_camera, void (*done)(bool granted, void* ctx), void* ctx);

/** Request POST_NOTIFICATIONS on API 33+ (no-op / granted elsewhere). */
void request_notification_permission();

/** VoIP AudioManager mode + speaker routing while call is active. */
void set_voip_audio_mode(bool active);

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

}  // namespace nyx_android
