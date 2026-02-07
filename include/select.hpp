#ifndef ETHREADS_SELECT_HPP
#define ETHREADS_SELECT_HPP

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <optional>
#include <variant>
#include <vector>

#include "shared_state/crtp_base.hpp"
#include "shared_state/channel.hpp"

namespace ethreads {

// =============================================================================
// Select Result
// =============================================================================

template <typename T>
struct select_result {
  std::size_t index;  // Which channel fired
  T value;
};

// =============================================================================
// Homogeneous Channel Select Awaiter
// =============================================================================
//
// Receives from the first ready channel in a set of same-typed channels.
// Uses gated waiter nodes to ensure exactly-once resume.

template <typename T, typename BoundednessPolicy = unbounded_policy,
          typename LockPolicy = mutex_lock_policy>
class select_awaiter
    : public awaitable_base<
          select_awaiter<T, BoundednessPolicy, LockPolicy>,
          select_result<T>> {
  std::vector<async_channel<T, BoundednessPolicy, LockPolicy> *> channels_;
  std::vector<waiter_node> nodes_;
  std::atomic<bool> gate_{false};
  std::size_t ready_index_{0};
  std::optional<T> result_;

public:
  explicit select_awaiter(
      std::vector<async_channel<T, BoundednessPolicy, LockPolicy> *> channels)
      : channels_(std::move(channels)),
        nodes_(channels_.size(), waiter_node{nullptr}) {}

  bool ready_impl() noexcept {
    // Try each channel in order
    for (std::size_t i = 0; i < channels_.size(); ++i) {
      result_ = channels_[i]->try_receive();
      if (result_.has_value()) {
        ready_index_ = i;
        return true;
      }
    }
    return false;
  }

  void suspend_impl(std::coroutine_handle<> h) {
    gate_.store(false, std::memory_order_relaxed);

    // Register gated waiter on ALL channels
    for (std::size_t i = 0; i < channels_.size(); ++i) {
      nodes_[i].handle = h;
      nodes_[i].gate = &gate_;
      channels_[i]->add_receive_waiter(&nodes_[i]);
    }

    // Re-check: data may have arrived between ready_impl and registration
    for (std::size_t i = 0; i < channels_.size(); ++i) {
      result_ = channels_[i]->try_receive();
      if (result_.has_value()) {
        ready_index_ = i;
        bool expected = false;
        if (gate_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
          schedule_coro_handle(h);
        }
        return;
      }
    }
  }

  select_result<T> resume_impl() {
    // If we already have a result from suspend_impl re-check, use it
    if (result_.has_value())
      return {ready_index_, std::move(*result_)};

    // One of the channels woke us — find which one has data
    for (std::size_t i = 0; i < channels_.size(); ++i) {
      result_ = channels_[i]->try_receive();
      if (result_.has_value())
        return {i, std::move(*result_)};
    }

    // Should not happen — a channel signaled but we can't receive.
    // This can occur under high contention (another consumer stole the value).
    // Return from first non-empty channel or throw.
    throw std::runtime_error("select: woken but no channel has data");
  }
};

// =============================================================================
// Convenience: select() free function for homogeneous channels
// =============================================================================

template <typename T, typename BoundednessPolicy = unbounded_policy,
          typename LockPolicy = mutex_lock_policy>
auto select(
    std::vector<async_channel<T, BoundednessPolicy, LockPolicy> *> channels) {
  return select_awaiter<T, BoundednessPolicy, LockPolicy>(
      std::move(channels));
}

// =============================================================================
// Variadic Select for Heterogeneous Channels
// =============================================================================

template <typename... Ts>
struct variadic_select_result {
  std::size_t index;
  std::variant<Ts...> value;
};

namespace detail {

// Helper to try_receive on the I-th channel in a tuple
template <std::size_t I, typename Variant, typename... Channels>
bool try_receive_at(std::tuple<Channels *...> &channels, Variant &result,
                    std::size_t &index) {
  auto val = std::get<I>(channels)->try_receive();
  if (val.has_value()) {
    result.template emplace<I>(std::move(*val));
    index = I;
    return true;
  }
  return false;
}

template <typename Variant, typename... Channels, std::size_t... Is>
bool try_receive_any(std::tuple<Channels *...> &channels, Variant &result,
                     std::size_t &index, std::index_sequence<Is...>) {
  return (try_receive_at<Is>(channels, result, index) || ...);
}

} // namespace detail

template <typename... Ts>
class variadic_select_awaiter
    : public awaitable_base<variadic_select_awaiter<Ts...>,
                            variadic_select_result<Ts...>> {
  using channel_tuple = std::tuple<async_channel<Ts> *...>;
  static constexpr std::size_t N = sizeof...(Ts);

  channel_tuple channels_;
  std::array<waiter_node, N> nodes_;
  std::atomic<bool> gate_{false};
  std::size_t ready_index_{0};
  std::variant<Ts...> result_;
  bool has_result_{false};

  template <std::size_t... Is>
  void register_all(std::coroutine_handle<> h, std::index_sequence<Is...>) {
    ((nodes_[Is].handle = h, nodes_[Is].gate = &gate_,
      std::get<Is>(channels_)->add_receive_waiter(&nodes_[Is])),
     ...);
  }

public:
  explicit variadic_select_awaiter(async_channel<Ts> *... channels)
      : channels_(channels...) {
    for (auto &n : nodes_) {
      n.handle = nullptr;
      n.gate = nullptr;
      n.next = nullptr;
    }
  }

  bool ready_impl() noexcept {
    has_result_ = detail::try_receive_any(
        channels_, result_, ready_index_,
        std::make_index_sequence<N>{});
    return has_result_;
  }

  void suspend_impl(std::coroutine_handle<> h) {
    gate_.store(false, std::memory_order_relaxed);

    register_all(h, std::make_index_sequence<N>{});

    // Re-check
    has_result_ = detail::try_receive_any(
        channels_, result_, ready_index_,
        std::make_index_sequence<N>{});
    if (has_result_) {
      bool expected = false;
      if (gate_.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
        schedule_coro_handle(h);
      }
    }
  }

  variadic_select_result<Ts...> resume_impl() {
    if (has_result_)
      return {ready_index_, std::move(result_)};

    // Find which channel has data
    has_result_ = detail::try_receive_any(
        channels_, result_, ready_index_,
        std::make_index_sequence<N>{});
    if (has_result_)
      return {ready_index_, std::move(result_)};

    throw std::runtime_error("select: woken but no channel has data");
  }
};

template <typename... Ts>
auto select(async_channel<Ts> *... channels) {
  return variadic_select_awaiter<Ts...>(channels...);
}

} // namespace ethreads

#endif // ETHREADS_SELECT_HPP
