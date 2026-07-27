#pragma once

#include "coring/IOEngine.hpp"
#include <coroutine>
#include <exception>
#include <utility>
namespace Coring {
    struct IOTask {
        struct promise_type {
            IOTask get_return_object() {
                return IOTask{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            struct FinalAwaiter {
                bool await_ready() noexcept {
                    return false;
                }
                void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    h.destroy();
                }
                void await_resume() noexcept {};
            };

            std::suspend_never initial_suspend() noexcept {return {};}
            FinalAwaiter final_suspend() noexcept {return {};}
            void return_void() noexcept {}
            void unhandled_exception() {std::terminate();}
        };

        std::coroutine_handle<promise_type> handle;

        IOTask(std::coroutine_handle<promise_type> h) : handle(h) {}
        ~IOTask() {
            if(handle && handle.done()) {
                handle.destroy();
            }
        }

        IOTask(const IOTask&) = delete;
        IOTask& operator=(const IOTask&) = delete;
        IOTask(IOTask&& other) : handle(std::exchange(other.handle, nullptr)) {}

        void detach() noexcept {
            handle = nullptr;
        }
    };

    template<typename CoroTask> 
    void spawn(CoroTask&& task) {
        task.detach();
    }
}