#pragma once

/** @file session_intent.hpp
 *  Желаемые сессии после старта: auto-reconnect / user disconnect.
 */

#include "nyx/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nyx {

/** Тип желаемой сессии. */
enum class SessionIntentKind : uint8_t {
  GroupHub = 1,
  GroupJoin = 2,
  Direct = 3,
};

/** Запись: поднимать ли сессию автоматически. */
struct SessionIntent {
  SessionIntentKind kind = SessionIntentKind::Direct;
  std::string key;           /**< dm:<hex> | group:<hex> */
  std::string ref_id_hex;    /**< peer или group id */
  std::string invite_hex;    /**< для join / outbound DM token */
  bool enabled = true;       /**< false после «Отключиться» */
  uint64_t updated_ms = 0;
};

/** Persist session_intents.json в data_dir аккаунта. */
class SessionIntentStore {
 public:
  explicit SessionIntentStore(std::string path = {});

  /** Читает intents с диска. @return true даже если файла ещё нет. */
  bool load();
  /** Пишет intents на диск. @return false при ошибке записи. */
  bool save() const;

  const std::vector<SessionIntent>& all() const { return intents_; }

  /** Upsert по key; обновляет updated_ms. */
  void upsert(SessionIntent intent);
  /** Помечает intent.enabled = false (или создаёт disabled). */
  void disable(const std::string& key);
  /** Включает существующий или создаёт enabled intent. */
  void enable(SessionIntent intent);
  /** @return true только если key есть и enabled; неизвестный key = false. */
  bool is_enabled(const std::string& key) const;
  const SessionIntent* find(const std::string& key) const;

 private:
  std::string path_;
  std::vector<SessionIntent> intents_;
};

/** Путь data_dir()/session_intents.json. */
std::string default_session_intents_path();

/** Загружает или создаёт стабильный DM-inbox InviteToken. */
bool load_or_create_dm_inbox_token(InviteToken& out);
/** Hex стабильного inbox token (пустая строка при ошибке). */
std::string dm_inbox_token_hex();

}  // namespace nyx
