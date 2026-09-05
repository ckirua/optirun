#include "hybrid_python/runtime_manager.hpp"
#include "hybrid_python/transport.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char **environ;

namespace hybrid_python {
namespace {
using detail::Frame;
using detail::MessageType;
constexpr std::uint32_t kMaxBody = 16U * 1024U * 1024U;
void close_fd(int &fd) noexcept {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}
void write_all(int fd, const std::vector<std::byte> &bytes) {
  const auto *data = reinterpret_cast<const char *>(bytes.data());
  std::size_t size = bytes.size();
  while (size) {
    const auto n = ::write(fd, data, size);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      throw std::runtime_error("worker pipe write failed: " +
                               std::string(std::strerror(errno)));
    data += n;
    size -= static_cast<std::size_t>(n);
  }
}
void read_all(int fd, std::byte *data, std::size_t size) {
  while (size) {
    const auto n = ::read(fd, data, size);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      throw std::runtime_error(n == 0 ? "worker closed response pipe"
                                      : "worker pipe read failed: " +
                                            std::string(std::strerror(errno)));
    data += n;
    size -= static_cast<std::size_t>(n);
  }
}
Frame receive_frame(int fd) {
  std::array<std::byte, 4> prefix{};
  read_all(fd, prefix.data(), prefix.size());
  std::uint32_t size{};
  for (auto byte : prefix)
    size = (size << 8) | std::to_integer<unsigned char>(byte);
  if (size < 11 || size > kMaxBody)
    throw std::runtime_error("invalid frame length");
  std::vector<std::byte> body(size);
  read_all(fd, body.data(), body.size());
  return detail::decode_frame_body(std::move(body));
}
std::string json_string(const std::string &json, const std::string &key) {
  const auto marker = "\"" + key + "\":\"";
  const auto begin = json.find(marker);
  if (begin == std::string::npos)
    throw std::runtime_error("HELLO missing " + key);
  const auto start = begin + marker.size(), end = json.find('"', start);
  if (end == std::string::npos)
    throw std::runtime_error("invalid HELLO string");
  return json.substr(start, end - start);
}
bool json_bool(const std::string &json, const std::string &key) {
  const auto marker = "\"" + key + "\":";
  const auto begin = json.find(marker);
  if (begin == std::string::npos)
    throw std::runtime_error("HELLO missing " + key);
  if (json.compare(begin + marker.size(), 4, "true") == 0)
    return true;
  if (json.compare(begin + marker.size(), 5, "false") == 0)
    return false;
  throw std::runtime_error("invalid HELLO bool");
}
std::array<int, 3> json_version(const std::string &json) {
  const auto begin = json.find("\"version\":[");
  if (begin == std::string::npos)
    throw std::runtime_error("HELLO missing version");
  std::array<int, 3> result{};
  char c{};
  std::istringstream stream(json.substr(begin + 11));
  if (!(stream >> result[0] >> c >> result[1] >> c >> result[2]))
    throw std::runtime_error("invalid HELLO version");
  return result;
}
std::vector<std::string> json_handlers(const std::string &json) {
  const std::string marker = "\"handlers\":[";
  auto pos = json.find(marker);
  if (pos == std::string::npos)
    throw std::runtime_error("HELLO missing handlers");
  pos += marker.size();
  std::vector<std::string> names;
  while (pos < json.size() && json[pos] != ']') {
    if (json[pos++] != '"')
      continue;
    const auto end = json.find('"', pos);
    if (end == std::string::npos)
      throw std::runtime_error("invalid HELLO handlers");
    names.push_back(json.substr(pos, end - pos));
    pos = end + 1;
  }
  return names;
}
std::string take_string(const std::vector<std::byte> &body,
                        std::size_t &offset) {
  if (offset + 4 > body.size())
    throw std::runtime_error("truncated remote error");
  std::uint32_t size{};
  for (int i = 0; i < 4; ++i)
    size = (size << 8) | std::to_integer<unsigned char>(body[offset++]);
  if (size > body.size() - offset)
    throw std::runtime_error("truncated remote error");
  std::string result(reinterpret_cast<const char *>(body.data() + offset),
                     size);
  offset += size;
  return result;
}
void append_u32(std::vector<std::byte> &body, std::uint32_t value) {
  for (int i = 3; i >= 0; --i)
    body.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xff));
}
void append_u16(std::vector<std::byte> &body, std::uint16_t value) {
  body.push_back(static_cast<std::byte>(value >> 8));
  body.push_back(static_cast<std::byte>(value));
}
} // namespace

