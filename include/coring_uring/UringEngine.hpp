#pragma once

#include "coring/IOEngine.hpp"
#include <coroutine>
#include <expected>
#include <liburing.h>
#include <sys/socket.h>
#include <system_error>
#include <utility>
namespace Coring {
    struct UringContext {
        std::coroutine_handle<> h{nullptr};
        int res{0};
    };


    class UringEngine : public IOEngineBase<UringEngine>{
        io_uring ring_;
        bool valid_{false};
        static constexpr auto QUEUE_DEPTH = 1024;

        UringEngine(io_uring ring) : ring_(ring), valid_(true) {}
    public:
        UringEngine(const UringEngine&) = delete;
        UringEngine& operator=(const UringEngine&) = delete;

        UringEngine(UringEngine&& other) : ring_(std::exchange(other.ring_, {})){}


        static bool is_io_uring_supported();
        using Context_t = UringContext;
        using AcceptAwaitable_t = struct UringAcceptAwaitable;
        using ReadAwaitable_t = struct UringReadAwaitable;
        using WriteAwaitable_t = struct UringWriteAwaitable;

        ~UringEngine();
        static std::expected<UringEngine, std::error_code> create();

        void impl_async_accept(int listen_fd, struct sockaddr* addr, socklen_t* len, UringContext& ctx);
        void impl_async_read(int fd, void* buf, size_t len, UringContext& ctx);
        void impl_async_write(int fd, const void* buf, size_t len, UringContext& ctx);
        void impl_unregister_fd(int fd);
        void impl_run();
    };
}

#include "coring_uring/UringAwaitables.hpp"