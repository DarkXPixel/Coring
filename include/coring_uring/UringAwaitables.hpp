#pragma once


#include "coring_uring/UringEngine.hpp"
#include <coroutine>
namespace Coring {
    struct UringAcceptAwaitable {
        UringEngine& engine;
        int listen_fd;
        typename UringEngine::Context_t& ctx;

        bool await_ready() noexcept {return false;}
        void await_suspend(std::coroutine_handle<> h) noexcept {
            engine.async_accept(listen_fd, ctx, h);
        }

        int await_resume() noexcept {
            return ctx.res;
        }
    };

    struct UringReadAwaitable {
        UringEngine& engine;
        int fd;
        void* buf;
        size_t len;
        typename UringEngine::Context_t& ctx;

        bool await_ready() noexcept {return false;}
        void await_suspend(std::coroutine_handle<> h) noexcept {
            engine.async_read(fd, buf, len, ctx, h);
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
        typename UringEngine::Context_t& ctx;

        bool await_ready() noexcept {
            return false;
        }
        void await_suspend(std::coroutine_handle<> h) noexcept {
            engine.async_write(fd, buf, len, ctx, h);
        }

        int await_resume() noexcept {
            return ctx.res;
        }
    };
}