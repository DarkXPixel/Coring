#pragma once

#include <coroutine>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <optional>
#include <print>
#include <sys/socket.h>
#include <netinet/in.h>
#include <ctime>
#include "HttpRequest.hpp"

namespace Coring {
    struct Task {
        struct promise_type {
            Task get_return_object() {
                return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            std::suspend_never initial_suspend() noexcept {return {};}

            std::suspend_never final_suspend() noexcept {return {};}
            void unhandled_exception() {std::terminate();}
            void return_void() {}
        };

        std::coroutine_handle<promise_type> handle{nullptr};
    };

    struct IOContext {
        std::coroutine_handle<> coro_handle;
        int result{-1}; 

        void resume(int res) {
            result = res;
            if(coro_handle && !coro_handle.done()) {
                coro_handle.resume();
            }
        }
    };

    struct IOAwaiter {
        io_uring* ring;
        uint8_t op_type;
        int fd;
        void* buf{nullptr};
        uint32_t len{0};
        socklen_t* addrlen{nullptr};
        int timeout_sec{0};

        IOContext ctx;
        __kernel_timespec ts{};

        bool await_ready() const noexcept {return false;}

        void await_suspend(std::coroutine_handle<> h) noexcept {
            ctx.coro_handle = h;
            io_uring_sqe* sqe = io_uring_get_sqe(ring);

            if(op_type == IORING_OP_READ) {
                io_uring_prep_read(sqe, fd, buf, len, 0);
            } else if (op_type == IORING_OP_WRITE) {
                io_uring_prep_write(sqe, fd, buf, len, 0);
            } else if (op_type == IORING_OP_ACCEPT) {
                io_uring_prep_accept(sqe, fd, static_cast<sockaddr*>(buf), addrlen, 0);
            }

            if(timeout_sec > 0) {
                sqe->flags |= IOSQE_IO_LINK;
                io_uring_sqe_set_data(sqe, &ctx);

                io_uring_sqe* timeout_sqe = io_uring_get_sqe(ring);
                ts.tv_sec = timeout_sec;
                ts.tv_nsec = 0;
                io_uring_prep_link_timeout(timeout_sqe, &ts, 0);
                io_uring_sqe_set_data(timeout_sqe, nullptr);
            } else {
                io_uring_sqe_set_data(sqe, &ctx);
            }
            io_uring_submit(ring);
        }

        int await_resume() const noexcept {
            return ctx.result;
        }
    };

