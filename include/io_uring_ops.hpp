#ifndef ETHREADS_IO_URING_OPS_HPP
#define ETHREADS_IO_URING_OPS_HPP

#include "io_uring_service.hpp"
#include "coro_task.hpp"
#include "cancellation.hpp"

#include <liburing.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ethreads {

// ============================================================================
// async_accept:  co_await async_accept(listen_fd) → client_fd or negative errno
// ============================================================================

struct async_accept {
  int listen_fd;
  io_completion comp{};

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) {
    comp.handle = h;
    auto& svc = get_io_uring_service();
    auto* sqe = svc.get_sqe();
    io_uring_prep_accept(sqe, listen_fd, nullptr, nullptr, 0);
    io_uring_sqe_set_data(sqe, &comp);
    svc.submit();
  }

  int await_resume() const noexcept { return comp.result; }
};

// ============================================================================
// async_recv:  co_await async_recv(fd, buf, len) → bytes or negative errno
// ============================================================================

struct async_recv {
  int fd;
  void* buf;
  size_t len;
  io_completion comp{};

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) {
    comp.handle = h;
    auto& svc = get_io_uring_service();
    auto* sqe = svc.get_sqe();
    io_uring_prep_recv(sqe, fd, buf, static_cast<unsigned>(len), 0);
    io_uring_sqe_set_data(sqe, &comp);
    svc.submit();
  }

  int await_resume() const noexcept { return comp.result; }
};

// ============================================================================
// async_send:  co_await async_send(fd, buf, len) → bytes or negative errno
// ============================================================================

struct async_send {
  int fd;
  const void* buf;
  size_t len;
  io_completion comp{};

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) {
    comp.handle = h;
    auto& svc = get_io_uring_service();
    auto* sqe = svc.get_sqe();
    io_uring_prep_send(sqe, fd, buf, static_cast<unsigned>(len), MSG_NOSIGNAL);
    io_uring_sqe_set_data(sqe, &comp);
    svc.submit();
  }

  int await_resume() const noexcept { return comp.result; }
};

// ============================================================================
// async_send_all:  send entire buffer, handling short writes
// ============================================================================

inline coro_task<bool> async_send_all(int fd, const uint8_t* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    int n = co_await async_send{fd, data + sent, len - sent};
    if (n <= 0) co_return false;
    sent += static_cast<size_t>(n);
  }
  co_return true;
}

} // namespace ethreads

#endif // ETHREADS_IO_URING_OPS_HPP
