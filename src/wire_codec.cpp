#include "hybrid_python/transport.hpp"
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace hybrid_python::detail {
namespace {
constexpr std::uint16_t version = 1;
constexpr std::uint32_t max_body = 16U * 1024U * 1024U;
void need(const std::vector<std::byte> &b, std::size_t o, std::size_t n) {
  if (o > b.size() || n > b.size() - o)
    throw std::runtime_error("truncated wire message");
}
template <class T> void put(std::vector<std::byte> &b, T x) {
  for (int i = sizeof(T) - 1; i >= 0; --i)
    b.push_back(static_cast<std::byte>(
        (static_cast<std::make_unsigned_t<T>>(x) >> (i * 8)) & 0xff));
}
template <class T> T get(const std::vector<std::byte> &b, std::size_t &o) {
  need(b, o, sizeof(T));
  std::make_unsigned_t<T> x{};
  for (std::size_t i = 0; i < sizeof(T); ++i)
    x = (x << 8) | std::to_integer<unsigned char>(b[o++]);
  return static_cast<T>(x);
}
void put_string(std::vector<std::byte> &b, const std::string &s) {
  if (s.size() > max_body)
    throw std::runtime_error("string too large");
  put<std::uint32_t>(b, static_cast<std::uint32_t>(s.size()));
  b.insert(b.end(), reinterpret_cast<const std::byte *>(s.data()),
           reinterpret_cast<const std::byte *>(s.data() + s.size()));
}
std::string get_string(const std::vector<std::byte> &b, std::size_t &o) {
  const auto n = get<std::uint32_t>(b, o);
  need(b, o, n);
  std::string s(reinterpret_cast<const char *>(b.data() + o), n);
  o += n;
  return s;
}
} // namespace
std::vector<std::byte> encode_frame(const Frame &f) {
  std::vector<std::byte> out;
  const auto n = 11 + f.body.size();
  if (n > max_body)
    throw std::runtime_error("frame too large");
  put<std::uint32_t>(out, static_cast<std::uint32_t>(n));
  put<std::uint16_t>(out, version);
  out.push_back(static_cast<std::byte>(f.type));
  put<std::uint64_t>(out, f.request_id);
  out.insert(out.end(), f.body.begin(), f.body.end());
  return out;
}
Frame decode_frame_body(std::vector<std::byte> b) {
  std::size_t o = 0;
  if (b.size() < 11)
    throw std::runtime_error("short frame");
  if (get<std::uint16_t>(b, o) != version)
    throw std::runtime_error("protocol version mismatch");
  const auto raw = get<std::uint8_t>(b, o);
  if (raw < 1 || raw > 6)
    throw std::runtime_error("unknown message type");
  const auto id = get<std::uint64_t>(b, o);
  return {static_cast<MessageType>(raw),
          id,
          {b.begin() + static_cast<std::ptrdiff_t>(o), b.end()}};
}
void encode_value(std::vector<std::byte> &b, const Value &v) {
  std::visit(
      [&](const auto &x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>)
          b.push_back(std::byte{0});
        else if constexpr (std::is_same_v<T, bool>)
          b.push_back(x ? std::byte{2} : std::byte{1});
        else if constexpr (std::is_same_v<T, std::int64_t>) {
          b.push_back(std::byte{3});
          put<std::int64_t>(b, x);
        } else if constexpr (std::is_same_v<T, double>) {
          b.push_back(std::byte{4});
          put<std::uint64_t>(b, std::bit_cast<std::uint64_t>(x));
        } else if constexpr (std::is_same_v<T, std::string>) {
          b.push_back(std::byte{5});
          put_string(b, x);
        } else {
          b.push_back(std::byte{6});
          put<std::uint32_t>(b, static_cast<std::uint32_t>(x.size()));
          b.insert(b.end(), x.begin(), x.end());
        }
      },
      v);
}
Value decode_value(const std::vector<std::byte> &b, std::size_t &o) {
  need(b, o, 1);
  switch (std::to_integer<unsigned char>(b[o++])) {
  case 0:
    return {};
  case 1:
    return false;
  case 2:
    return true;
  case 3:
    return get<std::int64_t>(b, o);
  case 4:
    return std::bit_cast<double>(get<std::uint64_t>(b, o));
  case 5:
    return get_string(b, o);
  case 6: {
    auto n = get<std::uint32_t>(b, o);
    need(b, o, n);
    std::vector<std::byte> v(b.begin() + static_cast<std::ptrdiff_t>(o),
                             b.begin() + static_cast<std::ptrdiff_t>(o + n));
    o += n;
    return v;
  }
  default:
    throw std::runtime_error("unknown value tag");
  }
}
} // namespace hybrid_python::detail
