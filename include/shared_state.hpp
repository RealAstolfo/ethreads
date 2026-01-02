#ifndef ETHREADS_SHARED_STATE_HPP
#define ETHREADS_SHARED_STATE_HPP

// =============================================================================
// Ethreads Shared State Library
// =============================================================================
//
// A comprehensive template metaprogramming library providing thread-safe
// shared state primitives with both synchronous (blocking) and asynchronous
// (coroutine-awaitable) APIs.
//
// Features:
// - Policy-based design for flexibility (lock policies, boundedness, memory order)
// - CRTP for compile-time polymorphism
// - C++20 Concepts for interface constraints
// - Separate sync_* and async_* variants for each primitive
//
// Primitives:
// - shared_value: Thread-safe value with change notifications
// - semaphore: Counting semaphore for resource limiting
// - event: Boolean flag with manual/auto reset policies
// - barrier: Generation-based thread coordination
// - channel: Message passing with bounded/unbounded variants
//
// =============================================================================

// Foundation headers
#include "shared_state/concepts.hpp"
#include "shared_state/policies.hpp"
#include "shared_state/crtp_base.hpp"

// Primitive headers
#include "shared_state/shared_value.hpp"
#include "shared_state/semaphore.hpp"
#include "shared_state/event.hpp"
#include "shared_state/barrier.hpp"
#include "shared_state/channel.hpp"
#include "shared_state/continuation_handoff.hpp"

#endif // ETHREADS_SHARED_STATE_HPP
