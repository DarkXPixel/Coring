#pragma once

#include "coring/IOEngine.hpp"
#include <coroutine>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
namespace Coring {
    template<IOEngineConcept Engine>
    struct AcceptAwaitable {
        Engine& engine;
        int listen_fd;
        typename Engine::Context_t& ctx;

        bool await_ready() noexcept {return false;}
        void await_suspend(std::coroutine_handle<> h) noexcept {
            engine.async_accept(listen_fd, ctx, h);
        }

        int await_resume() noexcept {
            sockaddr_in addr{};
            socklen_t len = sizeof(addr);
            return ::accept4(listen_fd, (sockaddr*)&addr, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        }
    };

    template<IOEngineConcept Engine>
    struct ReadAwaitable {
        Engine& engine;
        int fd;
        void* buf;
        size_t len;
        typename Engine::Context_t& ctx;

        bool await_ready() noexcept {return false;}
        void await_suspend(std::coroutine_handle<> h) noexcept {
            engine.async_read(fd,  buf,  len,  ctx, h);
        }

        ssize_t await_resume() noexcept {
            return ::read(fd, buf, len);
        }
    };

    template<IOEngineConcept Engine>
    struct WriteAwaitable {
        Engine& engine;
        int fd;
        const void* buf;
        size_t len;
        typename Engine::Context_t& ctx;

        bool await_ready() noexcept {return false;}
        void await_suspend(std::coroutine_handle<> h) noexcept {
            engine.async_write(fd,  buf,  len,  ctx, h);
        }

        ssize_t await_resume() noexcept {
            return ::write(fd, buf, len);
        }
    };
}