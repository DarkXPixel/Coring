#include <cstddef>
#include <liburing.h>
#include "../include/Task.hpp"
#include <print>
#include <iostream>
#include <sched.h>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <vector>
#include "HttpRequest.hpp"
#include "coring/AcceptorSocket.hpp"
#include "coring/ClientSocket.hpp"
#include "coring/IOEngine.hpp"
#include "coring/IOTask.hpp"
#include "coring_epoll/EpollEngine.hpp"
#include "coring_uring/UringEngine.hpp"

template<Coring::IOEngineConcept Engine>
Coring::IOTask handle_client(Coring::ClientSocket<Engine> client) {
    char buffer[1024];
    std::string raw_buffer;
    while(true) {
        size_t header_end_pos = raw_buffer.find("\r\n\r\n");

        while(header_end_pos == std::string::npos) {
            auto bytes_read = co_await client.async_read(buffer, sizeof(buffer));
            if(bytes_read <= 0) {
                co_return;
            }
            raw_buffer.append(buffer, bytes_read);
            header_end_pos = raw_buffer.find("\r\n\r\n");
        }

        std::string_view raw_headers(raw_buffer.data(), header_end_pos);
        Coring::HttpRequest request = Coring::parse_http_headers(raw_headers);

        size_t body_start_pos = header_end_pos + 4;

        size_t content_lenght = 0;
        auto it = request.headers.find("content-length");
        if(it != request.headers.end()) {
            content_lenght = std::stoull(it->second);
        }

        bool keep_alive = true;
        auto conn_it = request.headers.find("connection");
        if(conn_it != request.headers.end()) {
            std::string_view val = conn_it->second;
            if(val == "close") {
                keep_alive = false;
            } else {
                keep_alive = true;
            }
        }

        size_t bytes_already_read = raw_buffer.size() - body_start_pos;

        request.body.reserve(content_lenght);
        if(bytes_already_read > 0) {
            request.body.append(raw_buffer.data() + body_start_pos, std::min(bytes_already_read, content_lenght));
        }
        while(request.body.size() < content_lenght) {
                size_t bytes_needed = content_lenght - request.body.size();
                size_t read_size = std::min(sizeof(buffer), bytes_needed);
                auto bytes_read = co_await client.async_read(buffer, read_size);
                if(bytes_read <= 0) {
                    break;
                }
                request.body.append(buffer, bytes_read);
        }

         size_t total_processed_bytes = body_start_pos + content_lenght;
            if(raw_buffer.size() > total_processed_bytes) {
                raw_buffer.erase(0, total_processed_bytes);
            } else {
                raw_buffer.clear();
            }


        std::string_view body = "Hello World!";
        std::string response = std::format(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: {}\r\n"
                "Connection: {}\r\n"
                "{}"
                "\r\n"
                "{}", body.size(),keep_alive ? "keep-alive" : "close", keep_alive ? "Keep-Alive: timeout=5, max=1000\r\n" : "", body);

        auto bytes_written = co_await client.async_write(response.data(), response.size());
        if(!keep_alive) {
            co_return;
        }
        //co_return;
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


int main2() {
    auto engine = Coring::UringEngine::create();
    auto acceptor = Coring::AcceptorSocket<Coring::UringEngine>::create(8081, *engine);

    Coring::spawn(accept_loop(std::move(*acceptor), *engine));

    engine->run();

    return 0;
}

template<Coring::IOEngineConcept Engine>
int coring_main(int port) {
    auto engine = Engine::create();
    if(!engine.has_value()) {
        return -1;
    }

    auto acceptor = Coring::AcceptorSocket<Engine>::create(port, *engine);
    if(!acceptor.has_value()) {
        return -1;
    }

    Coring::spawn(accept_loop(std::move(*acceptor), *engine));
    engine->run();
    return 0;
}



int main() {
    constexpr int PORT = 8080;
    std::vector<std::jthread> threads;
    size_t num_threads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 1;
    std::println("threads: {}", num_threads);

    auto run_cluster = [&]<typename Engine>() {
        threads.reserve(num_threads - 1);
        for(size_t i = 0; i < num_threads - 1; ++i) {
            threads.emplace_back(coring_main<Engine>, PORT);
        }
        coring_main<Engine>(PORT);
    };

    if(Coring::UringEngine::is_io_uring_supported()) {
       run_cluster.operator()<Coring::UringEngine>();
    } else {
       run_cluster.operator()<Coring::EpollEngine>();
    }
    return 0;
}