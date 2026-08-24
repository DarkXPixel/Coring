#include "coring_uring/UringEngine.hpp"
#include <cerrno>
#include <coroutine>
#include <cstdio>
#include <expected>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <netinet/in.h>
#include <print>
#include <sys/socket.h>
#include <system_error>
#include <utility>


namespace Coring {
    bool UringEngine::is_io_uring_supported() {
        io_uring ring;
        int ret = io_uring_queue_init(2, &ring, 0);
        if(ret == 0) {
            io_uring_queue_exit(&ring);
            return true;
        }
        std::println(stderr, "io_uring not supported... Fallback epoll");
        return false;
    }

    std::expected<UringEngine, std::error_code> UringEngine::create() {
        io_uring ring;
        int err;

        if((err = io_uring_queue_init(QUEUE_DEPTH, &ring, 0)) < 0) {
            return std::unexpected(std::error_code(-err, std::generic_category()));
        }

        return UringEngine(ring);
    }

    UringEngine::~UringEngine() {
        io_uring_queue_exit(&ring_);
        ring_ = {};
    }

    void UringEngine::impl_async_accept(int listen_fd, struct sockaddr* addr, socklen_t* len, UringContext& ctx) {
        // while(true) {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if(sqe == nullptr) {
                io_uring_submit(&ring_);
                sqe = io_uring_get_sqe(&ring_);
            }
            io_uring_prep_accept(sqe, listen_fd, addr, len, 0);
            io_uring_sqe_set_data(sqe, &ctx);
        // }
    }

    void UringEngine::impl_async_read(int fd, void* buf, size_t len, UringContext& ctx) {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if(sqe == nullptr) {
            io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
        }
        io_uring_prep_read(sqe, fd, buf, len, 0);
        sqe->flags |= IOSQE_IO_LINK;
       // ctx.h = h;
        io_uring_sqe_set_data(sqe, &ctx);

        io_uring_sqe* sqe_timeout = io_uring_get_sqe(&ring_);
        if(sqe_timeout == nullptr) {
            io_uring_submit(&ring_);
            sqe_timeout = io_uring_get_sqe(&ring_);
        }

        // ctx.timeout.tv_sec = 5;
        // ctx.timeout.tv_nsec = 0;

        // io_uring_prep_link_timeout(sqe_timeout, &ctx.timeout, 0);
        // io_uring_sqe_set_data(sqe_timeout, nullptr);
    }

    void UringEngine::impl_async_write(int fd, const void* buf, size_t len, UringContext& ctx) {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if(sqe == nullptr) {
            io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
        }
        io_uring_prep_write(sqe, fd, buf, len, 0);
       // ctx.h = h;
        io_uring_sqe_set_data(sqe, &ctx);
    }

    void UringEngine::impl_unregister_fd(int fd) {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if(sqe == nullptr) {
            io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
        }
        io_uring_prep_close(sqe, fd);
    }
    void UringEngine::impl_run() {
        while (running_) {
            std::vector<std::coroutine_handle<>> current_ready;
            current_ready.swap(ready_tasks_);
            for(auto h : current_ready) {
                if(h && !h.done()) {
                    h.resume();
                }
            }

            int ret = 0;
            if(!ready_tasks_.empty()) {
                ret = io_uring_submit(&ring_);
            } else {
                ret = io_uring_submit_and_wait(&ring_, 1);
            }

            if(ret < 0) {
                if(ret == -EINTR) {
                    continue;
                }
                break;
            }

            io_uring_cqe* cqe;
            unsigned head;
            unsigned count = 0;

            io_uring_for_each_cqe(&ring_, head, cqe) {
                auto* ctx = static_cast<UringContext*>(io_uring_cqe_get_data(cqe));
                if(ctx) {
                    ctx->res = cqe->res;
                    schedule(std::exchange(ctx->h, nullptr));
                }
                ++count;
            }
            if(count > 0) {
                io_uring_cq_advance(&ring_, count);
            }
        }
    }
}