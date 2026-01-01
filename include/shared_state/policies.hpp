#ifndef ETHREADS_SHARED_STATE_POLICIES_HPP
#define ETHREADS_SHARED_STATE_POLICIES_HPP

#include <atomic>
#include <cstddef>
#include <limits>
#include <mutex>
#include <shared_mutex>

namespace ethreads {

// =============================================================================
// Lock Policies
// =============================================================================

struct mutex_lock_policy {
  using mutex_type = std::mutex;
  using lock_type = std::unique_lock<std::mutex>;
};

struct shared_mutex_lock_policy {
  using mutex_type = std::shared_mutex;
  using lock_type = std::unique_lock<std::shared_mutex>;
  using shared_lock_type = std::shared_lock<std::shared_mutex>;
};

struct spinlock {
  void lock() noexcept {
    while (flag_.test_and_set(std::memory_order_acquire)) {
      while (flag_.test(std::memory_order_relaxed)) {
        // Spin with relaxed ordering for cache efficiency
      }
    }
  }

  bool try_lock() noexcept {
    return !flag_.test_and_set(std::memory_order_acquire);
  }

  void unlock() noexcept { flag_.clear(std::memory_order_release); }

private:
  std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

struct spinlock_policy {
  using mutex_type = spinlock;
  using lock_type = std::unique_lock<spinlock>;
};

// Marker for lock-free implementations
struct lockfree_policy {
  struct noop_mutex {
    void lock() noexcept {}
    void unlock() noexcept {}
    bool try_lock() noexcept { return true; }
  };

  using mutex_type = noop_mutex;
  using lock_type = std::unique_lock<noop_mutex>;
};

// =============================================================================
// Boundedness Policies
// =============================================================================

template <std::size_t N> struct bounded_policy {
  static constexpr bool is_bounded = true;
  static constexpr std::size_t max_size() noexcept { return N; }
};

struct unbounded_policy {
  static constexpr bool is_bounded = false;
  static constexpr std::size_t max_size() noexcept {
    return std::numeric_limits<std::size_t>::max();
  }
};

// =============================================================================
// Memory Ordering Policies
// =============================================================================

struct seq_cst_memory_order {
  static constexpr std::memory_order load_order = std::memory_order_seq_cst;
  static constexpr std::memory_order store_order = std::memory_order_seq_cst;
  static constexpr std::memory_order rmw_order = std::memory_order_seq_cst;
  static constexpr std::memory_order fence_order = std::memory_order_seq_cst;
};

struct acquire_release_memory_order {
  static constexpr std::memory_order load_order = std::memory_order_acquire;
  static constexpr std::memory_order store_order = std::memory_order_release;
  static constexpr std::memory_order rmw_order = std::memory_order_acq_rel;
  static constexpr std::memory_order fence_order = std::memory_order_acq_rel;
};

struct relaxed_memory_order {
  static constexpr std::memory_order load_order = std::memory_order_relaxed;
  static constexpr std::memory_order store_order = std::memory_order_relaxed;
  static constexpr std::memory_order rmw_order = std::memory_order_relaxed;
  static constexpr std::memory_order fence_order = std::memory_order_relaxed;
};

// =============================================================================
// Event Reset Policies
// =============================================================================

struct manual_reset_policy {
  static constexpr bool auto_reset = false;
};

struct auto_reset_policy {
  static constexpr bool auto_reset = true;
};

// =============================================================================
// Wait Policies (for blocking operations)
// =============================================================================

struct blocking_wait_policy {
  static constexpr bool is_blocking = true;
};

struct spinning_wait_policy {
  static constexpr bool is_blocking = false;
  static constexpr unsigned spin_count = 1000;
};

// =============================================================================
// Default Policy Bundles
// =============================================================================

struct default_sync_policies {
  using lock_policy = mutex_lock_policy;
  using memory_order_policy = acquire_release_memory_order;
  using wait_policy = blocking_wait_policy;
};

struct default_async_policies {
  using lock_policy = mutex_lock_policy;
  using memory_order_policy = acquire_release_memory_order;
};

struct high_contention_policies {
  using lock_policy = spinlock_policy;
  using memory_order_policy = acquire_release_memory_order;
  using wait_policy = spinning_wait_policy;
};

struct low_contention_policies {
  using lock_policy = shared_mutex_lock_policy;
  using memory_order_policy = acquire_release_memory_order;
  using wait_policy = blocking_wait_policy;
};

} // namespace ethreads

#endif // ETHREADS_SHARED_STATE_POLICIES_HPP
