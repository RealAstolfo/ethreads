#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "async_runtime.hpp"
#include "task_scheduler.hpp"

// =============================================================================
// Strategy 1: Locked shared cache (baseline)
// =============================================================================
class LockedFibCache {
public:
  long long get(int n) {
    if (n <= 1) return n;

    {
      std::shared_lock lock(mutex_);
      auto it = cache_.find(n);
      if (it != cache_.end()) return it->second;
    }

    long long result = get(n - 1) + get(n - 2);

    {
      std::unique_lock lock(mutex_);
      cache_[n] = result;
    }
    return result;
  }

  void clear() {
    std::unique_lock lock(mutex_);
    cache_.clear();
  }

private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<int, long long> cache_;
};

// =============================================================================
// Strategy 2: Lock-free shared cache using atomic array
// =============================================================================
class LockFreeFibCache {
public:
  static constexpr long long NOT_COMPUTED = LLONG_MIN;
  static constexpr int MAX_N = 200000;

  LockFreeFibCache() : cache_(MAX_N + 1) {
    clear();
  }

  long long get(int n) {
    if (n <= 1) return n;
    if (n > MAX_N) return compute_uncached(n);

    // Try to read existing value
    long long val = cache_[n].load(std::memory_order_acquire);
    if (val != NOT_COMPUTED) return val;

    // Compute (might be redundant with other threads - that's OK)
    long long result = get(n - 1) + get(n - 2);

    // Try to store - if someone else stored first, use their value
    long long expected = NOT_COMPUTED;
    if (cache_[n].compare_exchange_strong(expected, result,
                                           std::memory_order_release,
                                           std::memory_order_acquire)) {
      return result;  // We stored it
    }
    return expected;  // Someone else stored it, use their value
  }

  void clear() {
    for (auto& v : cache_) {
      v.store(NOT_COMPUTED, std::memory_order_relaxed);
    }
    std::atomic_thread_fence(std::memory_order_release);
  }

private:
  long long compute_uncached(int n) {
    if (n <= 1) return n;
    return compute_uncached(n - 1) + compute_uncached(n - 2);
  }

  std::vector<std::atomic<long long>> cache_;
};

// Global caches
LockedFibCache g_locked_cache;
LockFreeFibCache g_lockfree_cache;

// Query fib for a list of indices with fresh local cache
long long fib_query_local(const std::vector<int>& queries) {
  std::unordered_map<int, long long> cache;

  std::function<long long(int)> fib = [&](int x) -> long long {
    if (x <= 1) return x;
    auto it = cache.find(x);
    if (it != cache.end()) return it->second;
    long long result = fib(x - 1) + fib(x - 2);
    cache[x] = result;
    return result;
  };

  long long sum = 0;
  for (int q : queries) {
    sum += fib(q);
  }
  return sum;
}

// Query fib for a list of indices with locked shared cache
long long fib_query_locked(const std::vector<int>& queries) {
  long long sum = 0;
  for (int q : queries) {
    sum += g_locked_cache.get(q);
  }
  return sum;
}

// Query fib for a list of indices with lock-free shared cache
long long fib_query_lockfree(const std::vector<int>& queries) {
  long long sum = 0;
  for (int q : queries) {
    sum += g_lockfree_cache.get(q);
  }
  return sum;
}

