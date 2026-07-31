#include "coring_uring/UringEngine.hpp"
#include <cerrno>
#include <coroutine>
#include <expected>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <netinet/in.h>
#include <print>
#include <system_error>
#include <utility>


namespace Coring {
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

    void UringEngine::impl_async_accept(int listen_fd, UringContext& ctx, std::coroutine_handle<> h) {
        // while(true) {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if(sqe == nullptr) {
                io_uring_submit(&ring_);
                sqe = io_uring_get_sqe(&ring_);
            }
            io_uring_prep_accept(sqe, listen_fd, static_cast<sockaddr*>(ctx.buf), ctx.addrlen, 0);
            ctx.h = h;
            io_uring_sqe_set_data(sqe, &ctx);
        // }
    }

    void UringEngine::impl_async_read(int fd, void* buf, size_t len, UringContext& ctx, std::coroutine_handle<> h) {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if(sqe == nullptr) {
            io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
        }
        io_uring_prep_read(sqe, fd, buf, len, 0);
        ctx.h = h;
        io_uring_sqe_set_data(sqe, &ctx);
    }

    void UringEngine::impl_async_write(int fd, const void* buf, size_t len, UringContext& ctx, std::coroutine_handle<> h) {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if(sqe == nullptr) {
            io_uring_submit(&ring_);
            sqe = io_uring_get_sqe(&ring_);
        }
        io_uring_prep_write(sqe, fd, buf, len, 0);
        ctx.h = h;
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

            int ret = io_uring_submit_and_wait(&ring_, 1);
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