    inline Task handle_client(io_uring* ring, int client_fd) {
        constexpr int KEEP_ALIVE_TIMEOUT_SEC = 5;
        char buf[1024];
        std::string raw_buffer;
        while(true)
        {
            bool keep_alive = true;

            size_t header_end_pos = std::string::npos;

            while(header_end_pos == std::string::npos) {
                IOAwaiter read_op{
                    .ring = ring,
                    .op_type = IORING_OP_READ,
                    .fd = client_fd,
                    .buf = buf,
                    .len = sizeof(buf),
                    .timeout_sec = 0
                };

                int bytes_read = co_await read_op;

                if(bytes_read <= 0) {
                    close(client_fd);
                    co_return;
                }

                raw_buffer.append(buf, bytes_read);
                // if(raw_buffer.size() > 8 * 1024) {
                //     auto strerror413 = get_http_413_response();
                //     IOAwaiter write_op{
                //         .ring = ring,
                //         .op_type = IORING_OP_WRITE,
                //         .fd = client_fd,
                //         .buf = strerror413.data(),
                //         .len = static_cast<uint32_t>(strerror413.size()),
                //     };
                //     co_await write_op;
                //     close(client_fd);
                //     co_return;
                // }

                header_end_pos = raw_buffer.find("\r\n\r\n");
                if(header_end_pos != std::string::npos) {
                    break;
                }
            }

            std::string_view raw_headers(raw_buffer.data(), header_end_pos);
            HttpRequest request = parse_http_headers(raw_headers);

            auto conn_it = request.headers.find("connection");
            if(conn_it != request.headers.end() && conn_it->second == "close") {
                keep_alive = false;
            }

            size_t body_start_pos = header_end_pos + 4;

            size_t content_lenght = 0;
            auto it = request.headers.find("content-lenght");
            if(it != request.headers.end()) {
                content_lenght = std::stoull(it->second);
            }

            size_t bytes_already_read = raw_buffer.size() - body_start_pos;

            request.body.reserve(content_lenght);
            if(bytes_already_read > 0) {
                request.body.append(raw_buffer.data() + body_start_pos, std::min(bytes_already_read, content_lenght));
            }

            while(request.body.size() < content_lenght) {
                size_t bytes_needed = content_lenght - request.body.size();
                size_t read_size = std::min(sizeof(buf), bytes_needed);
                IOAwaiter read_op{
                    .ring = ring,
                    .op_type = IORING_OP_READ,
                    .fd = client_fd,
                    .buf = buf,
                    .len = static_cast<uint32_t>(read_size),
                    .timeout_sec = 0
                };
                int bytes_read = co_await read_op;
                if(bytes_read <= 0) {
                    break;
                }
                request.body.append(buf, bytes_read);
            }

            size_t total_processed_bytes = body_start_pos + content_lenght;
            if(raw_buffer.size() > total_processed_bytes) {
                raw_buffer.erase(0, total_processed_bytes);
            } else {
                raw_buffer.clear();
            }

            std::println("[+] Recieved HTTP Request:");
            std::println("    Method: {}", request.method);
            std::println("    Path: {}", request.path);
            std::println("    Body lenght: {} bytes", request.body.size());
            if(!request.body.empty() && request.body.size() < 200) {
                std::println("    Body content: {}", request.body);
            }

            std::string response = std::format(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 13\r\n"
                "Connection: {}\r\n"
                "\r\n"
                "Hello World!",
                keep_alive ? "keep-alive" : "close");

            
            IOAwaiter write_op{
                .ring = ring,
                .op_type = IORING_OP_WRITE,
                .fd = client_fd,
                .buf = response.data(),
                .len = static_cast<uint32_t>(response.size())
            };
            int bytes_written = co_await write_op;

            if(bytes_written <= 0 || !keep_alive) {
                break;
            }
        }
        close(client_fd);
        // char buf[1024];

        // while(true) {
        //     IOAwaiter read_op{.ring = ring, .op_type = IORING_OP_READ, .fd = client_fd, .buf = buf, .len = sizeof(buf)};
        //     int bytes_read = co_await read_op;
        //     if(bytes_read <= 0) {
        //         break;
        //     }

        //     // IOAwaiter write_op{.ring = ring, .op_type = IORING_OP_WRITE, .fd = client_fd, .buf = buf, .len = static_cast<uint32_t>(bytes_read)};
        //     // int bytes_written = co_await write_op;

        //     // if(bytes_written <= 0) {
        //     //     break;
        //     // }
        // }
        // close(client_fd);
        // std::println("[-] Client disconnected, fd: {}", client_fd);
    }

    inline Task accept_loop(io_uring* ring, int server_fd) {
        while(true) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);

            IOAwaiter accept_op{.ring = ring, .op_type = IORING_OP_ACCEPT, .fd = server_fd, .buf = &client_addr, .addrlen = &client_len};

            int client_fd = co_await accept_op;

            if(client_fd >= 0) {
                std::println("[+] New client connected, fd: {}", client_fd);
                handle_client(ring, client_fd);
            }
        }
    }

    inline std::optional<int> create_server_socket(uint16_t port) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if(fd < 0) {
            return {};
        }

        int val = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if(bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
            return {};
        }
        if(listen(fd, SOMAXCONN) < 0) {
            return {};
        }
        return fd;
    }

    inline void run_event_loop(io_uring* ring) {
        while (true) {
            io_uring_cqe* cqe = nullptr;

            int ret = io_uring_wait_cqe(ring, &cqe);
            if(ret < 0) {
                break;
            }

            auto* ctx = static_cast<IOContext*>(io_uring_cqe_get_data(cqe));
            if(ctx) {
                ctx->result = cqe->res;
                auto coro = ctx->coro_handle;
                io_uring_cqe_seen(ring, cqe);
                if(coro && !coro.done()) {
                    coro.resume();
                } else {
                    io_uring_cqe_seen(ring, cqe);
                }
            }
        }
    }
    
}
