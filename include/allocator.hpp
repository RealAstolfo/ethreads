#pragma once

#include <mimalloc.h>
#include <cstddef>
#include <memory_resource>

namespace ethreads {

// PMR memory_resource backed by mimalloc (mi_malloc_aligned / mi_free_aligned)
class mi_memory_resource : public std::pmr::memory_resource {
  void* do_allocate(std::size_t bytes, std::size_t alignment) override;
  void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override;
  bool do_is_equal(const memory_resource& other) const noexcept override;
};

// Initialize mimalloc as global default PMR resource
// Called once from task_scheduler constructor (before any workers start)
void init_allocator();

// Global mimalloc-backed PMR resource (singleton)
std::pmr::memory_resource* mi_resource() noexcept;

// Thread-local heap (mimalloc auto-creates per-thread, this just exposes it)
mi_heap_t* thread_heap() noexcept;

// RAII arena: bulk-free all allocations on scope exit
struct mi_arena {
  mi_arena();
  ~mi_arena();
  mi_arena(const mi_arena&) = delete;
  mi_arena& operator=(const mi_arena&) = delete;
  mi_heap_t* heap() noexcept;
  void* allocate(std::size_t size);
  void reset();  // bulk-free everything, keep arena alive
private:
  mi_heap_t* heap_;
};

}  // namespace ethreads