RemoteException::RemoteException(RemoteError error)
    : std::runtime_error(error.type_name + ": " + error.message),
      error_(std::move(error)) {}
const RemoteError &RemoteException::error() const noexcept { return error_; }

class Runtime::Impl {
  struct Pending {
    std::promise<Value> promise;
    Backend backend;
  };
  struct Worker {
    Backend backend;
    WorkerConfig config;
    int request{-1};
    int response{-1};
    pid_t pid{-1};
    WorkerInfo info{};
    std::map<std::string, std::uint32_t> handlers;
    std::jthread reader;
    std::mutex writer;
    bool failed{};
  };
  RuntimeConfig config_;
  std::map<std::string, std::vector<Backend>> registrations_;
  std::unique_ptr<Worker> gil_;
  std::unique_ptr<Worker> free_;
  std::map<std::uint64_t, Pending> pending_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::uint64_t next_id_{1};
  bool started_{};
  bool stopping_{};
  Worker &worker(Backend backend) {
    auto &pointer = backend == Backend::gil ? gil_ : free_;
    if (!pointer)
      throw std::logic_error("runtime is not started");
    return *pointer;
  }
  void fail_worker(Backend backend, const std::string &reason) noexcept {
    std::vector<std::promise<Value>> promises;
    {
      std::lock_guard lock(mutex_);
      auto &selected = worker(backend);
      if (selected.failed)
        return;
      selected.failed = true;
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.backend == backend) {
          promises.push_back(std::move(it->second.promise));
          it = pending_.erase(it);
        } else
          ++it;
      }
      cv_.notify_all();
    }
    for (auto &promise : promises)
      promise.set_exception(std::make_exception_ptr(
          std::runtime_error("worker outcome unknown: " + reason)));
  }
  void reader_loop(Backend backend, std::stop_token token) noexcept {
    try {
      while (!token.stop_requested()) {
        auto frame = receive_frame(worker(backend).response);
        if (frame.type == MessageType::shutdown_ack)
          return;
        Pending pending;
        {
          std::lock_guard lock(mutex_);
          auto it = pending_.find(frame.request_id);
          if (it == pending_.end())
            throw std::runtime_error("unexpected completion");
          pending = std::move(it->second);
          pending_.erase(it);
          cv_.notify_all();
        }
        if (frame.type == MessageType::result) {
          std::size_t offset{};
          auto value = detail::decode_value(frame.body, offset);
          if (offset != frame.body.size())
            throw std::runtime_error("trailing result payload");
          pending.promise.set_value(std::move(value));
        } else if (frame.type == MessageType::error) {
          std::size_t offset{};
          RemoteError error{take_string(frame.body, offset),
                            take_string(frame.body, offset),
                            take_string(frame.body, offset)};
          if (offset != frame.body.size())
            throw std::runtime_error("trailing error payload");
          pending.promise.set_exception(
              std::make_exception_ptr(RemoteException(std::move(error))));
        } else
          throw std::runtime_error("unexpected worker frame");
      }
    } catch (const std::exception &error) {
      if (!stopping_)
        fail_worker(backend, error.what());
    }
  }
  void spawn(Backend backend, WorkerConfig config) {
    int input[2]{-1, -1}, output[2]{-1, -1};
    if (pipe2(input, O_CLOEXEC) != 0 || pipe2(output, O_CLOEXEC) != 0)
      throw std::runtime_error("pipe2 failed");
    std::string executable = config.executable.string(),
                script = std::filesystem::absolute(
                             config_.worker_script.empty()
                                 ? std::filesystem::path(HYBRID_WORKER_SCRIPT)
                                 : config_.worker_script)
                             .string(),
                handler =
                    std::filesystem::absolute(config.handler_file).string();
    std::array<char *, 7> argv{executable.data(),
                               const_cast<char *>("-I"),
                               const_cast<char *>("-B"),
                               script.data(),
                               const_cast<char *>("--handler-file"),
                               handler.data(),
                               nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    pid_t pid{};
    const int result = posix_spawn(&pid, executable.c_str(), &actions, nullptr,
                                   argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close_fd(input[0]);
    close_fd(output[1]);
    if (result != 0) {
      close_fd(input[1]);
      close_fd(output[0]);
      throw std::runtime_error("posix_spawn failed: " +
                               std::string(std::strerror(result)));
    }
    auto selected = std::make_unique<Worker>();
    selected->backend = backend;
    selected->config = std::move(config);
    selected->request = input[1];
    selected->response = output[0];
    selected->pid = pid;
    selected->info.backend = backend;
    selected->info.executable = selected->config.executable;
    const auto hello = receive_frame(selected->response);
    if (hello.type != MessageType::hello || hello.request_id != 0)
      throw std::runtime_error("worker did not send HELLO");
    const std::string json(reinterpret_cast<const char *>(hello.body.data()),
                           hello.body.size());
    if (json_string(json, "implementation") != "cpython")
      throw std::runtime_error("worker is not CPython");
    selected->info.version = json_version(json);
    selected->info.abiflags = json_string(json, "abiflags");
    selected->info.soabi = json_string(json, "soabi");
    selected->info.build_supports_free_threading =
        json_bool(json, "build_supports_free_threading");
    selected->info.gil_enabled = json_bool(json, "gil_enabled");
    const bool free_threaded = backend == Backend::free_threaded;
    if (selected->info.version[0] != 3 || selected->info.version[1] != 14 ||
        selected->info.build_supports_free_threading != free_threaded ||
        selected->info.gil_enabled == free_threaded ||
        (free_threaded &&
         selected->info.abiflags.find('t') == std::string::npos))
      throw std::runtime_error("worker build/GIL identity mismatch");
    for (const auto &name : json_handlers(json))
      selected->handlers.emplace(
          name, static_cast<std::uint32_t>(selected->handlers.size() + 1));
    if (backend == Backend::gil)
      gil_ = std::move(selected);
    else
      free_ = std::move(selected);
  }
  void reap(Worker *selected) noexcept {
    if (!selected)
      return;
    close_fd(selected->request);
    close_fd(selected->response);
    if (selected->pid > 0) {
      kill(selected->pid, SIGKILL);
      waitpid(selected->pid, nullptr, 0);
      selected->pid = -1;
    }
  }

public:
  explicit Impl(RuntimeConfig config) : config_(std::move(config)) {
    if (!config_.max_pending)
      throw std::invalid_argument("max_pending must be positive");
  }
  void register_handler(std::string name, std::vector<Backend> backends) {
    if (started_)
      throw std::logic_error("register_handler after start");
    if (name.empty() || backends.empty())
      throw std::invalid_argument("invalid handler registration");
    registrations_.emplace(std::move(name), std::move(backends));
  }
  void start() {
    if (started_)
      throw std::logic_error("runtime already started");
    std::signal(SIGPIPE, SIG_IGN);
    try {
      spawn(Backend::gil, config_.gil);
      spawn(Backend::free_threaded, config_.free_threaded);
      for (const auto &[name, backends] : registrations_)
        for (auto backend : backends)
          if (!worker(backend).handlers.contains(name))
            throw std::runtime_error("worker lacks registered handler: " +
                                     name);
      started_ = true;
      gil_->reader = std::jthread(
          [this](std::stop_token token) { reader_loop(Backend::gil, token); });
      free_->reader = std::jthread([this](std::stop_token token) {
        reader_loop(Backend::free_threaded, token);
      });
    } catch (...) {
      reap(gil_.get());
      reap(free_.get());
      throw;
    }
  }
  std::future<Value> submit(Backend backend, std::string_view name,
                            std::vector<Value> arguments) {
    std::unique_lock lock(mutex_);
    if (!started_ || stopping_)
      throw std::logic_error("runtime is not accepting submissions");
    auto &selected = worker(backend);
    if (selected.failed)
      throw std::runtime_error("selected worker failed");
    const auto registration = registrations_.find(std::string(name));
    if (registration == registrations_.end() ||
        std::find(registration->second.begin(), registration->second.end(),
                  backend) == registration->second.end())
      throw std::logic_error("handler is not registered for backend");
    if (pending_.size() >= config_.max_pending)
      throw std::runtime_error("maximum pending requests reached");
    if (arguments.size() > 65535)
      throw std::invalid_argument("too many arguments");
    const auto id = next_id_++;
    std::promise<Value> promise;
    auto future = promise.get_future();
    pending_.emplace(id, Pending{std::move(promise), backend});
    std::vector<std::byte> body;
    append_u32(body, selected.handlers.at(std::string(name)));
    append_u16(body, static_cast<std::uint16_t>(arguments.size()));
    for (const auto &value : arguments)
      detail::encode_value(body, value);
    try {
      std::lock_guard write_lock(selected.writer);
      write_all(selected.request, detail::encode_frame({MessageType::invoke, id,
                                                        std::move(body)}));
    } catch (...) {
      auto it = pending_.find(id);
      auto failed = std::move(it->second.promise);
      pending_.erase(it);
      lock.unlock();
      failed.set_exception(std::current_exception());
    }
    return future;
  }
  WorkerInfo info(Backend backend) const {
    return const_cast<Impl *>(this)->worker(backend).info;
  }
  void shutdown() noexcept {
    if (!started_) {
      reap(gil_.get());
      reap(free_.get());
      return;
    }
    std::vector<std::promise<Value>> timed_out;
    {
      std::unique_lock lock(mutex_);
      stopping_ = true;
      if (!cv_.wait_for(lock, config_.shutdown_timeout,
                        [this] { return pending_.empty(); })) {
        for (auto &[id, pending] : pending_)
          timed_out.push_back(std::move(pending.promise));
        pending_.clear();
      }
    }
    for (auto &promise : timed_out)
      promise.set_exception(std::make_exception_ptr(
          std::runtime_error("worker outcome unknown: shutdown timeout")));
    for (auto *selected : {gil_.get(), free_.get()})
      if (selected)
        try {
          std::lock_guard lock(selected->writer);
          write_all(selected->request,
                    detail::encode_frame({MessageType::shutdown, 0, {}}));
        } catch (...) {
        }
    const auto deadline =
        std::chrono::steady_clock::now() + config_.shutdown_timeout;
    for (auto *selected : {gil_.get(), free_.get()})
      if (selected && selected->pid > 0) {
        int status{};
        while (waitpid(selected->pid, &status, WNOHANG) == 0 &&
               std::chrono::steady_clock::now() < deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (waitpid(selected->pid, &status, WNOHANG) == 0)
          kill(selected->pid, SIGKILL);
        waitpid(selected->pid, nullptr, 0);
        selected->pid = -1;
        close_fd(selected->request);
        close_fd(selected->response);
      }
    started_ = false;
  }
};
Runtime::Runtime(RuntimeConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
Runtime::~Runtime() noexcept {
  if (impl_)
    impl_->shutdown();
}
Runtime::Runtime(Runtime &&) noexcept = default;
Runtime &Runtime::operator=(Runtime &&) noexcept = default;
void Runtime::register_handler(std::string name,
                               std::vector<Backend> backends) {
  impl_->register_handler(std::move(name), std::move(backends));
}
void Runtime::start() { impl_->start(); }
std::future<Value> Runtime::submit(Backend backend, std::string_view handler,
                                   std::vector<Value> arguments) {
  return impl_->submit(backend, handler, std::move(arguments));
}
WorkerInfo Runtime::worker_info(Backend backend) const {
  return impl_->info(backend);
}
void Runtime::shutdown() { impl_->shutdown(); }
} // namespace hybrid_python
