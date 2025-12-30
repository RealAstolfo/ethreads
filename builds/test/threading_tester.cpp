#include <array>
#include <cassert>
#include <cmath>
#include <future>
#include <iostream>
#include <iterator>
#include <stdexcept>

#include "task_scheduler.hpp"

void fiber_print(const char *s) {
  std::cout << s;
  return;
}

float pi() { return 3.14f; }

void throws() { throw std::runtime_error("test error"); }

void double_it(std::array<int, 10>::iterator start,
               std::array<int, 10>::iterator stop) {
  for (; start != stop; std::advance(start, 1)) {
    *start *= 2;
  }
}

void double_empty(std::array<int, 0>::iterator start,
                  std::array<int, 0>::iterator stop) {
  for (; start != stop; std::advance(start, 1)) {
    *start *= 2;
  }
}

int main() {
  int failures = 0;

  // Test 1: Basic task with return value
  std::cout << "Test 1: Basic task with return value... ";
  {
    auto p1 = add_task(pi);
    float v = p1.get();
    if (std::fabs(v - 3.14f) < 0.001f) {
      std::cout << "PASS" << std::endl;
    } else {
      std::cout << "FAIL (expected 3.14, got " << v << ")" << std::endl;
      failures++;
    }
  }

  // Test 2: Void task
  std::cout << "Test 2: Void task... ";
  {
    const char *msg = "Hello World\n";
    auto p2 = add_task(fiber_print, msg);
    p2.wait();
    std::cout << "PASS" << std::endl;
  }

  // Test 3: Exception handling
  std::cout << "Test 3: Exception propagation... ";
  {
    auto exc_future = add_task(throws);
    try {
      exc_future.get();
      std::cout << "FAIL (exception not thrown)" << std::endl;
      failures++;
    } catch (const std::runtime_error &e) {
      if (std::string(e.what()) == "test error") {
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

  // Test 4: Batch task
  std::cout << "Test 4: Batch task... ";
  {
    std::array<int, 10> arr = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    auto pv1 = add_batch_task(double_it, arr.begin(), arr.end());
    for (auto &p : pv1)
      p.wait();

    bool pass = true;
    for (std::size_t i = 0; i < arr.size(); i++) {
      if (arr[i] != static_cast<int>(i * 2)) {
        pass = false;
        break;
      }
    }
    if (pass) {
      std::cout << "PASS" << std::endl;
    } else {
      std::cout << "FAIL (array values incorrect)" << std::endl;
      failures++;
    }
  }

  // Test 5: Empty batch task (edge case)
  std::cout << "Test 5: Empty batch task... ";
  {
    std::array<int, 0> empty_arr = {};
    auto pv = add_batch_task(double_empty, empty_arr.begin(), empty_arr.end());
    for (auto &p : pv)
      p.wait();
    std::cout << "PASS" << std::endl;
  }

  // Summary
  std::cout << std::endl;
  if (failures == 0) {
    std::cout << "All tests passed!" << std::endl;
    return 0;
  } else {
    std::cout << failures << " test(s) failed!" << std::endl;
    return 1;
  }
}
