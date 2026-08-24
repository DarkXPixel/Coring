#include "coring/IOContext.hpp"
#include "coring/ClientSocket.hpp"
#include "coring/IOEngine.hpp"
#include "coring_uring/UringEngine.hpp"
#include "coring/StaticConfig.hpp"
#include "coring/AcceptorSocket.hpp"
#include "coring/IOTask.hpp"
#include "coring/html404.hpp"
#include "coring/Connection.hpp"
#include <print>
#include <memory>


namespace Coring {
    Coring::Coring(const StaticConfig& cfg) : uring_available_(UringEngine::is_io_uring_supported()), cfg_(cfg) {


    }


    void Coring::run() {
        if(uring_available_) {
            impl_run<UringEngine>();
         } 
        //     impl_run<EpollEngine>();
        // }

    }

    template<IOEngineConcept Engine>
    DetachedTask handle_client(Connection<Engine> conn) {
        co_await conn.handle_client();
    }

    template<IOEngineConcept Engine>
    void Coring::impl_run() {
        auto engine = Engine::create();
        if(!engine.has_value()) {
            return;
        }
        auto acceptor = AcceptorSocket<Engine>::create(8888, engine.value());
        if(!acceptor.has_value()) {
            std::println("Error create acceptor socket");
            return;
        }
        
        [](AcceptorSocket<Engine> acceptor, Engine& engine) -> DetachedTask {
            while(true) {
                ClientSocket<Engine> client_socket = co_await acceptor.async_accept();
                if(client_socket.fd() >= 0) {
                    Connection<Engine> conn(std::move(client_socket));
                    handle_client(std::move(conn));
                }
            }
        }(std::move(*acceptor), *engine);

        engine->run();
    }


    template void Coring::impl_run<UringEngine>();
   // template void Coring::impl_run<EpollEngine>();
}