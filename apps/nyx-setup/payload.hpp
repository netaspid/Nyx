#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nyx_setup {

struct PayloadFile {
  std::string relative_path;
  std::vector<std::uint8_t> data;
};

using ProgressFn = std::function<void(int percent, const std::string& status)>;

bool read_self_payload(std::vector<std::uint8_t>& out);
bool parse_payload(const std::vector<std::uint8_t>& blob, std::vector<PayloadFile>& files);
bool extract_payload(const std::vector<std::uint8_t>& blob, const std::string& target_dir,
                     ProgressFn progress, std::string* err);

}  // namespace nyx_setup
