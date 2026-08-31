#pragma once
#include "coring2/IOContext.hpp"
#include "coring2/TcpConnection.hpp"
#include <expected>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <format>

namespace Coring2 {
    template<typename Loop>
    class WorkerThreadLoop {
    public:
        WorkerThreadLoop() {
            loop_.init(4096);
            static_thread_local_loop_ = this;
        }

        static std::expected<WorkerThreadLoop, std::string> create(int cpu_core_id) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpu_core_id);
            CPU_SET(cpu_core_id, &cpuset);
            pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

            std::expected<Loop, std::string> engine = Loop::create();
            if(!engine.has_value()) {
                return std::unexpected(std::format("Error create engine: {}", engine.error()));
            }
            return WorkerThreadLoop{std::move(*engine)};
        }

        void on_accept(IOAcceptContext* ctx, int res) {
            if(res < 0) {
                rearm_accept(ctx);
                return;
            }

            int client_fd = res;
            TcpConnection<Loop>* conn = new TcpConnection<Loop>(client_fd);

            conn->start_reading(loop_);

            rearm_accept(ctx);
        }

        Loop& get_loop() {
            return loop_;
        }

        static WorkerThreadLoop<Loop>* get_thread_loop() noexcept {
            return static_thread_local_loop_;
        }

        //temp
        void start(int port) noexcept {
            int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            int opt = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);
            auto _ = ::bind(fd, (sockaddr*)&addr, sizeof(addr));
            ::listen(fd, SOMAXCONN);
            ctx.fd = fd;
            ctx.data = this;
            rearm_accept(&ctx);

            while(true) {
                loop_.poll_events(0);
            }
        }

    private:
        void rearm_accept(IOAcceptContext* accept_ctx) {
            loop_.submit_accept(accept_ctx);
        }
    private:
        Loop loop_;
        IOAcceptContext ctx;
        thread_local inline static WorkerThreadLoop<Loop>* static_thread_local_loop_{nullptr};

    };

}