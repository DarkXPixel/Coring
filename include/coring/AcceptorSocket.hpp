#pragma once

#include <asm-generic/socket.h>
#include <cerrno>
#include <expected>
#include <netinet/in.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include "IOEngine.hpp"
#include "coring/Awaitables.hpp"

namespace Coring {
    template<IOEngineConcept Engine>
    class AcceptorSocket {
    private:
        int listen_fd_{-1};
        Engine& engine_;
        typename Engine::Context_t ctx_{};

        AcceptorSocket(int fd, Engine& engine) : listen_fd_(fd), engine_(engine) {}
    public:
        AcceptorSocket(const AcceptorSocket&) = delete;
        AcceptorSocket& operator=(const AcceptorSocket&) = delete;

        AcceptorSocket(AcceptorSocket&& other) noexcept : listen_fd_(std::exchange(other.listen_fd_, -1)), engine_(other.engine_), ctx_(other.ctx_) {}

        ~AcceptorSocket() {
            if(listen_fd_ >= 0) {
                engine_.unregister_fd(listen_fd_);
                ::close(listen_fd_);
                listen_fd_ = -1;
            }
        }

        static std::expected<AcceptorSocket, std::error_code> create(int port, Engine& engine) {
            int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if(fd < 0) {
                return std::unexpected(std::error_code(errno, std::generic_category()));
            }

            int opt = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);

            if(::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
                int err = errno;
                ::close(fd);
                return std::unexpected(std::error_code(std::error_code(err, std::generic_category())));
            }

            if(::listen(fd, SOMAXCONN) < 0) {
                int err = errno;
                ::close(fd);
                return std::unexpected(std::error_code(err, std::generic_category()));
            }
            return AcceptorSocket(fd, engine);
        }

        int fd() const {
            return listen_fd_;
        }

        auto async_accept() {
            return AcceptAwaitable<Engine>{engine_, listen_fd_, ctx_};
        }
    };
}