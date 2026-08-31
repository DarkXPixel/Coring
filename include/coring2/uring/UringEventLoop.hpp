#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <optional>
#include <span>
#include <stdexcept>
#include <sys/socket.h>
#include "coring2/IOContext.hpp"
#include "coring2/TcpConnection.hpp"
#include "coring2/WorkerThreadLoop.hpp"
#include "coring2/uring/UringBufferRing.hpp"
#include "UringBufferRing.hpp"
#include <time.h>

namespace Coring2 {
    class UringLoop {
        io_uring ring_;
        bool is_init_{false};

        static constexpr std::size_t BGID_SMALL = 0;
        BufferGroup buffers_[1];

    public:
        bool init(uint32_t queue_depth = 4096) {
            int ret = io_uring_queue_init(queue_depth, &ring_, 0);
            if(ret == 0) {
                auto small_buffer = setup_buffer_ring(BGID_SMALL, 16384, 4096);
                //auto small_buffer = setup_buffer_ring(BGID_SMALL, 2, 4096);
                if(!small_buffer.has_value()) {
                    return false;
                }
                buffers_[BGID_SMALL] = *small_buffer;
                is_init_ = true;
                return true;
            }
            return false;
        }
        ~UringLoop() noexcept {
            if(is_init_) {
                io_uring_queue_exit(&ring_);
            }
        }


        std::optional<BufferGroup> setup_buffer_ring(uint16_t bgid, uint32_t count, uint32_t size) {
            BufferGroup group;
            group.bgid = bgid;
            group.buf_count = count;
            group.buf_size = size;

            const std::size_t ring_mem_size = sizeof(io_uring_buf) * count;

            void* ring_ptr = nullptr;
            posix_memalign(&ring_ptr, 4096, ring_mem_size);
            group.buf_ring = static_cast<io_uring_buf_ring*>(ring_ptr);

            const std::size_t total_data_size = static_cast<std::size_t>(count) * size;
            group.data_arena = static_cast<std::byte*>(malloc(total_data_size)); // temp
            io_uring_buf_reg reg{};
            reg.ring_addr = reinterpret_cast<uint64_t>(group.buf_ring);
            reg.ring_entries = count;
            reg.bgid = bgid;

            int ret = io_uring_register_buf_ring(&ring_, &reg, 0);
            if(ret < 0) {
                return std::nullopt;
            }

            io_uring_buf_ring_init(group.buf_ring);
            for(uint32_t i = 0; i < count; ++i) {
                std::byte* buf_addr = group.data_arena + (i * size);
                io_uring_buf_ring_add(group.buf_ring, buf_addr, size, i, io_uring_buf_ring_mask(count), i);
            }

            io_uring_buf_ring_advance(group.buf_ring, count);

            return group;
        }

        void submit_read(TcpConnection<UringLoop>* conn, uint32_t timeout = 0) noexcept {
            io_uring_sqe* sqe1;
            io_uring_sqe* sqe2;
            if(timeout) {
                auto sqe_pair = get_sqe_pair();
                sqe1 = sqe_pair.first;
                sqe2 = sqe_pair.second;
            } else {
                sqe1 = get_sqe();
            }
            io_uring_prep_read(sqe1, conn->socket_fd_, nullptr, 0, 0);
            sqe1->flags |= IOSQE_BUFFER_SELECT | IOSQE_IO_LINK;
            sqe1->buf_group = BGID_SMALL;
            io_uring_sqe_set_data64(sqe1, encode_user_data_bgid(conn, IOType::Read, BGID_SMALL));
            if(timeout) {
                __kernel_timespec ts;
                ts.tv_sec = (time_t)timeout;
                ts.tv_nsec = (long long)((timeout - ts.tv_sec) * 1e9);

                io_uring_prep_link_timeout(sqe2, &ts, 0);
                sqe2->user_data = 0;
            }
        }

        void submit_write(TcpConnection<UringLoop>* conn, std::span<const std::byte> buf) noexcept {
            io_uring_sqe* sqe = get_sqe();
            io_uring_prep_write(sqe, conn->socket_fd_, buf.data(), buf.size(), 0);
            io_uring_sqe_set_data64(sqe, encode_user_data(conn, IOType::Write));
        };

