#include <atomic>
#include <iostream>
#include <vector>

#include "async_runtime.hpp"
#include "task_scheduler.hpp"

// =============================================================================
// Test coroutines
// =============================================================================

ethreads::coro_task<int> simple_computation() { co_return 42; }

ethreads::coro_task<int> add_numbers(int a, int b) { co_return a + b; }

ethreads::coro_task<void> increment_counter(std::atomic<int> &counter) {
  counter.fetch_add(1);
  co_return;
}

ethreads::coro_task<int> nested_coro() {
  auto inner = add_coro([]() { return 10; });
  int result = co_await inner;
  co_return result * 2;
}

ethreads::coro_task<int> yielding_coro(int iterations) {
  int sum = 0;
  for (int i = 0; i < iterations; ++i) {
    sum += i;
    co_await ethreads::yield();
  }
  co_return sum;
}

// =============================================================================
// Tests
// =============================================================================

int main() {
  int failures = 0;

  // Test 1: Basic run()
  std::cout << "Test 1: Basic run()... ";
  {
    auto task = simple_computation();
    int result = ethreads::g_runtime.run(std::move(task));
    if (result == 42) {
      std::cout << "PASS\n";
    } else {
      std::cout << "FAIL (expected 42, got " << result << ")\n";
      failures++;
    }
  }

  // Test 2: block_on()
  std::cout << "Test 2: block_on()... ";
  {
    auto task = add_numbers(10, 20);
    int result = ethreads::g_runtime.block_on(std::move(task));
    if (result == 30) {
      std::cout << "PASS\n";
    } else {
      std::cout << "FAIL (expected 30, got " << result << ")\n";
      failures++;
    }
  }

  // Test 3: when_all with values
  std::cout << "Test 3: when_all (values)... ";
  {
    auto runner = []() -> ethreads::coro_task<int> {
      std::vector<ethreads::coro_task<int>> tasks;
      for (int i = 1; i <= 5; ++i) {
        tasks.push_back(add_coro([i]() { return i * 10; }));
      }
      auto results = co_await ethreads::when_all(std::move(tasks));

      int sum = 0;
      for (int r : results)
        sum += r;
      co_return sum;
    };

    int result = ethreads::g_runtime.run(runner());
    if (result == 150) { // 10+20+30+40+50
      std::cout << "PASS\n";
    } else {
      std::cout << "FAIL (expected 150, got " << result << ")\n";
      failures++;
    }
  }

  // Test 4: when_all with void
  std::cout << "Test 4: when_all (void)... ";
  {
    std::atomic<int> counter{0};

    auto runner = [&counter]() -> ethreads::coro_task<int> {
      std::vector<ethreads::coro_task<void>> tasks;
      for (int i = 0; i < 10; ++i) {
        tasks.push_back(add_coro(increment_counter(counter)));
      }
      co_await ethreads::when_all(std::move(tasks));
      co_return counter.load();
    };

    int result = ethreads::g_runtime.run(runner());
    if (result == 10) {
      std::cout << "PASS\n";
    } else {
      std::cout << "FAIL (expected 10, got " << result << ")\n";
      failures++;
    }
  }

  // Test 5: when_any
  std::cout << "Test 5: when_any... ";
  {
    auto runner = []() -> ethreads::coro_task<int> {
      std::vector<ethreads::coro_task<int>> tasks;
      tasks.push_back(add_coro([]() { return 100; }));
      tasks.push_back(add_coro([]() { return 200; }));
      tasks.push_back(add_coro([]() { return 300; }));

      auto result = co_await ethreads::when_any(std::move(tasks));
      // One of the tasks should complete
      co_return result.value;
    };

    int result = ethreads::g_runtime.run(runner());
    if (result == 100 || result == 200 || result == 300) {
      std::cout << "PASS (got " << result << ")\n";
    } else {
      std::cout << "FAIL (got unexpected " << result << ")\n";
      failures++;
    }
  }

  // Test 6: yield()
  std::cout << "Test 6: yield()... ";
  {
    int result = ethreads::g_runtime.run(yielding_coro(5));
    if (result == 10) { // 0+1+2+3+4
      std::cout << "PASS\n";
    } else {
      std::cout << "FAIL (expected 10, got " << result << ")\n";
      failures++;
    }
  }

  // Test 7: spawn_detached
  std::cout << "Test 7: spawn_detached... ";
  {
    std::atomic<int> counter{0};

    auto runner = [&counter]() -> ethreads::coro_task<int> {
      // Spawn detached tasks
      for (int i = 0; i < 5; ++i) {
        ethreads::g_runtime.spawn_detached(add_coro(increment_counter(counter)));
      }

      // Wait a bit for them to complete by yielding
      for (int i = 0; i < 10; ++i) {
        co_await ethreads::yield();
      }

      co_return counter.load();
    };

    int result = ethreads::g_runtime.run(runner());
    if (result == 5) {
      std::cout << "PASS\n";
    } else {
      std::cout << "FAIL (expected 5, got " << result << ")\n";
      failures++;
    }
  }

  // Test 8: Nested coroutines
  std::cout << "Test 8: Nested coroutines... ";
  {
    int result = ethreads::g_runtime.run(nested_coro());
    if (result == 20) {
      std::cout << "PASS\n";
    } else {
      std::cout << "FAIL (expected 20, got " << result << ")\n";
      failures++;
    }
  }

  // Test 9: run() with void
  std::cout << "Test 9: run() with void... ";
  {
    std::atomic<int> counter{0};
    auto task = increment_counter(counter);
    ethreads::g_runtime.run(std::move(task));
    if (counter.load() == 1) {
      std::cout << "PASS\n";
    } else {
      std::cout << "FAIL (counter = " << counter.load() << ")\n";
      failures++;
    }
  }

  // Summary
  std::cout << "\n";
  if (failures == 0) {
    std::cout << "All async runtime tests passed!\n";
    return 0;
  } else {
    std::cout << failures << " test(s) failed!\n";
    return 1;
  }
}
