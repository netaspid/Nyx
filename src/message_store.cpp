#include "nyx/message_store.hpp"
#include "nyx/chat_id.hpp"
#include "nyx/group.hpp"

#include "nyx/json_text.hpp"

#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace nyx {

namespace {

std::string to_lower_ascii(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

} // namespace

std::string MessageStore::path_for_chat(const ChatId& chat_id) {
  const std::string dir = data_dir() + "/chats";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir + '/' + chat_id_hex(chat_id) + ".jsonl";
}

std::string MessageStore::path_for_group(const GroupId& group_id) {
  const std::string dir = data_dir() + "/groups";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir + '/' + GroupStore::group_id_hex(group_id) + ".jsonl";
}

std::string MessageStore::chat_path(const UserId& peer_id) {
  const std::string dir = data_dir() + "/chats";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir + '/' + to_hex(peer_id.data(), peer_id.size()) + ".jsonl";
}

MessageStore::MessageStore(std::string path) : path_(std::move(path)) {}

void MessageStore::rebind(std::string path) {
  path_ = std::move(path);
  cache_.clear();
  loaded_ = false;
}

bool MessageStore::load_from_disk() const {
  if (loaded_)
    return true;
  loaded_ = true;
  cache_.clear();

  std::error_code ec;
  const auto size = std::filesystem::file_size(path_, ec);
  if (!ec && !json_store_within_limit(static_cast<std::size_t>(size)))
    return false;

  std::ifstream file(path_, std::ios::binary);
  if (!file)
    return true;

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty())
      continue;
    StoredMessage msg;
    msg.id = json_get_u64(line, "id");
    msg.timestamp_ms = json_get_u64(line, "ts");
    if (auto author = json_get_string(line, "author"))
      msg.author = *author;
    if (auto author_id = json_get_string(line, "author_id")) {
      msg.author_id_hex = *author_id;
    }
    if (auto chat_id = json_get_string(line, "chat_id"))
      msg.chat_id_hex = *chat_id;
    if (auto text = json_get_string(line, "text"))
      msg.text = *text;
    msg.outgoing = json_get_bool(line, "out").value_or(false);
    cache_.push_back(std::move(msg));
  }
  return true;
}

bool MessageStore::append(const StoredMessage& message) {
  load_from_disk();
  if (contains_id(message.id))
    return true;
  ensure_data_dir();
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);

  std::ofstream file(path_, std::ios::binary | std::ios::app);
  if (!file)
    return false;

  file << "{\"id\":" << message.id << ",\"ts\":" << message.timestamp_ms << ",\"chat_id\":\""
       << json_escape(message.chat_id_hex) << "\",\"author\":\"" << json_escape(message.author)
       << "\",\"author_id\":\"" << json_escape(message.author_id_hex) << "\",\"text\":\""
       << json_escape(message.text) << "\",\"out\":" << (message.outgoing ? "true" : "false")
       << "}\n";

  cache_.push_back(message);
  return static_cast<bool>(file);
}

bool MessageStore::contains_id(uint64_t id) const {
  if (id == 0)
    return false;
  load_from_disk();
  for (const auto& msg : cache_) {
    if (msg.id == id)
      return true;
  }
  return false;
}

std::vector<StoredMessage> MessageStore::recent(std::size_t count) const {
  load_from_disk();
  if (count >= cache_.size())
    return cache_;
  return std::vector<StoredMessage>(cache_.end() - static_cast<std::ptrdiff_t>(count),
                                    cache_.end());
}

std::vector<StoredMessage> MessageStore::search(const std::string& query, std::size_t limit) const {
  load_from_disk();
  if (query.empty())
    return {};

  const std::string q = to_lower_ascii(query);
  std::vector<StoredMessage> hits;
  for (auto it = cache_.rbegin(); it != cache_.rend() && hits.size() < limit; ++it) {
    const std::string text = to_lower_ascii(it->text);
    const std::string author = to_lower_ascii(it->author);
    if (text.find(q) != std::string::npos || author.find(q) != std::string::npos) {
      hits.push_back(*it);
    }
  }
  std::reverse(hits.begin(), hits.end());
  return hits;
}

} // namespace nyx
