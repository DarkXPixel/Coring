#pragma once
#include "ClientSocket.hpp"
#include "coring/IOTask.hpp"
#include <memory>
#include <stdexcept>


namespace Coring {
    template<IOEngineConcept Engine>
    class Connection {
    public:
        Connection(ClientSocket<Engine>&& socket) : socket_(std::move(socket)) {

        }

        ~Connection() {
        }

        Connection(Connection&&) = default;
        Connection& operator=(Connection&&) = default;

        
        IOTask2<> handle_client();

    private:
        ClientSocket<Engine> socket_;
    };
}