        void submit_accept(IOAcceptContext* ctx) noexcept {
            io_uring_sqe* sqe = get_sqe();
            ctx->reset_len();
            io_uring_prep_accept(sqe, ctx->fd, reinterpret_cast<sockaddr*>(&ctx->client_addr), &ctx->addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
            io_uring_sqe_set_data64(sqe, encode_user_data(ctx, IOType::Accept));
        }

        void poll_events(int timeout_ms) noexcept {
            io_uring_submit_and_wait(&ring_, 1);
            unsigned head;
            unsigned count = 0;
            io_uring_cqe* cqe;

            uint32_t small_bufs_to_advance = 0;

            io_uring_for_each_cqe(&ring_, head, cqe) {
                uint64_t raw = io_uring_cqe_get_data64(cqe);
                auto type = raw ? decode_type(raw) : IOType::Timer;
                void* ptr = decode_ptr(raw);
                switch (type) {
                case IOType::Accept:
                    {
                        auto* accept_ctx = static_cast<IOAcceptContext*>(ptr);
                        auto* thread_loop = static_cast<WorkerThreadLoop<UringLoop>*>(accept_ctx->data);
                        thread_loop->on_accept(accept_ctx, cqe->res);
                        break;
                    }
                case IOType::Read:
                    {
                        auto* connection = static_cast<TcpConnection<UringLoop>*>(ptr);
                        const bool has_buffer = (cqe->flags & IORING_CQE_F_BUFFER) != 0;

                        if (has_buffer) {
                            const uint16_t bid = static_cast<uint16_t>(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
                            const uint16_t bgid = decode_bgid(raw);

                            auto& group = buffers_[bgid];
                            std::byte* buf_ptr = group.data_arena + (bid * group.buf_size);

                            if (cqe->res > 0) {
                                connection->on_read_completed(*this, cqe->res, buf_ptr);
                            } else {
                                connection->on_read_completed(*this, cqe->res, nullptr);
                            }

                            io_uring_buf_ring_add(
                                group.buf_ring, 
                                buf_ptr, 
                                group.buf_size, 
                                bid, 
                                io_uring_buf_ring_mask(group.buf_count), 
                                small_bufs_to_advance++
                            );
                        } else {
                            connection->on_read_completed(*this, cqe->res, nullptr);
                        }

                        break;
                    }
                case IOType::Write:
                    {
                        auto* connection = static_cast<TcpConnection<UringLoop>*>(ptr);
                        connection->on_write_completed(*this, cqe->res);
                        break;
                    }
                case IOType::Close:
                    {
                        auto* connection = static_cast<TcpConnection<UringLoop>*>(ptr);
                        connection->on_close_completed(*this, cqe->res);
                        break;
                    }
                case IOType::Cancel:
                    {
                        auto* connection = static_cast<TcpConnection<UringLoop>*>(ptr);
                        connection->on_cancel_completed(*this, cqe->res);
                        break;
                    }
                default:
                    {
                    //    std::println("Unknown op");
                    }
                }
                ++count;
            }

            if(small_bufs_to_advance > 0) {
                io_uring_buf_ring_advance(buffers_[0].buf_ring, small_bufs_to_advance);
            }
            if(count > 0) {
                io_uring_cq_advance(&ring_, count);
            }

        }

        void cancel_fd(TcpConnection<UringLoop>* conn) noexcept {
            io_uring_sqe* sqe = get_sqe();
            io_uring_prep_cancel_fd(sqe, conn->socket_fd_, IORING_ASYNC_CANCEL_ALL);
            io_uring_sqe_set_data64(sqe, encode_user_data(conn, IOType::Cancel));
        }

        void close_fd(TcpConnection<UringLoop>* conn) noexcept {
            io_uring_sqe* sqe = get_sqe();
            io_uring_prep_close(sqe, conn->socket_fd_);
            io_uring_sqe_set_data64(sqe, encode_user_data(conn, IOType::Close));
        }


    private:
        inline io_uring_sqe* get_sqe() {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if(!sqe) {
                io_uring_submit(&ring_);
                sqe = io_uring_get_sqe(&ring_);
                if(!sqe) {
                    throw std::runtime_error("error get sqe");
                }
            }
            return sqe;
        }

        struct SQEPair {
            io_uring_sqe* first;
            io_uring_sqe* second;
        };

        inline SQEPair get_sqe_pair() {
            io_uring_sqe* sqe1 = io_uring_get_sqe(&ring_);
            io_uring_sqe* sqe2 = nullptr;

            if (sqe1) {
                sqe2 = io_uring_get_sqe(&ring_);
            }
            if (!sqe1 || !sqe2) {
                io_uring_submit(&ring_);

                sqe1 = io_uring_get_sqe(&ring_);
                sqe2 = io_uring_get_sqe(&ring_);

                if (!sqe1 || !sqe2) {
                    throw std::runtime_error("Critical: unable to get 2 SQE slots even after submit");
                }
            }

            return {sqe1, sqe2};
        }

    };
}