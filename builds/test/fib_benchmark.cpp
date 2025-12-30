#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include "task_scheduler.hpp"

// Deliberately slow Fibonacci to simulate CPU-intensive work
// Uses naive recursion up to a depth, then iterative
std::uint64_t fib_slow(int n, int naive_depth = 25) {
  if (n <= 1)
    return n;

  if (n <= naive_depth) {
    // Naive recursive (slow, O(2^n))
    return fib_slow(n - 1, naive_depth) + fib_slow(n - 2, naive_depth);
  }

  // For larger n, use iterative for the upper portion
  std::uint64_t fib_naive = fib_slow(naive_depth, naive_depth);
  std::uint64_t fib_naive_minus1 = fib_slow(naive_depth - 1, naive_depth);

  std::uint64_t a = fib_naive_minus1;
  std::uint64_t b = fib_naive;
  for (int i = naive_depth + 1; i <= n; ++i) {
    std::uint64_t c = a + b;
    a = b;
    b = c;
  }
  return b;
}

// Simple iterative for verification
std::uint64_t fib_iterative(int n) {
  if (n <= 1)
    return n;
  std::uint64_t a = 0, b = 1;
  for (int i = 2; i <= n; ++i) {
    std::uint64_t c = a + b;
    a = b;
    b = c;
  }
  return b;
}

// Coroutine wrapper
ethreads::coro_task<std::uint64_t> fib_coro(int n) {
  co_return fib_slow(n);
}

template <typename Func>
double measure_time_ms(Func &&func) {
  auto start = std::chrono::high_resolution_clock::now();
  func();
  auto end = std::chrono::high_resolution_clock::now();
  return std::chrono::duration<double, std::milli>(end - start).count();
}

int main() {
  std::cout << "Fibonacci Parallel Computation Benchmark\n";
  std::cout << "=========================================\n";
  std::cout << "Using deliberately slow Fibonacci to simulate CPU-bound work\n";
  std::cout << "Hardware threads: " << std::thread::hardware_concurrency()
            << "\n\n";

  // Generate a list of Fibonacci indices to compute
  // Each fib_slow(35) takes ~50-100ms on typical hardware
  std::vector<int> fib_indices;
  for (int i = 0; i < 16; ++i) {
    fib_indices.push_back(35); // All same value for consistent workload
  }
  const std::size_t NUM_COMPUTATIONS = fib_indices.size();
  const int NUM_RUNS = 3;

  std::cout << "Computing " << NUM_COMPUTATIONS
            << " independent Fib(35) calculations\n";
  std::cout << "Each timing is average of " << NUM_RUNS << " runs\n\n";

  std::vector<std::uint64_t> results_st(NUM_COMPUTATIONS);
  std::vector<std::uint64_t> results_coro(NUM_COMPUTATIONS);
  std::vector<std::uint64_t> results_task(NUM_COMPUTATIONS);

  // Warm up
  std::cout << "Warming up... " << std::flush;
  volatile auto _ = fib_slow(35);
  (void)_;
  std::cout << "done\n\n";

  // ===== Single-threaded (sequential) =====
  std::cout << "Single-threaded (sequential)... " << std::flush;
  double time_st = 0;
  for (int run = 0; run < NUM_RUNS; ++run) {
    time_st += measure_time_ms([&]() {
      for (std::size_t i = 0; i < NUM_COMPUTATIONS; ++i) {
        results_st[i] = fib_slow(fib_indices[i]);
      }
    });
  }
  time_st /= NUM_RUNS;
  std::cout << time_st << " ms\n";

  // ===== Coroutines (parallel) =====
  std::cout << "Coroutines (parallel)... " << std::flush;
  double time_coro = 0;
  for (int run = 0; run < NUM_RUNS; ++run) {
    time_coro += measure_time_ms([&]() {
      std::vector<ethreads::coro_task<std::uint64_t>> tasks;
      tasks.reserve(NUM_COMPUTATIONS);

      // Launch all coroutines
      for (std::size_t i = 0; i < NUM_COMPUTATIONS; ++i) {
        tasks.push_back(add_coro(fib_coro(fib_indices[i])));
      }

      // Collect results
      for (std::size_t i = 0; i < NUM_COMPUTATIONS; ++i) {
        results_coro[i] = tasks[i].get();
      }
    });
  }
  time_coro /= NUM_RUNS;
  std::cout << time_coro << " ms\n";

  // ===== Tasks (parallel) =====
  std::cout << "Tasks (parallel)... " << std::flush;
  double time_task = 0;
  for (int run = 0; run < NUM_RUNS; ++run) {
    time_task += measure_time_ms([&]() {
      std::vector<std::future<std::uint64_t>> futures;
      futures.reserve(NUM_COMPUTATIONS);

      // Launch all tasks
      for (std::size_t i = 0; i < NUM_COMPUTATIONS; ++i) {
        int n = fib_indices[i];
        futures.push_back(add_task([n]() { return fib_slow(n); }));
      }

      // Collect results
      for (std::size_t i = 0; i < NUM_COMPUTATIONS; ++i) {
        results_task[i] = futures[i].get();
      }
    });
  }
  time_task /= NUM_RUNS;
  std::cout << time_task << " ms\n";

  // Verify correctness
  std::uint64_t expected = fib_iterative(35);
  bool correct = true;
  for (std::size_t i = 0; i < NUM_COMPUTATIONS; ++i) {
    if (results_st[i] != expected || results_coro[i] != expected ||
        results_task[i] != expected) {
      std::cerr << "ERROR: Mismatch at index " << i << "\n";
      std::cerr << "  Expected: " << expected << "\n";
      std::cerr << "  Single: " << results_st[i] << "\n";
      std::cerr << "  Coro: " << results_coro[i] << "\n";
      std::cerr << "  Task: " << results_task[i] << "\n";
      correct = false;
    }
  }

  if (!correct) {
    return 1;
  }

  // Results summary
  std::cout << "\n";
  std::cout << "+---------------------+------------+---------+\n";
  std::cout << "| Method              | Time (ms)  | Speedup |\n";
  std::cout << "+---------------------+------------+---------+\n";
  printf("| Single-threaded     | %10.2f | %7.2fx |\n", time_st, 1.0);
  printf("| Coroutines          | %10.2f | %7.2fx |\n", time_coro,
         time_st / time_coro);
  printf("| Tasks               | %10.2f | %7.2fx |\n", time_task,
         time_st / time_task);
  std::cout << "+---------------------+------------+---------+\n";

  std::cout << "\nFib(35) = " << expected << "\n";
  std::cout << "\nIdeal speedup with " << std::thread::hardware_concurrency()
            << " cores: " << std::thread::hardware_concurrency() << "x\n";

  return 0;
}
