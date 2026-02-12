#include "io_uring_service.hpp"
#include "task_scheduler.hpp"

#include <liburing.h>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <vector>

namespace ethreads {

static constexpr unsigned RING_ENTRIES = 256;

io_uring_service::io_uring_service() {
  ring_ = new ::io_uring();

  struct io_uring_params params{};
  params.flags = IORING_SETUP_COOP_TASKRUN;

  int ret = io_uring_queue_init_params(RING_ENTRIES, ring_, &params);
  if (ret < 0) {
    std::fprintf(stderr, "[io_uring] queue_init failed: %d\n", -ret);
    delete ring_;
    ring_ = nullptr;
    return;
  }

  running_.store(true, std::memory_order_release);
  completion_thread_ = std::thread([this] { run(); });
}

io_uring_service::~io_uring_service() { shutdown(); }

io_uring_sqe* io_uring_service::get_sqe() {
  if (!ring_) return nullptr;
  return io_uring_get_sqe(ring_);
}

std::pair<io_uring_sqe*, io_uring_sqe*> io_uring_service::get_sqe_pair() {
  if (!ring_) return {nullptr, nullptr};
  auto* a = io_uring_get_sqe(ring_);
  auto* b = io_uring_get_sqe(ring_);
  if (!a || !b) return {nullptr, nullptr};
  return {a, b};
}

int io_uring_service::submit() {
  if (!ring_) return -1;
  return io_uring_submit(ring_);
}

void io_uring_service::shutdown() {
  if (!running_.exchange(false, std::memory_order_acq_rel))
    return;  // already shut down

  if (ring_) {
    // Wake the completion thread by submitting a NOP
    auto* sqe = io_uring_get_sqe(ring_);
    if (sqe) {
      io_uring_prep_nop(sqe);
      io_uring_sqe_set_data(sqe, nullptr);
      io_uring_submit(ring_);
    }
  }

  if (completion_thread_.joinable())
    completion_thread_.join();

  if (ring_) {
    io_uring_queue_exit(ring_);
    delete ring_;
    ring_ = nullptr;
  }
}

void io_uring_service::run() {
  while (running_.load(std::memory_order_acquire)) {
    struct io_uring_cqe* cqe = nullptr;
    int ret = io_uring_wait_cqe(ring_, &cqe);
    if (ret < 0) {
      if (ret == -EINTR) continue;
      break;  // fatal error
    }

    auto* comp = static_cast<io_completion*>(io_uring_cqe_get_data(cqe));
    if (comp) {
      comp->result = cqe->res;
      comp->flags = cqe->flags;
      schedule_coro_handle(comp->handle);
    }
    // comp == nullptr is our shutdown NOP — just consume it

    io_uring_cqe_seen(ring_, cqe);
  }
}

// Per-thread ring pool: one ring per coro_worker + one fallback for non-worker threads
static std::vector<std::unique_ptr<io_uring_service>> g_rings;
static thread_local io_uring_service* tl_ring = nullptr;

io_uring_service& get_io_uring_service() {
  if (tl_ring) return *tl_ring;
  if (task_scheduler::is_coro_worker)
    tl_ring = g_rings[task_scheduler::current_coro_worker_id].get();
  else
    tl_ring = g_rings.back().get();  // fallback ring for non-worker threads
  return *tl_ring;
}

void init_io_uring_service() {
  size_t n = std::thread::hardware_concurrency();
  g_rings.reserve(n + 1);  // n workers + 1 fallback
  for (size_t i = 0; i <= n; i++)
    g_rings.push_back(std::make_unique<io_uring_service>());
}

void shutdown_io_uring_service() {
  for (auto& ring : g_rings)
    ring->shutdown();
  // Don't clear vector — ring objects stay valid (ring_=nullptr after shutdown)
  // so any late get_sqe() returns nullptr safely
}

} // namespace ethreads