// The coroutine main - replaces traditional main()
ethreads::coro_task<int> coro_main() {
  using clock = std::chrono::high_resolution_clock;

  constexpr int NUM_TASKS = 16;
  constexpr int MAX_FIB = 50000;
  constexpr int QUERIES_PER_TASK = 100000;

  std::cout << "=== Memoization Strategy Comparison ===\n";
  std::cout << "Pattern: " << NUM_TASKS << " tasks, each making " << QUERIES_PER_TASK
            << " random fib queries (1.." << MAX_FIB << ")\n";
  std::cout << "This pattern has HIGH overlap - cache sharing should help!\n\n";

  // Generate random queries - same values queried by multiple tasks
  // This is where shared caches shine!
  std::vector<std::vector<int>> task_queries(NUM_TASKS);
  for (int t = 0; t < NUM_TASKS; ++t) {
    task_queries[t].reserve(QUERIES_PER_TASK);
    for (int q = 0; q < QUERIES_PER_TASK; ++q) {
      // Pseudo-random but deterministic, with high overlap between tasks
      int fib_n = ((q * 7919 + t * 13) % MAX_FIB) + 1;
      task_queries[t].push_back(fib_n);
    }
  }

  // =========================================================================
  // Strategy 1: Local caches (no sharing - each task recomputes everything)
  // =========================================================================
  std::cout << "--- Strategy 1: Local caches (no sharing) ---\n";

  auto s1_seq_start = clock::now();
  for (int i = 0; i < NUM_TASKS; ++i) {
    fib_query_local(task_queries[i]);
  }
  auto s1_seq_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      clock::now() - s1_seq_start).count();

  std::vector<ethreads::coro_task<long long>> tasks1;
  for (int i = 0; i < NUM_TASKS; ++i) {
    const auto& queries = task_queries[i];
    tasks1.push_back(add_coro([&queries]() { return fib_query_local(queries); }));
  }
  auto s1_par_start = clock::now();
  co_await ethreads::when_all(std::move(tasks1));
  auto s1_par_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      clock::now() - s1_par_start).count();

  std::cout << "  Sequential: " << s1_seq_ms << " ms\n";
  std::cout << "  Parallel:   " << s1_par_ms << " ms\n";

  // =========================================================================
  // Strategy 2: Locked shared cache
  // =========================================================================
  std::cout << "\n--- Strategy 2: Locked shared cache ---\n";

  g_locked_cache.clear();
  auto s2_seq_start = clock::now();
  for (int i = 0; i < NUM_TASKS; ++i) {
    fib_query_locked(task_queries[i]);
  }
  auto s2_seq_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      clock::now() - s2_seq_start).count();

  g_locked_cache.clear();
  std::vector<ethreads::coro_task<long long>> tasks2;
  for (int i = 0; i < NUM_TASKS; ++i) {
    const auto& queries = task_queries[i];
    tasks2.push_back(add_coro([&queries]() { return fib_query_locked(queries); }));
  }
  auto s2_par_start = clock::now();
  co_await ethreads::when_all(std::move(tasks2));
  auto s2_par_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      clock::now() - s2_par_start).count();

  std::cout << "  Sequential: " << s2_seq_ms << " ms\n";
  std::cout << "  Parallel:   " << s2_par_ms << " ms\n";

  // =========================================================================
  // Strategy 3: Lock-free shared cache
  // =========================================================================
  std::cout << "\n--- Strategy 3: Lock-free shared cache ---\n";

  g_lockfree_cache.clear();
  auto s3_seq_start = clock::now();
  for (int i = 0; i < NUM_TASKS; ++i) {
    fib_query_lockfree(task_queries[i]);
  }
  auto s3_seq_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      clock::now() - s3_seq_start).count();

  g_lockfree_cache.clear();
  std::vector<ethreads::coro_task<long long>> tasks3;
  for (int i = 0; i < NUM_TASKS; ++i) {
    const auto& queries = task_queries[i];
    tasks3.push_back(add_coro([&queries]() { return fib_query_lockfree(queries); }));
  }
  auto s3_par_start = clock::now();
  co_await ethreads::when_all(std::move(tasks3));
  auto s3_par_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      clock::now() - s3_par_start).count();

  std::cout << "  Sequential: " << s3_seq_ms << " ms\n";
  std::cout << "  Parallel:   " << s3_par_ms << " ms\n";

  // =========================================================================
  // Summary
  // =========================================================================
  std::cout << "\n=== Summary ===\n";
  std::cout << "                    Sequential    Parallel\n";
  std::cout << "Local caches:       " << s1_seq_ms << " ms         " << s1_par_ms << " ms\n";
  std::cout << "Locked cache:       " << s2_seq_ms << " ms         " << s2_par_ms << " ms\n";
  std::cout << "Lock-free cache:    " << s3_seq_ms << " ms         " << s3_par_ms << " ms\n";
  std::cout << "\n";

  std::cout << "Parallel comparison:\n";
  std::cout << "  Lock-free vs Local:  " << (double)s1_par_ms / s3_par_ms << "x faster\n";
  std::cout << "  Lock-free vs Locked: " << (double)s2_par_ms / s3_par_ms << "x faster\n";
  std::cout << "\n";
  std::cout << "With overlapping queries, lock-free cache sharing wins!\n";

  co_return 0;
}

CORO_MAIN  // Generates: int main() { return g_runtime.run_main(coro_main()); }
