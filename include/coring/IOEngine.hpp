#pragma once


#include <atomic>
#include <coroutine>
#include <sys/socket.h>
#include <system_error>
#include <vector>
#include <expected>
#include <netinet/in.h>
namespace Coring {
   
    template <typename Engine>
    concept IOEngineConcept = requires(Engine engine, int fd, void* buf, size_t len, 
                                   typename Engine::Context_t& ctx, 
                                   std::coroutine_handle<> h, struct sockaddr* addr, socklen_t* socklen) {
    typename Engine::Context_t;
    typename Engine::AcceptAwaitable_t;
    typename Engine::ReadAwaitable_t;
    typename Engine::WriteAwaitable_t;                            

    {Engine::create()} ->std::same_as<std::expected<Engine, std::error_code>>;
    { engine.async_accept(fd, addr, socklen, ctx) };
    { engine.async_read(fd, buf, len, ctx) };
    { engine.async_write(fd, buf, len, ctx) };
    { engine.unregister_fd(fd) };
    { engine.schedule(h) };
};

    template<typename Derived>
    class IOEngineBase {
    protected:
        std::vector<std::coroutine_handle<>> ready_tasks_;
        bool running_;

    public:
        void schedule(std::coroutine_handle<> h) {
            if(h && !h.done()) {
                ready_tasks_.push_back(h);
            }
        }

        template<typename Context>
        void async_accept(int listen_fd, struct sockaddr* addr, socklen_t* len, Context& ctx) {
            static_cast<Derived*>(this)->impl_async_accept(listen_fd, addr, len, ctx);
        }

        template<typename Context>
        void async_read(int fd, void* buf, size_t len, Context& ctx) {
            static_cast<Derived*>(this)->impl_async_read(fd, buf, len, ctx);
        }

        template<typename Context>
        void async_write(int fd, const void* buf, size_t len, Context& ctx) {
            static_cast<Derived*>(this)->impl_async_write(fd, buf, len, ctx);
        }

        void unregister_fd(int fd) {
            static_cast<Derived*>(this)->impl_unregister_fd(fd);
        }

        void run() {
            running_ = true;
            static_cast<Derived*>(this)->impl_run();
        }

        void stop() {
            running_ = false;
        }
    };
}