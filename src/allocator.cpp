#include "allocator.hpp"

#include <atomic>

namespace ethreads {

// --- mi_memory_resource ---

void* mi_memory_resource::do_allocate(std::size_t bytes, std::size_t alignment) {
  void* p = mi_malloc_aligned(bytes, alignment);
  if (!p)
    throw std::bad_alloc();
  return p;
}

void mi_memory_resource::do_deallocate(void* p, std::size_t /*bytes*/,
                                       std::size_t alignment) {
  mi_free_aligned(p, alignment);
}

bool mi_memory_resource::do_is_equal(
    const std::pmr::memory_resource& other) const noexcept {
  return this == &other;
}

// --- Singleton resource ---

static mi_memory_resource g_mi_resource;

std::pmr::memory_resource* mi_resource() noexcept { return &g_mi_resource; }

// --- init_allocator ---

static std::atomic<bool> g_allocator_initialized{false};

void init_allocator() {
  bool expected = false;
  if (g_allocator_initialized.compare_exchange_strong(expected, true)) {
    std::pmr::set_default_resource(&g_mi_resource);
  }
}

// --- Thread-local heap ---

mi_heap_t* thread_heap() noexcept { return mi_heap_get_default(); }

// --- mi_arena ---

mi_arena::mi_arena() : heap_(mi_heap_new()) {}

mi_arena::~mi_arena() {
  if (heap_) {
    mi_heap_destroy(heap_);
  }
}

mi_heap_t* mi_arena::heap() noexcept { return heap_; }

void* mi_arena::allocate(std::size_t size) {
  return mi_heap_malloc(heap_, size);
}

void mi_arena::reset() {
  if (heap_) {
    mi_heap_collect(heap_, true);
  }
}

}  // namespace ethreads
