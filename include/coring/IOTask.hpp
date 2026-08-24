#pragma once

#include "coring/IOEngine.hpp"
#include <coroutine>
#include <exception>
#include <utility>
namespace Coring {
    struct IOTask {
        struct promise_type {
            std::coroutine_handle<> continuation;
            IOTask get_return_object() {
                return IOTask{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            struct FinalAwaiter {
                bool await_ready() noexcept {
                    return false;
                }
                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    auto parent =  h.promise().continuation;
                    if(parent) {
                        return parent;
                    }
                    h.destroy();
                    return std::noop_coroutine();
                }
                void await_resume() noexcept {};
            };

            std::suspend_always initial_suspend() noexcept {return {};}
            FinalAwaiter final_suspend() noexcept {return {};}
            void return_void() noexcept {}
            void unhandled_exception() {std::terminate();}
        };

        std::coroutine_handle<promise_type> handle{nullptr};

        IOTask(std::coroutine_handle<promise_type> h) : handle(h) {}
        ~IOTask() {
            if(handle) {
                handle.destroy();
            }
        }

        IOTask(const IOTask&) = delete;
        IOTask& operator=(const IOTask&) = delete;
        IOTask(IOTask&& other) : handle(std::exchange(other.handle, nullptr)) {}

        bool await_ready() const noexcept {
            return !handle || handle.done();
        }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> requesting_handle) noexcept {
            handle.promise().continuation = requesting_handle;
            return handle;
        }

        void await_resume() noexcept {}

        void detach() noexcept {
            handle = nullptr;
        }
    };


    struct promise_type_base {
        std::coroutine_handle<> continuation;
        
        std::suspend_always initial_suspend() noexcept {return {};}

        struct FinalAwaiter {
            bool await_ready() noexcept {
                return false;
            }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type_base> h) noexcept {
                auto parent = h.promise().continuation;
                if(parent) {
                    return parent;
                }
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        template<typename PromiseType>
        struct GenericFinalAwaiter {
            bool await_ready() noexcept {return false;}
            std::coroutine_handle<> await_suspend(std::coroutine_handle<PromiseType> h) noexcept {
                auto parent = h.promise().continuation;
                if(parent) {
                    return parent;
                }
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };

        void unhandled_exception() noexcept {
            std::terminate();
        }
    };

    template<typename T = void>
    struct IOTask2 {
        struct promise_type;
        using handle_type = std::coroutine_handle<promise_type>;

        struct promise_type : public promise_type_base{
            T value;

            IOTask2 get_return_object() noexcept {
                return IOTask2{handle_type::from_promise(*this)};
            }   
            auto final_suspend() noexcept { return GenericFinalAwaiter<promise_type>{};}

            void return_value(T val) requires (!std::is_same_v<T, void>) {
                value = std::move(val);
            }
        };

        handle_type handle{nullptr};

        explicit IOTask2(handle_type h) noexcept : handle(h) {}

        ~IOTask2() {
            if(handle) {
                handle.destroy();
            }
        }

        IOTask2(const IOTask2&) = delete;
        IOTask2& operator=(const IOTask2&) = delete;

        IOTask2(IOTask2&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {}
        IOTask2& operator=(IOTask2&& other) noexcept {
            if(this != &other) {
                if(handle) {
                    handle.destroy();
                }
                handle = std::exchange(other.handle, nullptr);
            }
            return *this;
        }

        bool await_ready() const noexcept {
            return !handle || handle.done();
        }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) noexcept {
            handle.promise().continuation = parent;
            return handle;
        }

        T await_resume() {
            return std::move(handle.promise().value);
        }
    };

    template<>
    struct IOTask2<void> {
        struct promise_type;
        using handle_type = std::coroutine_handle<promise_type>;
        struct promise_type : public promise_type_base {
            IOTask2 get_return_object() noexcept {
                return IOTask2{handle_type::from_promise(*this)};
            }   
            auto final_suspend() noexcept { return GenericFinalAwaiter<promise_type>{};}

            void return_void() noexcept {}
        };

         handle_type handle{nullptr};

        explicit IOTask2(handle_type h) noexcept : handle(h) {}

        ~IOTask2() {
            if(handle) {
                handle.destroy();
            }
        }

        IOTask2(const IOTask2&) = delete;
        IOTask2& operator=(const IOTask2&) = delete;

        IOTask2(IOTask2&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {}
        IOTask2& operator=(IOTask2&& other) noexcept {
            if(this != &other) {
                if(handle) {
                    handle.destroy();
                }
                handle = std::exchange(other.handle, nullptr);
            }
            return *this;
        }

        bool await_ready() const noexcept {
            return !handle || handle.done();
        }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) noexcept {
            handle.promise().continuation = parent;
            return handle;
        }

        void await_resume() {
        }

    };
    struct DetachedTask {
        struct promise_type {
            DetachedTask get_return_object() noexcept {return {};}
            std::suspend_never initial_suspend() noexcept {return {};}

            struct FinalAwaiter {
                bool await_ready() noexcept {
                    return false;
                }
                void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    h.destroy();
                }
                void await_resume() noexcept {}
            };

            FinalAwaiter final_suspend() noexcept {
                return {};
            }
            void return_void() noexcept {}
            void unhandled_exception() {std::terminate();}
        };
    };

    inline void spawn(IOTask&& task) {
        if(task.handle) {
            auto h = task.handle;
            task.detach();
            h.resume();
        }
    }
}