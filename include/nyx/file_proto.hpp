#pragma once

/** @file file_proto.hpp
 *  Протокол передачи файлов на kBulkStream (фаза 4).
 */

#include "nyx/file_access.hpp"
#include "nyx/file_hash.hpp"
#include "nyx/file_index.hpp"
#include "nyx/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace nyx {

/** Тип кадра на bulk-потоке. */
enum class FileKind : uint8_t {
  ListReq = 1,
  ListResp = 2,
  Request = 3,
  Offer = 4,
  Chunk = 5,
  Complete = 6,
  Deny = 7,
  /** Участник публикует свой индекс поля на hub. */
  IndexPush = 8,
  /** Hub → участник: полная политика ACL поля. */
  PolicyPush = 9,
  /** Участник → hub: запрос актуальной политики ACL. */
  PolicyReq = 10,
};

constexpr std::size_t kFileChunkSize = 8192;

struct FileOffer {
  FileHash hash{};
  uint64_t size = 0;
  std::string name;
  std::string mime;

  ByteBuffer encode() const;
  static std::optional<FileOffer> decode(const ByteBuffer& data);
};

struct FileRequest {
  FileHash hash{};

  ByteBuffer encode() const;
  static std::optional<FileRequest> decode(const ByteBuffer& data);
};

struct FileChunk {
  FileHash hash{};
  uint64_t offset = 0;
  ByteBuffer data;

  ByteBuffer encode() const;
  static std::optional<FileChunk> decode(const ByteBuffer& data);
};

struct FileComplete {
  FileHash hash{};
  uint64_t size = 0;

  ByteBuffer encode() const;
  static std::optional<FileComplete> decode(const ByteBuffer& data);
};

struct FileDeny {
  FileHash hash{};
  std::string reason;

  ByteBuffer encode() const;
  static std::optional<FileDeny> decode(const ByteBuffer& data);
};

ByteBuffer encode_list_request();
ByteBuffer encode_list_response(const std::vector<FileEntry>& entries);

/** Участник отправляет hub свой список файлов поля. */
ByteBuffer encode_index_push(const std::vector<FileEntry>& entries,
                             const std::vector<std::string>& root_paths = {});

std::optional<std::vector<FileEntry>> decode_list_response(const ByteBuffer& data);

/** Полезная нагрузка IndexPush: файлы и корни папок участника. */
struct IndexPushPayload {
  std::vector<FileEntry> entries;
  std::vector<std::string> root_paths;
};

std::optional<IndexPushPayload> decode_index_push(const ByteBuffer& data);

/** Hub отправляет участнику JSON политики поля. */
ByteBuffer encode_policy_push(const GroupFileAccess& policy);
/** Разбор PolicyPush; std::nullopt при ошибке. */
std::optional<GroupFileAccess> decode_policy_push(const ByteBuffer& data);

ByteBuffer encode_policy_request();

}  // namespace nyx
