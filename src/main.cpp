#include <liburing.h>
#include "../include/Task.hpp"
#include <print>
#include <iostream>
#include <sched.h>

int main() {
    constexpr int16_t PORT = 8080;
    constexpr uint32_t QUEUE_DEPTH = 256;


    // cpu_set_t cpuset;
    // CPU_ZERO(&cpuset);
    // CPU_SET(core_id, cpusetp)

    //io_uring_params;
    //io_uring_queue_init_params(unsigned int entries, struct io_uring *ring, struct io_uring_params *p)

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
        io_uring_cqe* cqe = nullptr; //test
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
      
    }

    close(*server_fd_opt);
    io_uring_queue_exit(&ring);

    return 0;
}