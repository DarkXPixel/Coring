#pragma once


#include "coring2/ProtocolHandler.hpp"
#include "coring2/TcpConnection.hpp"
#include "HttpRequest.hpp"
#include "coring/html404.hpp"
#include <span>
#include <string_view>
#include <format>
namespace Coring2 {
    namespace Debug {
        template<typename Loop>
        class TestHandler : public IProtocolHandler<Loop> {
        public:
            TestHandler(TcpConnection<Loop>& connection) : conn_(connection) {}
            virtual HandlerResponse on_data(Loop& loop, std::span<const std::byte> data) {
                auto str = std::string_view(reinterpret_cast<const char*>(data.data()), data.size_bytes());

                auto http = Coring::parse_http_headers(str);
                auto it = http.headers.find("connection");
                const bool keep_alive = it != http.headers.end() ? it->second == "keep-alive" : false;



                static constexpr std::string_view HTTP_RESPONSE = 
                    "HTTP/1.1 200 OK\r\n"
                    "Server: Coring\r\n"
                    "Content-Length: 13\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n"
                    "Hello, World!";

                static constexpr std::string_view HTTP_RESPONSE_CLOSE = 
                    "HTTP/1.1 200 OK\r\n"
                    "Server: Coring\r\n"
                    "Content-Length: 13\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n"
                    "Hello, World!";

                // static const std::string HTTP_RESPONSE = std::format( 
                //     "HTTP/1.1 404 Not Found\r\n"
                //     "Server: Coring\r\n"
                //     "Content-Type: text/html; charset=UTF-8\r\n"
                //     "Content-Length: {}\r\n"
                //     "Connection: close\r\n"
                //     "\r\n"
                //     "{}"
                //     , Coring::Utilites::HTML_404_ERROR.length(), Coring::Utilites::HTML_404_ERROR);
                conn_.send_data(loop, std::as_bytes(std::span(keep_alive ? HTTP_RESPONSE : HTTP_RESPONSE_CLOSE)));
                return keep_alive ? HandlerResponse::Ok : HandlerResponse::Close;
            }
            virtual void on_write_ready(Loop& loop) {

            }
        private:
            TcpConnection<Loop>& conn_;
        };
    }
}