#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace optirun {
enum class Backend { gil, free_threaded };
using Value = std::variant<std::monostate, bool, std::int64_t, double,
                           std::string, std::vector<std::byte>>;

struct WorkerInfo {
  Backend backend;
  std::filesystem::path executable;
  std::array<int, 3> version{};
  std::string abiflags;
  std::string soabi;
  bool build_supports_free_threading{};
  bool gil_enabled{};
};
struct RemoteError {
  std::string type_name;
  std::string message;
  std::string traceback;
};
class RemoteException final : public std::runtime_error {
public:
  explicit RemoteException(RemoteError error);
  [[nodiscard]] const RemoteError &error() const noexcept;

private:
  RemoteError error_;
};
struct WorkerConfig {
  std::filesystem::path executable;
  std::filesystem::path handler_file;
};
struct RuntimeConfig {
  WorkerConfig gil;
  WorkerConfig free_threaded;
  std::size_t max_pending{64};
  std::chrono::milliseconds handshake_timeout{5000};
  std::chrono::milliseconds shutdown_timeout{5000};
  std::filesystem::path worker_script;
};
class Runtime {
public:
  explicit Runtime(RuntimeConfig config);
  ~Runtime() noexcept;
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  Runtime(Runtime &&) noexcept;
  Runtime &operator=(Runtime &&) noexcept;
  void register_handler(std::string name, std::vector<Backend> backends);
  void start();
  [[nodiscard]] std::future<Value> submit(Backend backend,
                                          std::string_view handler,
                                          std::vector<Value> arguments);
  [[nodiscard]] WorkerInfo worker_info(Backend backend) const;
  void shutdown();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace optirun
