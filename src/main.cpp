#include <liburing.h>
#include "../include/Task.hpp"
#include <print>
#include <iostream>

int main() {
    constexpr int16_t PORT = 8080;
    constexpr uint32_t QUEUE_DEPTH = 256;

    io_uring ring;
    if(io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
        std::println(stderr, "Failed to init io_uring");
        return 1;
    }

    auto server_fd_opt = Coring::create_server_socket(PORT);
    if(!server_fd_opt.has_value()) {
        std::println(stderr, "Failed to create server socket");
        return 1;
    }

    std::println("Server started on port {}...", PORT);
    Coring::accept_loop(&ring, *server_fd_opt);

    while(true) {
        io_uring_cqe* cqe = nullptr;
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if(ret < 0) {
            std::println(stderr, "io_uring_wait_cqe failed: {}", ret);
            break;
        }
        auto* ctx = static_cast<Coring::IOContext*>(io_uring_cqe_get_data(cqe));
        if(ctx) {
            ctx->resume(cqe->res);
        }
        io_uring_cqe_seen(&ring, cqe);
        // io_uring_submit_and_wait(&ring, 1);

        // io_uring_cqe* cqe;
        // uint32_t head;
        // uint32_t count = 0;

        // io_uring_for_each_cqe(&ring, head, cqe) {
        //     count++;

        //     auto* awaiter = static_cast<Coring::IOAwaiter*>(io_uring_cqe_get_data(cqe));
        //     if(awaiter) {
        //         awaiter->resume_coroutine(cqe->res);
        //     }
        // }
        // io_uring_cq_advance(&ring, count);
    }

    close(*server_fd_opt);
    io_uring_queue_exit(&ring);

    return 0;
}