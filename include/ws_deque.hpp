#ifndef ETHREADS_WS_DEQUE_HPP
#define ETHREADS_WS_DEQUE_HPP

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace ethreads {

// Chase-Lev work-stealing deque
// Owner pushes/pops from bottom (LIFO), thieves steal from top (FIFO)
// Lock-free implementation based on "Dynamic Circular Work-Stealing Deque"
template <typename T>
class ws_deque {
public:
  explicit ws_deque(std::size_t initial_capacity = 1024)
      : top_(0), bottom_(0),
        array_(new circular_array(initial_capacity)) {}

  ws_deque(const ws_deque &) = delete;
  ws_deque &operator=(const ws_deque &) = delete;

  ~ws_deque() { delete array_.load(std::memory_order_relaxed); }

  // Owner: push to bottom
  void push(T item) {
    std::size_t b = bottom_.load(std::memory_order_relaxed);
    std::size_t t = top_.load(std::memory_order_acquire);
    circular_array *arr = array_.load(std::memory_order_relaxed);

    if (b - t > arr->capacity() - 1) {
      arr = arr->grow(b, t);
      array_.store(arr, std::memory_order_release);
    }

    arr->put(b, std::move(item));
    std::atomic_thread_fence(std::memory_order_release);
    bottom_.store(b + 1, std::memory_order_relaxed);
  }

  // Owner: pop from bottom (LIFO)
  std::optional<T> pop() {
    std::size_t b = bottom_.load(std::memory_order_relaxed);
    if (b == 0) {
      return std::nullopt;
    }
    b = b - 1;
    circular_array *arr = array_.load(std::memory_order_relaxed);
    bottom_.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::size_t t = top_.load(std::memory_order_relaxed);

    if (t <= b) {
      T item = arr->get(b);
      if (t == b) {
        // Last item - race with thieves
        if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                          std::memory_order_relaxed)) {
          // Lost race to thief
          bottom_.store(b + 1, std::memory_order_relaxed);
          return std::nullopt;
        }
        bottom_.store(b + 1, std::memory_order_relaxed);
      }
      return item;
    }

    // Empty
    bottom_.store(b + 1, std::memory_order_relaxed);
    return std::nullopt;
  }

  // Thief: steal from top (FIFO)
  std::optional<T> steal() {
    std::size_t t = top_.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::size_t b = bottom_.load(std::memory_order_acquire);

    if (t < b) {
      circular_array *arr = array_.load(std::memory_order_consume);
      T item = arr->get(t);
      if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                        std::memory_order_relaxed)) {
        // Lost race to another thief or owner
        return std::nullopt;
      }
      return item;
    }
    return std::nullopt;
  }

  bool empty() const {
    std::size_t b = bottom_.load(std::memory_order_relaxed);
    std::size_t t = top_.load(std::memory_order_relaxed);
    return b <= t;
  }

  std::size_t size() const {
    std::size_t b = bottom_.load(std::memory_order_relaxed);
    std::size_t t = top_.load(std::memory_order_relaxed);
    return b > t ? b - t : 0;
  }

private:
  class circular_array {
  public:
    explicit circular_array(std::size_t n) : mask_(n - 1), buffer_(n) {}

    std::size_t capacity() const { return buffer_.size(); }

    T get(std::size_t i) const { return buffer_[i & mask_]; }

    void put(std::size_t i, T item) { buffer_[i & mask_] = std::move(item); }

    circular_array *grow(std::size_t b, std::size_t t) {
      circular_array *new_arr = new circular_array(capacity() * 2);
      for (std::size_t i = t; i < b; ++i) {
        new_arr->put(i, get(i));
      }
      return new_arr;
    }

  private:
    std::size_t mask_;
    std::vector<T> buffer_;
  };

  std::atomic<std::size_t> top_;
  std::atomic<std::size_t> bottom_;
  std::atomic<circular_array *> array_;
};

} // namespace ethreads

#endif
