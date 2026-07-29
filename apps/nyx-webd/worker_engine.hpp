#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nyx_web {

using EventSink = std::function<void(const std::string& json_event)>;

class WorkerEngine {
public:
  explicit WorkerEngine(std::string data_root);
  ~WorkerEngine();

  WorkerEngine(const WorkerEngine&) = delete;
  WorkerEngine& operator=(const WorkerEngine&) = delete;

  void set_event_sink(EventSink sink);
  std::string handle_rpc(const std::string& raw_json);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace nyx_web
