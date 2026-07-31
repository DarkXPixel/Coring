#pragma once
#include <cerrno>
#include <coroutine>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "coring_epoll/EpollEngine.hpp"

namespace Coring {
    struct EpollAcceptAwaitable {
        EpollEngine& engine;
        int listen_fd;
        typename EpollEngine::Context_t& ctx;

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

    struct EpollReadAwaitable {
        EpollEngine& engine;
        int fd;
        void* buf;
        size_t len;
        typename EpollEngine::Context_t& ctx;

        ssize_t bytes_read {-1};
        int read_errno{0};

        bool await_ready() noexcept {
            bytes_read = ::read(fd, buf, len);
            if(bytes_read >= 0) {
                return 0;
            }

            read_errno = errno;

            if(read_errno == EAGAIN || read_errno == EWOULDBLOCK) {
                return false;
            }

            return true;
        }
        void await_suspend(std::coroutine_handle<> h) noexcept {
            engine.async_read(fd,  buf,  len,  ctx, h);
        }

        ssize_t await_resume() noexcept {
            if(bytes_read >= 0) {
                return bytes_read;
            }
            if(read_errno != EAGAIN && read_errno != EWOULDBLOCK) {
                errno = read_errno;
            }

            ssize_t res = ::read(fd, buf, len);
            if(res < 0) {
                return -1;
            }
            return res;
        }
    };

    struct EpollWriteAwaitable {
        EpollEngine& engine;
        int fd;
        const void* buf;
        size_t len;
        typename EpollEngine::Context_t& ctx;

        bool await_ready() noexcept {return false;}
        void await_suspend(std::coroutine_handle<> h) noexcept {
            engine.async_write(fd,  buf,  len,  ctx, h);
        }

        ssize_t await_resume() noexcept {
            return ::write(fd, buf, len);
        }
    };
}