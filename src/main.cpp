#include <cstddef>
#include <liburing.h>
#include "../include/Task.hpp"
#include <print>
#include <iostream>
#include <sched.h>
#include <sys/types.h>
#include "coring/AcceptorSocket.hpp"
#include "coring/ClientSocket.hpp"
#include "coring/IOEngine.hpp"
#include "coring/IOTask.hpp"
#include "coring_epoll/EpollEngine.hpp"

template<Coring::IOEngineConcept Engine>
Coring::IOTask handle_client(Coring::ClientSocket<Engine> client) {
    char buffer[1024];
    while(true) {
        ssize_t bytes_read = co_await client.async_read(buffer, sizeof(buffer));

        if(bytes_read <= 0) {
            break;
        }

        size_t total_written = 0;
        while(total_written < static_cast<size_t>(bytes_read)) {
            ssize_t bytes_written = co_await client.async_write(buffer + total_written, bytes_read - total_written);
            if(bytes_written <= 0) {
                break;
            }
            total_written += bytes_written;
        }
    }
}

template<Coring::IOEngineConcept Engine>
Coring::IOTask accept_loop(Coring::AcceptorSocket<Engine> acceptor, Engine& engine) {
    while(true) {
        int client_fd = co_await acceptor.async_accept();
        if(client_fd >= 0) {
            Coring::spawn(handle_client(Coring::ClientSocket<Engine>{client_fd, engine}));
        }
    }
}


int main() {
    auto engine = Coring::EpollEngine::create();

    auto acceptor = Coring::AcceptorSocket<Coring::EpollEngine>::create(8080, *engine);

    Coring::spawn(accept_loop(std::move(*acceptor), *engine));

    engine->run();

    // constexpr int16_t PORT = 8080;
    // constexpr uint32_t QUEUE_DEPTH = 256;


    // // cpu_set_t cpuset;
    // // CPU_ZERO(&cpuset);
    // // CPU_SET(core_id, cpusetp)

    // //io_uring_params;
    // //io_uring_queue_init_params(unsigned int entries, struct io_uring *ring, struct io_uring_params *p)

    // io_uring ring;
    // if(io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
    //     std::println(stderr, "Failed to init io_uring");
    //     return 1;
    // }

    // auto server_fd_opt = Coring::create_server_socket(PORT);
    // if(!server_fd_opt.has_value()) {
    //     std::println(stderr, "Failed to create server socket");
    //     return 1;
    // }

    // std::println("Server started on port {}...", PORT);
    // Coring::accept_loop(&ring, *server_fd_opt);

    // while(true) {
    //     io_uring_cqe* cqe = nullptr; //test
    //     int ret = io_uring_wait_cqe(&ring, &cqe);
    //     if(ret < 0) {
    //         std::println(stderr, "io_uring_wait_cqe failed: {}", ret);
    //         break;
    //     }
    //     auto* ctx = static_cast<Coring::IOContext*>(io_uring_cqe_get_data(cqe));
    //     if(ctx) {
    //         ctx->resume(cqe->res);
    //     }
    //     io_uring_cqe_seen(&ring, cqe);
      
    // }

    // close(*server_fd_opt);
    // io_uring_queue_exit(&ring);

    return 0;
}