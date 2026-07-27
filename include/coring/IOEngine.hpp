#pragma once


#include <atomic>
#include <coroutine>
#include <vector>
namespace Coring {
   
    template <typename Engine>
    concept IOEngineConcept = requires(Engine engine, int fd, void* buf, size_t len, 
                                   typename Engine::Context_t& ctx, 
                                   std::coroutine_handle<> h) {
    { engine.async_accept(fd, ctx, h) };
    { engine.async_read(fd, buf, len, ctx, h) };
    { engine.async_write(fd, buf, len, ctx, h) };
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
        void async_accept(int listen_fd, Context& ctx, std::coroutine_handle<> h) {
            static_cast<Derived*>(this)->impl_async_accept(listen_fd, ctx, h);
        }

        template<typename Context>
        void async_read(int fd, void* buf, size_t len, Context& ctx, std::coroutine_handle<> h) {
            static_cast<Derived*>(this)->impl_async_read(fd, buf, len, ctx, h);
        }

        template<typename Context>
        void async_write(int fd, const void* buf, size_t len, Context& ctx, std::coroutine_handle<> h) {
            static_cast<Derived*>(this)->impl_async_write(fd, buf, len, ctx, h);
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