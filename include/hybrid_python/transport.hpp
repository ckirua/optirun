#pragma once
#include "hybrid_python/runtime_manager.hpp"
#include <cstdint>
#include <memory>
#include <vector>

namespace hybrid_python::detail {
enum class MessageType : std::uint8_t { hello = 1, invoke, result, error, shutdown, shutdown_ack };
struct Frame { MessageType type; std::uint64_t request_id; std::vector<std::byte> body; };
class Transport {
 public:
  virtual ~Transport() = default;
  virtual void send(const Frame& frame) = 0;
  virtual Frame receive() = 0;
  virtual void close() noexcept = 0;
};
std::vector<std::byte> encode_frame(const Frame& frame);
Frame decode_frame_body(std::vector<std::byte> body);
void encode_value(std::vector<std::byte>& output, const Value& value);
Value decode_value(const std::vector<std::byte>& input, std::size_t& offset);
}  // namespace hybrid_python::detail
