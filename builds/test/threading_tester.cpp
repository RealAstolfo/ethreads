#include <future>
#include <iostream>
#include <iterator>

#include "task_scheduler.hpp"

void fiber_print(const char *s) {
  std::cout << s;
  return;
}

float pi() { return 3.14; }

void double_it(std::array<int, 10>::iterator start,
               std::array<int, 10>::iterator stop) {
  for (; start != stop; std::advance(start, 1)) {
    *start *= 2;
  }
}

int main() {
  const char *msg = "Hello World\n";

  std::array<int, 10> arr = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

  auto p1 = add_task(pi);
  auto p2 = add_task(fiber_print, msg);

  auto v = p1.get();
  std::cout << "Promise 1: " << v << std::endl;
  p2.wait();

  std::cout << "Arr:";
  for (std::size_t i = 0; i < arr.size(); i++) {
    std::cout << " " << arr[i];
  }
  std::cout << std::endl;

  auto pv1 = add_batch_task(double_it, arr.begin(), arr.end());
  for (auto &p : pv1)
    p.wait();

  std::cout << "Arr:";
  for (std::size_t i = 0; i < arr.size(); i++) {
    std::cout << " " << arr[i];
  }
  std::cout << std::endl;
  return 0;
}
