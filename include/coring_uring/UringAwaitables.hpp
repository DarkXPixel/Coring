#pragma once


#include "coring/ClientSocket.hpp"
#include "coring_uring/UringEngine.hpp"
#include <coroutine>
#include <netinet/in.h>
#include <sys/socket.h>
namespace Coring {
    struct UringAcceptAwaitable {
        UringEngine& engine;
        int listen_fd;

        struct sockaddr_storage client_addr{};
        socklen_t addrlen{sizeof(client_addr)};
        typename UringEngine::Context_t ctx{};

        bool await_ready() noexcept {return false;}
        void await_suspend(std::coroutine_handle<> h) noexcept {
            ctx.h = h;
            engine.async_accept(listen_fd, (struct sockaddr*)&client_addr, &addrlen, ctx);
        }

        ClientSocket<UringEngine> await_resume() noexcept {
            return ClientSocket<UringEngine>(ctx.res, engine, ctx);
        }
    };

    struct UringReadAwaitable {
        UringEngine& engine;
        int fd;
        void* buf;
        size_t len;
        typename UringEngine::Context_t ctx{};

        bool await_ready() noexcept {return false;}
        void await_suspend(std::coroutine_handle<> h) noexcept {
            ctx.h = h;
            engine.async_read(fd, buf, len, ctx);
        } 

        int await_resume() noexcept {
            return ctx.res;
        }
    };

    struct UringWriteAwaitable {
        UringEngine& engine;
        int fd;
        const void* buf;
        size_t len;
        typename UringEngine::Context_t ctx{};

        bool await_ready() noexcept {
            return false;
        }
        void await_suspend(std::coroutine_handle<> h) noexcept {
            ctx.h = h;
            engine.async_write(fd, buf, len, ctx);
        }

        int await_resume() noexcept {
            return ctx.res;
        }
    };
}