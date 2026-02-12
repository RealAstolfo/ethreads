#ifndef ETHREADS_IO_URING_OPS_HPP
#define ETHREADS_IO_URING_OPS_HPP

#include "io_uring_service.hpp"
#include "coro_task.hpp"
#include "cancellation.hpp"

#include <liburing.h>
#include <cerrno>
#include <chrono>
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
    if (!sqe) {
      comp.result = -ENOMEM;
      schedule_coro_handle(h);
      return;
    }
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
    if (!sqe) {
      comp.result = -ENOMEM;
      schedule_coro_handle(h);
      return;
    }
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
    if (!sqe) {
      comp.result = -ENOMEM;
      schedule_coro_handle(h);
      return;
    }
    io_uring_prep_send(sqe, fd, buf, static_cast<unsigned>(len), MSG_NOSIGNAL);
    io_uring_sqe_set_data(sqe, &comp);
    svc.submit();
  }

  int await_resume() const noexcept { return comp.result; }
};

// ============================================================================
// async_send_timeout:  send with io_uring linked timeout
//   Returns bytes sent, -ECANCELED on timeout, or other negative errno
// ============================================================================

struct async_send_timeout {
  int fd;
  const void* buf;
  size_t len;
  std::chrono::milliseconds timeout;

  io_completion send_comp{};
  io_completion timeout_comp{};  // handle left null — completion thread skips it
  __kernel_timespec ts{};

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) {
    send_comp.handle = h;
    // timeout_comp.handle stays default (null) — no resume on timeout CQE

    ts.tv_sec = static_cast<long long>(timeout.count() / 1000);
    ts.tv_nsec = static_cast<long long>((timeout.count() % 1000) * 1000000);

    auto& svc = get_io_uring_service();
    auto [sqe_send, sqe_timeout] = svc.get_sqe_pair();
    if (!sqe_send) {
      send_comp.result = -ENOMEM;
      schedule_coro_handle(h);
      return;
    }

    // SQE 1: send, linked to timeout
    io_uring_prep_send(sqe_send, fd, buf, static_cast<unsigned>(len), MSG_NOSIGNAL);
    io_uring_sqe_set_data(sqe_send, &send_comp);
    sqe_send->flags |= IOSQE_IO_LINK;

    // SQE 2: linked timeout — cancels send if it hasn't completed
    io_uring_prep_link_timeout(sqe_timeout, &ts, 0);
    io_uring_sqe_set_data(sqe_timeout, &timeout_comp);

    svc.submit();
  }

  int await_resume() const noexcept { return send_comp.result; }
};

// ============================================================================
// async_send_all:  send entire buffer, handling short writes
//   Optional timeout per individual send syscall (not total)
// ============================================================================

inline coro_task<bool> async_send_all(
    int fd, const uint8_t* data, size_t len,
    std::chrono::milliseconds timeout = {}) {
  size_t sent = 0;
  while (sent < len) {
    int n;
    if (timeout.count() > 0)
      n = co_await async_send_timeout{fd, data + sent, len - sent, timeout};
    else
      n = co_await async_send{fd, data + sent, len - sent};
    if (n <= 0) co_return false;
    sent += static_cast<size_t>(n);
  }
  co_return true;
}

} // namespace ethreads

#endif // ETHREADS_IO_URING_OPS_HPP
