#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "shared_state.hpp"
#include "task_scheduler.hpp"

using namespace ethreads;
using namespace std::chrono_literals;

// =============================================================================
// Test Counters
// =============================================================================

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                                             \
  std::cout << "Testing " << name << "... ";                                   \
  try

#define PASS()                                                                 \
  std::cout << "PASSED" << std::endl;                                          \
  ++tests_passed

#define FAIL(msg)                                                              \
  std::cout << "FAILED: " << msg << std::endl;                                 \
  ++tests_failed

// =============================================================================
// Shared Value Tests
// =============================================================================

void test_sync_shared_value() {
  TEST("sync_shared_value basic operations") {
    sync_shared_value<int> value(42);

    assert(value.load() == 42);
    value.store(100);
    assert(value.load() == 100);

    int old = value.exchange(200);
    assert(old == 100);
    assert(value.load() == 200);

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("sync_shared_value compare_exchange") {
    sync_shared_value<int> value(10);

    int expected = 10;
    bool success = value.compare_exchange(expected, 20);
    assert(success);
    assert(value.load() == 20);

    expected = 10;
    success = value.compare_exchange(expected, 30);
    assert(!success);
    assert(expected == 20); // Updated to current value

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("sync_shared_value modify") {
    sync_shared_value<int> value(5);

    value.modify([](int &v) { v *= 2; });
    assert(value.load() == 10);

    auto result = value.modify_and_get([](int &v) {
      v += 5;
      return v * 2;
    });
    assert(result == 30);
    assert(value.load() == 15);

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("sync_shared_value version tracking") {
    sync_shared_value<int> value(0);

    auto v1 = value.version();
    value.store(1);
    auto v2 = value.version();
    assert(v2 > v1);

    value.store(2);
    auto v3 = value.version();
    assert(v3 > v2);

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("sync_shared_value concurrent access") {
    sync_shared_value<int> counter(0);
    constexpr int num_threads = 4;
    constexpr int increments = 1000;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
      threads.emplace_back([&counter]() {
        for (int j = 0; j < increments; ++j) {
          counter.modify([](int &v) { ++v; });
        }
      });
    }

    for (auto &t : threads) {
      t.join();
    }

    assert(counter.load() == num_threads * increments);

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }
}

// =============================================================================
// Semaphore Tests
// =============================================================================

void test_sync_semaphore() {
  TEST("sync_semaphore basic operations") {
    sync_semaphore<> sem(3);

    assert(sem.available() == 3);
    assert(sem.try_acquire());
    assert(sem.available() == 2);

    sem.acquire();
    assert(sem.available() == 1);

    sem.release(2);
    assert(sem.available() == 3);

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("binary_semaphore") {
    binary_semaphore sem(1);

    assert(sem.try_acquire());
    assert(!sem.try_acquire()); // Should fail - no permits

    sem.release();
    assert(sem.try_acquire());

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("sync_semaphore try_acquire_for") {
    sync_semaphore<> sem(0);

    auto start = std::chrono::steady_clock::now();
    bool acquired = sem.try_acquire_for(50ms);
    auto elapsed = std::chrono::steady_clock::now() - start;

    assert(!acquired);
    assert(elapsed >= 45ms); // Allow some tolerance

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("sync_semaphore producer-consumer") {
    sync_semaphore<> sem(0);
    int produced = 0;
    int consumed = 0;

    std::thread producer([&]() {
      for (int i = 0; i < 5; ++i) {
        ++produced;
        sem.release();
        std::this_thread::sleep_for(10ms);
      }
    });

    std::thread consumer([&]() {
      for (int i = 0; i < 5; ++i) {
        sem.acquire();
        ++consumed;
      }
    });

    producer.join();
    consumer.join();

    assert(produced == 5);
    assert(consumed == 5);

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }
}

// =============================================================================
// Event Tests
// =============================================================================

void test_sync_event() {
  TEST("manual_reset_event basic") {
    manual_reset_event evt;

    assert(!evt.is_set());
    evt.set();
    assert(evt.is_set());
    assert(evt.try_wait()); // Still set after wait
    assert(evt.is_set());

    evt.reset();
    assert(!evt.is_set());

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("auto_reset_event basic") {
    auto_reset_event evt;

    evt.set();
    assert(evt.is_set());
    assert(evt.try_wait()); // Consumes the signal
    assert(!evt.is_set()); // Auto-reset occurred

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("manual_reset_event wait_for timeout") {
    manual_reset_event evt;

    auto start = std::chrono::steady_clock::now();
    bool signaled = evt.wait_for(50ms);
    auto elapsed = std::chrono::steady_clock::now() - start;

    assert(!signaled);
    assert(elapsed >= 45ms);

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("manual_reset_event multi-thread wait") {
    manual_reset_event evt;
    std::atomic<int> waiters_done{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i) {
      threads.emplace_back([&]() {
        evt.wait();
        ++waiters_done;
      });
    }

    std::this_thread::sleep_for(20ms);
    assert(waiters_done == 0);

    evt.set(); // Wake all waiters

    for (auto &t : threads) {
      t.join();
    }

    assert(waiters_done == 3);

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }
}

// =============================================================================
// Barrier Tests
// =============================================================================

void test_sync_barrier() {
  TEST("sync_barrier basic") {
    constexpr int num_threads = 3;
    barrier bar(num_threads);
    std::atomic<int> phase1_done{0};
    std::atomic<int> phase2_done{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
      threads.emplace_back([&]() {
        // Phase 1
        ++phase1_done;
        bar.arrive_and_wait();

        // Phase 2
        ++phase2_done;
        bar.arrive_and_wait();
      });
    }

    for (auto &t : threads) {
      t.join();
    }

    assert(phase1_done == num_threads);
    assert(phase2_done == num_threads);

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("sync_barrier with completion function") {
    int completion_count = 0;
    auto completion = [&]() { ++completion_count; };

    sync_barrier<decltype(completion)> bar(2, completion);

    std::thread t1([&]() { bar.arrive_and_wait(); });
    std::thread t2([&]() { bar.arrive_and_wait(); });

    t1.join();
    t2.join();

    assert(completion_count == 1);

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("sync_barrier arrive_and_drop") {
    barrier bar(3);
    std::atomic<int> phase_done{0};

    std::thread dropper([&]() {
      bar.arrive_and_drop(); // Drop out
    });

    std::thread t1([&]() {
      bar.arrive_and_wait();
      ++phase_done;
    });

    std::thread t2([&]() {
      bar.arrive_and_wait();
      ++phase_done;
    });

    dropper.join();
    t1.join();
    t2.join();

    assert(phase_done == 2);

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }
}

// =============================================================================
// Channel Tests
// =============================================================================

void test_sync_channel() {
  TEST("unbounded channel basic") {
    channel<int> ch;

    ch.send(1);
    ch.send(2);
    ch.send(3);

    assert(ch.size() == 3);
    assert(ch.receive() == 1);
    assert(ch.receive() == 2);
    assert(ch.receive() == 3);
    assert(ch.empty());

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("bounded channel basic") {
    bounded_channel<int, 2> ch;

    assert(ch.try_send(1));
    assert(ch.try_send(2));
    assert(!ch.try_send(3)); // Full

    assert(ch.receive() == 1);
    assert(ch.try_send(3)); // Now has space

    assert(ch.receive() == 2);
    assert(ch.receive() == 3);

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("channel try_receive") {
    channel<int> ch;

    auto result = ch.try_receive();
    assert(!result.has_value());

    ch.send(42);
    result = ch.try_receive();
    assert(result.has_value());
    assert(*result == 42);

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("channel receive_for timeout") {
    channel<int> ch;

    auto start = std::chrono::steady_clock::now();
    auto result = ch.receive_for(50ms);
    auto elapsed = std::chrono::steady_clock::now() - start;

    assert(result.status == channel_op_status::timeout);
    assert(elapsed >= 45ms);

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("channel close") {
    channel<int> ch;

    ch.send(1);
    ch.close();

    assert(ch.is_closed());
    assert(ch.receive() == 1); // Can receive existing items

    bool threw = false;
    try {
      ch.receive(); // Should throw - empty and closed
    } catch (const std::runtime_error &) {
      threw = true;
    }
    assert(threw);

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("channel producer-consumer") {
    channel<int> ch;
    int sum = 0;

    std::thread producer([&]() {
      for (int i = 1; i <= 10; ++i) {
        ch.send(i);
      }
      ch.close();
    });

    std::thread consumer([&]() {
      while (true) {
        try {
          sum += ch.receive();
        } catch (const std::runtime_error &) {
          break; // Closed and empty
        }
      }
    });

    producer.join();
    consumer.join();

    assert(sum == 55); // 1 + 2 + ... + 10

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }
}

// =============================================================================
// Policy Tests
// =============================================================================

void test_policies() {
  TEST("spinlock_policy shared_value") {
    fast_shared_value<int> value(0);

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
      threads.emplace_back([&]() {
        for (int j = 0; j < 100; ++j) {
          value.modify([](int &v) { ++v; });
        }
      });
    }

    for (auto &t : threads) {
      t.join();
    }

    assert(value.load() == 400);

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }

  TEST("bounded_policy channel") {
    sync_channel<int, bounded_policy<5>, mutex_lock_policy> ch;

    for (int i = 0; i < 5; ++i) {
      assert(ch.try_send(i));
    }
    assert(!ch.try_send(5)); // Full

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }
}

// =============================================================================
// Concept Tests (compile-time)
// =============================================================================

void test_concepts() {
  TEST("concepts compilation") {
    // These are compile-time checks
    static_assert(Lockable<std::mutex>);
    static_assert(SharedLockable<std::shared_mutex>);
    static_assert(Lockable<spinlock>);

    static_assert(LockPolicy<mutex_lock_policy>);
    static_assert(LockPolicy<shared_mutex_lock_policy>);
    static_assert(LockPolicy<spinlock_policy>);

    static_assert(BoundednessPolicy<bounded_policy<10>>);
    static_assert(BoundednessPolicy<unbounded_policy>);

    static_assert(MemoryOrderPolicy<seq_cst_memory_order>);
    static_assert(MemoryOrderPolicy<acquire_release_memory_order>);

    static_assert(ResetPolicy<manual_reset_policy>);
    static_assert(ResetPolicy<auto_reset_policy>);

    PASS();
  } catch (const std::exception &e) {
    FAIL(e.what());
  }
}

// =============================================================================
// Main
// =============================================================================

int main() {
  std::cout << "=== Shared State Library Tests ===" << std::endl << std::endl;

  std::cout << "--- Shared Value Tests ---" << std::endl;
  test_sync_shared_value();
  std::cout << std::endl;

  std::cout << "--- Semaphore Tests ---" << std::endl;
  test_sync_semaphore();
  std::cout << std::endl;

  std::cout << "--- Event Tests ---" << std::endl;
  test_sync_event();
  std::cout << std::endl;

  std::cout << "--- Barrier Tests ---" << std::endl;
  test_sync_barrier();
  std::cout << std::endl;

  std::cout << "--- Channel Tests ---" << std::endl;
  test_sync_channel();
  std::cout << std::endl;

  std::cout << "--- Policy Tests ---" << std::endl;
  test_policies();
  std::cout << std::endl;

  std::cout << "--- Concept Tests ---" << std::endl;
  test_concepts();
  std::cout << std::endl;

  std::cout << "=== Results ===" << std::endl;
  std::cout << "Passed: " << tests_passed << std::endl;
  std::cout << "Failed: " << tests_failed << std::endl;

  return tests_failed > 0 ? 1 : 0;
}
