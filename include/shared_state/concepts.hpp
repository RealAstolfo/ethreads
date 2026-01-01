#ifndef ETHREADS_SHARED_STATE_CONCEPTS_HPP
#define ETHREADS_SHARED_STATE_CONCEPTS_HPP

#include <concepts>
#include <coroutine>
#include <type_traits>

namespace ethreads {

// =============================================================================
// Core Type Concepts
// =============================================================================

template <typename T>
concept Transferable = std::move_constructible<T> || std::copy_constructible<T>;

template <typename T>
concept Copyable = std::copy_constructible<T> && std::copyable<T>;

template <typename T>
concept MoveOnly = std::move_constructible<T> && !std::copy_constructible<T>;

// =============================================================================
// Coroutine Awaitable Concepts
// =============================================================================

template <typename T>
concept Awaiter = requires(T a, std::coroutine_handle<> h) {
  { a.await_ready() } -> std::convertible_to<bool>;
  { a.await_suspend(h) };
  { a.await_resume() };
};

template <typename T>
concept AwaitableWithMemberOperator = requires(T a) {
  { a.operator co_await() } -> Awaiter;
};

template <typename T>
concept AwaitableWithFreeOperator = requires(T a) {
  { operator co_await(a) } -> Awaiter;
};

template <typename T>
concept Awaitable =
    Awaiter<T> || AwaitableWithMemberOperator<T> || AwaitableWithFreeOperator<T>;

// =============================================================================
// Lockable Concepts (std::mutex-like)
// =============================================================================

template <typename T>
concept BasicLockable = requires(T m) {
  { m.lock() } -> std::same_as<void>;
  { m.unlock() } -> std::same_as<void>;
};

template <typename T>
concept Lockable = BasicLockable<T> && requires(T m) {
  { m.try_lock() } -> std::convertible_to<bool>;
};

template <typename T, typename Duration>
concept TimedLockable = Lockable<T> && requires(T m, Duration d) {
  { m.try_lock_for(d) } -> std::convertible_to<bool>;
};

template <typename T>
concept SharedLockable = Lockable<T> && requires(T m) {
  { m.lock_shared() } -> std::same_as<void>;
  { m.unlock_shared() } -> std::same_as<void>;
  { m.try_lock_shared() } -> std::convertible_to<bool>;
};

// =============================================================================
// Shared State Concepts
// =============================================================================

template <typename S, typename T>
concept SharedStateReader = requires(S s) {
  { s.load() } -> std::convertible_to<T>;
  { s.try_load() } -> std::convertible_to<std::optional<T>>;
};

template <typename S, typename T>
concept SharedStateWriter = requires(S s, T value) {
  { s.store(value) } -> std::same_as<void>;
  { s.try_store(value) } -> std::convertible_to<bool>;
};

template <typename S, typename T>
concept SharedState = SharedStateReader<S, T> && SharedStateWriter<S, T>;

// =============================================================================
// Channel Concepts
// =============================================================================

template <typename C, typename T>
concept ChannelSender = requires(C c, T value) {
  { c.send(value) } -> std::same_as<void>;
  { c.try_send(value) } -> std::convertible_to<bool>;
};

template <typename C, typename T>
concept ChannelReceiver = requires(C c) {
  { c.receive() } -> std::convertible_to<T>;
  { c.try_receive() } -> std::convertible_to<std::optional<T>>;
};

template <typename C, typename T>
concept Channel = ChannelSender<C, T> && ChannelReceiver<C, T> && requires(C c) {
  { c.close() } -> std::same_as<void>;
  { c.is_closed() } -> std::convertible_to<bool>;
};

// =============================================================================
// Synchronization Primitive Concepts
// =============================================================================

template <typename S>
concept Semaphore = requires(S s, std::ptrdiff_t n) {
  { s.acquire() } -> std::same_as<void>;
  { s.try_acquire() } -> std::convertible_to<bool>;
  { s.release(n) } -> std::same_as<void>;
  { s.available() } -> std::convertible_to<std::ptrdiff_t>;
};

template <typename B>
concept Barrier = requires(B b, std::ptrdiff_t n) {
  { b.arrive(n) };
  { b.arrive_and_wait() } -> std::same_as<void>;
  { b.arrive_and_drop() } -> std::same_as<void>;
};

template <typename E>
concept Event = requires(E e) {
  { e.set() } -> std::same_as<void>;
  { e.reset() } -> std::same_as<void>;
  { e.wait() } -> std::same_as<void>;
  { e.try_wait() } -> std::convertible_to<bool>;
  { e.is_set() } -> std::convertible_to<bool>;
};

// =============================================================================
// Policy Concepts
// =============================================================================

template <typename P>
concept LockPolicy = requires {
  typename P::mutex_type;
  typename P::lock_type;
};

template <typename P>
concept SharedLockPolicy = LockPolicy<P> && requires {
  typename P::shared_lock_type;
};

template <typename P>
concept BoundednessPolicy = requires {
  { P::is_bounded } -> std::convertible_to<bool>;
  { P::max_size() } -> std::convertible_to<std::size_t>;
};

template <typename P>
concept MemoryOrderPolicy = requires {
  { P::load_order } -> std::convertible_to<std::memory_order>;
  { P::store_order } -> std::convertible_to<std::memory_order>;
};

template <typename P>
concept ResetPolicy = requires {
  { P::auto_reset } -> std::convertible_to<bool>;
};

// =============================================================================
// Type Traits for Detection
// =============================================================================

template <typename T>
struct is_awaitable : std::bool_constant<Awaitable<T>> {};

template <typename T>
inline constexpr bool is_awaitable_v = is_awaitable<T>::value;

template <typename T>
struct is_lockable : std::bool_constant<Lockable<T>> {};

template <typename T>
inline constexpr bool is_lockable_v = is_lockable<T>::value;

} // namespace ethreads

#endif // ETHREADS_SHARED_STATE_CONCEPTS_HPP
