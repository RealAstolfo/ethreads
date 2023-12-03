#include <future>
#include <iostream>

#include "task_scheduler.hpp"

void fiber_print(const char *s) {
  std::cout << s;
  return;
}

float pi() { return 3.14; }

int main() {
  const char *msg = "Hello World\n";

  auto p1 = add_task(pi);
  auto p2 = add_task(fiber_print, msg);

  auto v = p1.get();
  std::cout << "Promise 1: " << v << std::endl;
  p2.wait();

  return 0;
}
