#pragma once

#include "coring2/ProtocolHandler.hpp"
#include "coring2/http1/http1_response.hpp"
#include "http1_parser.hpp"
#include <cstddef>
#include <cstring>
#include <memory>
#include <string_view>
namespace Coring2 {
// template <typename Loop> class Http1Handler : public IProtocolHandler<Loop> {
//   enum class State { HeaderReading, BodyStream };

// public:
//   Http1Handler(TcpConnection<Loop> &connection) : conn_(connection) {}

//   virtual HandlerResponse on_data(Loop &loop, std::span<const std::byte>
//   data) {
//     header_buf_.append_range(data);
//     if (header_buf_.size() > 8196) {
//       header_buf_.clear();
//       auto str =
//           Http1ResponseBuilder{}
//               .status(HttpStatus::HeaderTooLarge)
//               .content_type("text/plain")
//               .body(
//                   "The size of the request headers exceeded the server
//                   limit.")
//               .keep_alive(false)
//               .build();
//       std::span<const std::byte> buf(
//           reinterpret_cast<const std::byte *>(str.data()), str.size());
//       conn_.send_data(loop, buf);
//       return HandlerResponse::NotRead;
//     }
//     auto result = Http1Parser::parse(
//         std::string_view(reinterpret_cast<const char *>(header_buf_.data()),
//                          header_buf_.size()));

//     if (!result) {
//       if (result.error() == Http1Parser::ParseError::Incomplete) {
//         return HandlerResponse::Ok;
//       }
//       return HandlerResponse::Close;
//     }

//     std::size_t consumed_bytes = result->header_size;
//     header_buf_.erase(header_buf_.begin(),
//                       header_buf_.begin() + consumed_bytes);
//     // header_buf_.clear();

//     auto str = Http1ResponseBuilder{}
//                    .status(HttpStatus::OK)
//                    .body("hello world")
//                    .content_type("text/plain")
//                    .keep_alive()
//                    .build();
//     std::span<const std::byte> buf(
//         reinterpret_cast<const std::byte *>(str.data()), str.size());

//     conn_.send_data(loop, buf);

//     return HandlerResponse::Ok;
//   }

//   virtual void on_write_ready(Loop &loop) {}

// private:
//   TcpConnection<Loop> &conn_;
//   std::vector<std::byte> header_buf_; // temp
// };

template <typename Loop> class TcpConnection;

template <typename Loop> class Http1Handler_ {
  static constexpr auto MAX_HEADER_SIZE = 8196;

public:
  HandlerResponse on_data(Loop &loop, TcpConnection<Loop> &conn,
                          std::span<const std::byte> data,
                          int flags = 0) noexcept;

private:
  struct HeaderBuffer {
    std::array<std::byte, MAX_HEADER_SIZE> data;
    std::uint16_t size{0};
  };
  std::unique_ptr<HeaderBuffer> header_buf_;
};
} // namespace Coring2

namespace Coring2 {
template <typename Loop>
HandlerResponse Http1Handler_<Loop>::on_data(Loop &loop,
                                             TcpConnection<Loop> &conn,
                                             std::span<const std::byte> data,
                                             int flags) noexcept {
  std::span<const std::byte> parse_data = data;
  if (header_buf_) {
    if (header_buf_->size + data.size_bytes() > MAX_HEADER_SIZE) {
      header_buf_.reset();
      return HandlerResponse::Close;
    }
    std::memcpy(header_buf_->data.data() + header_buf_->size, data.data(),
                data.size_bytes());
    header_buf_->size += data.size_bytes();
    parse_data = header_buf_->data;
  }

  auto result = Http1Parser::parse(
      std::string_view(reinterpret_cast<const char *>(parse_data.data()),
                       parse_data.size_bytes()));
  if (!result) {
    if (result.error() == Http1Parser::ParseError::Incomplete) {
      if (!header_buf_) {
        header_buf_ = std::make_unique<HeaderBuffer>();
        std::memcpy(header_buf_->data.data(), data.data(), data.size_bytes());
        header_buf_->size = data.size_bytes();
      }
      return HandlerResponse::Ok;
    }
  }

  auto buf = conn.prepare_buffer();
  auto res = Http1ResponseBuilder{}
                 .status(HttpStatus::OK)
                 .body("hello world")
                 .content_type("text/plain")
                 .keep_alive()
                 .build_to(buf);

  conn.send_data(loop, res);

  return HandlerResponse::Ok;
}

} // namespace Coring2