#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "task_scheduler.hpp"

// Test 1: Simple coroutine with return value
ethreads::coro_task<int> simple_coro() { co_return 42; }

// Test 2: Coroutine that does computation
ethreads::coro_task<float> compute_pi() { co_return 3.14159f; }

// Test 3: Coroutine that throws exception
ethreads::coro_task<void> throws_coro() {
  throw std::runtime_error("coro error");
  co_return;
}

// Test 4: Coroutine with multiple co_await
ethreads::coro_task<int> chained_coro() {
  auto t1 = add_coro([]() { return 10; });
  auto t2 = add_coro([]() { return 20; });
  auto t3 = add_coro([]() { return 12; });

  int a = t1.get();
  int b = t2.get();
  int c = t3.get();

  co_return a + b + c;
}

// Test 5: Nested coroutines
ethreads::coro_task<int> inner_coro() { co_return 5; }

ethreads::coro_task<int> outer_coro() {
  auto inner = add_coro(inner_coro());
  int x = inner.get();
  co_return x * 2;
}

// Test 6: Void coroutine
ethreads::coro_task<void> void_coro(std::atomic<int> &counter) {
  counter.fetch_add(1);
  co_return;
}

int main() {
  int failures = 0;

  // Test 1: Simple coroutine
  std::cout << "Test 1: Simple coroutine... ";
  {
    auto task = add_coro(simple_coro());
    int result = task.get();
    if (result == 42) {
      std::cout << "PASS" << std::endl;
    } else {
      std::cout << "FAIL (expected 42, got " << result << ")" << std::endl;
      failures++;
    }
  }

  // Test 2: Coroutine wrapping regular callable
  std::cout << "Test 2: Wrapped callable... ";
  {
    auto task = add_coro([]() { return 100; });
    int result = task.get();
    if (result == 100) {
      std::cout << "PASS" << std::endl;
    } else {
      std::cout << "FAIL (expected 100, got " << result << ")" << std::endl;
      failures++;
    }
  }

  // Test 3: Exception propagation
  std::cout << "Test 3: Exception propagation... ";
  {
    auto task = add_coro(throws_coro());
    try {
      task.get();
      std::cout << "FAIL (exception not thrown)" << std::endl;
      failures++;
    } catch (const std::runtime_error &e) {
      if (std::string(e.what()) == "coro error") {
        std::cout << "PASS" << std::endl;
      } else {
        std::cout << "FAIL (wrong message: " << e.what() << ")" << std::endl;
        failures++;
      }
    } catch (...) {
      std::cout << "FAIL (wrong exception type)" << std::endl;
      failures++;
    }
  }

  // Test 4: Chained coroutines with multiple awaits
  std::cout << "Test 4: Chained coroutines... ";
  {
    auto task = add_coro(chained_coro());
    int result = task.get();
    if (result == 42) { // 10 + 20 + 12
      std::cout << "PASS" << std::endl;
    } else {
      std::cout << "FAIL (expected 42, got " << result << ")" << std::endl;
      failures++;
    }
  }

  // Test 5: Nested coroutines
  std::cout << "Test 5: Nested coroutines... ";
  {
    auto task = add_coro(outer_coro());
    int result = task.get();
    if (result == 10) { // 5 * 2
      std::cout << "PASS" << std::endl;
    } else {
      std::cout << "FAIL (expected 10, got " << result << ")" << std::endl;
      failures++;
    }
  }

  // Test 6: Void coroutine
  std::cout << "Test 6: Void coroutine... ";
  {
    std::atomic<int> counter{0};
    auto task = add_coro(void_coro(counter));
    task.get();
    if (counter.load() == 1) {
      std::cout << "PASS" << std::endl;
    } else {
      std::cout << "FAIL (counter = " << counter.load() << ")" << std::endl;
      failures++;
    }
  }

  // Test 7: Multiple concurrent coroutines
  std::cout << "Test 7: Concurrent coroutines... ";
  {
    std::atomic<int> counter{0};
    std::vector<ethreads::coro_task<void>> tasks;
    tasks.reserve(100);

    for (int i = 0; i < 100; ++i) {
      tasks.push_back(add_coro(void_coro(counter)));
    }

    for (auto &t : tasks) {
      t.get();
    }

    if (counter.load() == 100) {
      std::cout << "PASS" << std::endl;
    } else {
      std::cout << "FAIL (counter = " << counter.load() << ", expected 100)"
                << std::endl;
      failures++;
    }
  }

  // Test 8: Mixed add_task and add_coro
  std::cout << "Test 8: Mixed add_task/add_coro... ";
  {
    auto future = add_task([]() { return 50; });
    auto coro = add_coro([]() { return 50; });

    int f_result = future.get();
    int c_result = coro.get();

    if (f_result == 50 && c_result == 50) {
      std::cout << "PASS" << std::endl;
    } else {
      std::cout << "FAIL (future=" << f_result << ", coro=" << c_result << ")"
                << std::endl;
      failures++;
    }
  }

  // Summary
  std::cout << std::endl;
  if (failures == 0) {
    std::cout << "All coroutine tests passed!" << std::endl;
    return 0;
  } else {
    std::cout << failures << " coroutine test(s) failed!" << std::endl;
    return 1;
  }
}
