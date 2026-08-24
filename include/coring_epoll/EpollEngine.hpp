#pragma once

#include <cerrno>
#include <coroutine>
#include <cstdint>
#include <expected>
#include <liburing.h>
#include <sys/epoll.h>
#include <system_error>
#include <utility>
#include <vector>
#include "coring/IOEngine.hpp"

namespace Coring {
    struct EpollContext {
        std::coroutine_handle<> read_coro;
        std::coroutine_handle<> write_coro;
        //int port{0};
    };

    class EpollEngine : public IOEngineBase<EpollEngine> {
        int epoll_fd_ {-1};
        static constexpr int MAX_EVENTS = 1024;
        epoll_event events_[MAX_EVENTS];

        EpollEngine(int epoll_fd) : epoll_fd_(epoll_fd) {}
    public:
        using Context_t = EpollContext;
        using AcceptAwaitable_t = struct EpollAcceptAwaitable;
        using ReadAwaitable_t = struct EpollReadAwaitable;
        using WriteAwaitable_t = struct EpollWriteAwaitable;

        

        EpollEngine(const EpollEngine&) = delete;
        EpollEngine& operator=(const EpollEngine&) = delete;
        EpollEngine(EpollEngine&& other) : epoll_fd_(std::exchange(other.epoll_fd_, -1)) {}

        ~EpollEngine() {
            if(epoll_fd_ >= 0) {
                ::close(epoll_fd_);
                epoll_fd_ = -1;
            }
        }

        static std::expected<EpollEngine, std::error_code> create() {
            int fd = ::epoll_create1(EPOLL_CLOEXEC);
            if(fd < 0) {
                return std::unexpected(std::error_code(errno, std::generic_category()));
            }
            return EpollEngine(fd);
        }

        void impl_async_accept(int listen_fd, EpollContext& ctx, std::coroutine_handle<> h) {
            async_read_event(listen_fd, ctx, h);
        }

        void impl_async_read(int fd, void*, size_t, EpollContext& ctx, std::coroutine_handle<> h) {
            async_read_event(fd, ctx, h);
        }

        void impl_async_write(int fd, const void*, size_t, EpollContext& ctx, std::coroutine_handle<> h) {
            ctx.write_coro = h;
            epoll_event ev{};
            ev.events = EPOLLOUT | EPOLLET | EPOLLONESHOT;
            ev.data.ptr = &ctx;
            if(::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
                if(errno == ENOENT) {
                    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
                }
            }
        } 

        void impl_unregister_fd(int fd) {
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
            ::close(fd);
        }

        void impl_run() {
            while(running_) {
                std::vector<std::coroutine_handle<>> current_ready;
                current_ready.swap(ready_tasks_);
                for(auto h : current_ready) {
                    if(h && !h.done()) {
                        h.resume();
                    }
                }

                int timeout = ready_tasks_.empty() ? -1 : 0;
                int nfds = ::epoll_wait(epoll_fd_, events_, MAX_EVENTS, timeout);

                for(int i = 0; i < nfds; ++i) {
                    auto* ctx = static_cast<EpollContext*>(events_[i].data.ptr);
                    uint32_t evs = events_[i].events;
                    if((evs & (EPOLLIN | EPOLLHUP | EPOLLERR)) && ctx->read_coro) {
                        schedule(std::exchange(ctx->read_coro, nullptr));
                    }
                    if((evs & (EPOLLOUT | EPOLLHUP | EPOLLERR)) && ctx->write_coro) {
                        schedule(std::exchange(ctx->write_coro, nullptr));
                    }
                }
            }
        }
    private:
        void async_read_event(int fd, EpollContext& ctx, std::coroutine_handle<> h) {
            ctx.read_coro = h;
            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
            ev.data.ptr = &ctx;

            if(::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
                if(errno == ENOENT) {
                    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
                }
            }
        }
    };

}
#include "EpollAwaitables.hpp"