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
  Capabilities = 11,
  RangeRequest = 12,
  Cancel = 13,
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

struct FileCapabilities {
  uint8_t version = 2;
  uint32_t flags = 0;
  uint8_t max_parallel = 1;

  static constexpr uint32_t kResume = 1u << 0;
  static constexpr uint32_t kCancel = 1u << 1;
  static constexpr uint32_t kMultiTransfer = 1u << 2;

  ByteBuffer encode() const;
  static std::optional<FileCapabilities> decode(const ByteBuffer& data);
};

struct FileRangeRequest {
  FileHash hash{};
  uint64_t offset = 0;

  ByteBuffer encode() const;
  static std::optional<FileRangeRequest> decode(const ByteBuffer& data);
};

struct FileCancel {
  FileHash hash{};

  ByteBuffer encode() const;
  static std::optional<FileCancel> decode(const ByteBuffer& data);
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
/** ListReq с путём: корень + relative parent (пустые = только share-корни). */
ByteBuffer encode_list_request(const std::string& root_path, const std::string& parent_rel);
std::optional<std::pair<std::string, std::string>> decode_list_request(const ByteBuffer& data);

/** ListResp / IndexPush: записи каталога (папки первыми; обрезается под лимит Noise). */
ByteBuffer encode_list_response(const std::vector<FileEntry>& entries);

/** Участник отправляет hub свой список файлов поля. */
ByteBuffer encode_index_push(const std::vector<FileEntry>& entries,
                             const std::vector<std::string>& root_paths = {},
                             uint64_t revision = 0);

std::optional<std::vector<FileEntry>> decode_list_response(const ByteBuffer& data);

/** Полезная нагрузка IndexPush: файлы и корни папок участника. */
struct IndexPushPayload {
  std::vector<FileEntry> entries;
  std::vector<std::string> root_paths;
  uint64_t revision = 0;
};

std::optional<IndexPushPayload> decode_index_push(const ByteBuffer& data);

/** Hub отправляет участнику JSON политики поля. */
ByteBuffer encode_policy_push(const GroupFileAccess& policy);
/** Разбор PolicyPush; std::nullopt при ошибке. */
std::optional<GroupFileAccess> decode_policy_push(const ByteBuffer& data);

ByteBuffer encode_policy_request();

}  // namespace nyx
