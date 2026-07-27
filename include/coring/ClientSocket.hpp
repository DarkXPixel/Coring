#pragma once

#include <print>
#include <unistd.h>
#include <utility>
#include "coring/Awaitables.hpp"
#include "coring/IOEngine.hpp"
namespace Coring {
    template<IOEngineConcept Engine>
    class ClientSocket {
        int fd_ {-1};
        Engine& engine_;
        typename Engine::Context_t ctx_{};

    public:
        ClientSocket(int fd, Engine& engine) : fd_(fd), engine_(engine) {
            std::println("[+] New client connected fd: {}", fd_);
        }

        ~ClientSocket() {
            if(fd_ >= 0) {
                std::println("[-] Client disconnected fd: {}", fd_);
                engine_.unregister_fd(fd_);
                ::close(fd_);
                fd_ = -1;
            }
        }

        ClientSocket(const ClientSocket&) = delete;
        ClientSocket& operator=(const ClientSocket&) = delete;
        ClientSocket(ClientSocket&& o) noexcept : fd_(std::exchange(o.fd_, -1)), engine_(o.engine_), ctx_(o.ctx_) {}

        auto async_read(void* buf, size_t len) {
            return ReadAwaitable<Engine>{engine_, fd_, buf, len, ctx_};
        }
        auto async_write(const void* buf, size_t len) {
            return WriteAwaitable<Engine>{engine_, fd_, buf, len, ctx_};
        }
        int fd() const {return fd_;}
    };
}