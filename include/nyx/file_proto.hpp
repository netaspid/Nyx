#pragma once

/** @file file_proto.hpp
 *  File transfer protocol on kBulkStream.
 */

#include "nyx/file_access.hpp"
#include "nyx/file_hash.hpp"
#include "nyx/file_index.hpp"
#include "nyx/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace nyx {

/** Frame type on the bulk stream. */
enum class FileKind : uint8_t {
  ListReq = 1,
  ListResp = 2,
  Request = 3,
  Offer = 4,
  Chunk = 5,
  Complete = 6,
  Deny = 7,
  /** Member publishes its field index to the hub. */
  IndexPush = 8,
  /** Hub -> member: the full field ACL policy. */
  PolicyPush = 9,
  /** Member -> hub: request for the current ACL policy. */
  PolicyReq = 10,
  Capabilities = 11,
  RangeRequest = 12,
  Cancel = 13,
};

constexpr std::size_t kFileChunkSize = 8192;

struct FileOffer {
  FileHash hash {};
  uint64_t size = 0;
  std::string name;
  std::string mime;

  ByteBuffer encode() const;
  static std::optional<FileOffer> decode(const ByteBuffer& data);
};

struct FileRequest {
  FileHash hash {};

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
  FileHash hash {};
  uint64_t offset = 0;

  ByteBuffer encode() const;
  static std::optional<FileRangeRequest> decode(const ByteBuffer& data);
};

struct FileCancel {
  FileHash hash {};

  ByteBuffer encode() const;
  static std::optional<FileCancel> decode(const ByteBuffer& data);
};

struct FileChunk {
  FileHash hash {};
  uint64_t offset = 0;
  ByteBuffer data;

  ByteBuffer encode() const;
  static std::optional<FileChunk> decode(const ByteBuffer& data);
};

struct FileComplete {
  FileHash hash {};
  uint64_t size = 0;

  ByteBuffer encode() const;
  static std::optional<FileComplete> decode(const ByteBuffer& data);
};

struct FileDeny {
  FileHash hash {};
  std::string reason;

  ByteBuffer encode() const;
  static std::optional<FileDeny> decode(const ByteBuffer& data);
};

ByteBuffer encode_list_request();
/** ListReq with a path: root + relative parent (empty = share roots only). */
ByteBuffer encode_list_request(const std::string& root_path, const std::string& parent_rel);
std::optional<std::pair<std::string, std::string>> decode_list_request(const ByteBuffer& data);

/** ListResp / IndexPush: catalog entries (folders first; trimmed to the Noise limit). */
ByteBuffer encode_list_response(const std::vector<FileEntry>& entries);

/** Member sends the hub its field file list. */
ByteBuffer encode_index_push(const std::vector<FileEntry>& entries,
                             const std::vector<std::string>& root_paths = {},
                             uint64_t revision = 0);

std::optional<std::vector<FileEntry>> decode_list_response(const ByteBuffer& data);

/** IndexPush payload: the member files and folder roots. */
struct IndexPushPayload {
  std::vector<FileEntry> entries;
  std::vector<std::string> root_paths;
  uint64_t revision = 0;
};

std::optional<IndexPushPayload> decode_index_push(const ByteBuffer& data);

/** Hub sends the member the field policy JSON. */
ByteBuffer encode_policy_push(const GroupFileAccess& policy);
/** Parses PolicyPush; std::nullopt on error. */
std::optional<GroupFileAccess> decode_policy_push(const ByteBuffer& data);

ByteBuffer encode_policy_request();

} // namespace nyx
