#pragma once

#include "coring2/ProtocolHandler.hpp"
#include "coring2/TcpConnection.hpp"
#include "coring2/http1/http1_response.hpp"
#include "coring2/utility.hpp"
#include "http1_parser.hpp"
#include <vector>
namespace Coring2 {
template <typename Loop> class Http1Handler : public IProtocolHandler<Loop> {
public:
  Http1Handler(TcpConnection<Loop> &connection) : conn_(connection) {}

  virtual HandlerResponse on_data(Loop &loop, std::span<const std::byte> data) {
    header_buf_.append_range(data);
    if (header_buf_.size() > 8196) {
      header_buf_.clear();
      auto str =
          Http1ResponseBuilder{}
              .status(HttpStatus::HeaderTooLarge)
              .content_type("text/plain")
              .body(
                  "The size of the request headers exceeded the server limit.")
              .keep_alive(false)
              .build();
      std::span<const std::byte> buf(
          reinterpret_cast<const std::byte *>(str.data()), str.size());
      conn_.send_data(loop, buf);
      return HandlerResponse::NotRead;
    }
    auto result = Http1Parser::parse(
        std::string_view(reinterpret_cast<const char *>(header_buf_.data()),
                         header_buf_.size()));

    if (!result) {
      if (result.error() == Http1Parser::ParseError::Incomplete) {
        return HandlerResponse::Ok;
      }
      return HandlerResponse::Close;
    }

    std::size_t consumed_bytes = result->header_size;
    header_buf_.erase(header_buf_.begin(),
                      header_buf_.begin() + consumed_bytes);
    // header_buf_.clear();

    auto str = Http1ResponseBuilder{}
                   .status(HttpStatus::OK)
                   .body("hello world")
                   .content_type("text/plain")
                   .keep_alive()
                   .build();
    std::span<const std::byte> buf(
        reinterpret_cast<const std::byte *>(str.data()), str.size());

    conn_.send_data(loop, buf);

    return HandlerResponse::Ok;
  }

  virtual void on_write_ready(Loop &loop) {}

private:
  TcpConnection<Loop> &conn_;
  std::vector<std::byte> header_buf_; // temp
};
} // namespace Coring2