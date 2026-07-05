# ethreads --- C++23 task scheduler + coroutine runtime over io_uring
#
# Pure GNU make.  Dependencies (exstd headers, mimalloc, liburing) are provided
# by the Guix build environment on CPATH / LIBRARY_PATH; nothing is vendored.
#
# Build the static library:        make lib        (-> libethreads.a)
# Install lib + headers:           make install PREFIX=/dest
# Dev tooling (compile_commands):  make clangd

CXX ?= clang++
# Default to the LTO-aware gcc-ar.  `AR ?=` alone would not win against make's
# built-in default (AR=ar), so only honour an explicit command-line/environment
# override; otherwise force gcc-ar.
AR  ?= llvm-ar
ifeq ($(origin AR),default)
AR := llvm-ar
endif

PREFIX  ?= /usr/local
DESTDIR ?=

NAME = ethreads
LIBNAME = lib$(NAME).a

# exstd headers and the third-party deps come from the build environment's
# search paths (CPATH / LIBRARY_PATH); only this repo's own include dir is local.
# mimalloc-static and liburing expose their headers at include/ root (on CPATH).
INC = -I./include

# Mostly-static link policy: static libstdc++/libgcc and the named static
# archives; glibc stays dynamic.  No global -static.
STATIC_FLAGS = -static-libstdc++ -static-libgcc
STATIC_LIBS  = -l:libmimalloc.a -l:liburing.a

CFLAGS   = -march=native -O3 -g -Wall -Wextra -pedantic -Wtype-safety -Wlifetime-safety-all -Wthread-safety $(INC)
CXXFLAGS = $(CFLAGS) -std=c++23
LDFLAGS  = -O3 $(STATIC_FLAGS)

# Debug flags for Valgrind (no optimization for accurate line numbers).
CXX_DEBUG      = clang++
CFLAGS_DEBUG   = -O0 -g3 -Wall -Wextra -pedantic -Wtype-safety -Wlifetime-safety-all -Wthread-safety $(INC)
CXXFLAGS_DEBUG = $(CFLAGS_DEBUG) -std=c++23

# Library objects that make up libethreads.a
LIB_OBJS = task-scheduler.o coro-scheduler.o async-runtime.o \
           timer-service.o io-uring-service.o allocator.o

# Public headers (installed verbatim, preserving the shared_state/ subtree).
HEADERS = include

#########################################################################################
# Library objects
#########################################################################################

task-scheduler.o:
	$(CXX) $(CXXFLAGS) -c src/task_scheduler.cpp -o $@

coro-scheduler.o:
	$(CXX) $(CXXFLAGS) -c src/coro_scheduler.cpp -o $@

async-runtime.o:
	$(CXX) $(CXXFLAGS) -c src/async_runtime.cpp -o $@

timer-service.o:
	$(CXX) $(CXXFLAGS) -c src/timer_service.cpp -o $@

io-uring-service.o:
	$(CXX) $(CXXFLAGS) -c src/io_uring_service.cpp -o $@

allocator.o:
	$(CXX) $(CXXFLAGS) -c src/allocator.cpp -o $@

# Relocatable object combining all library objects (kept for dependents that
# still consume threading.o directly).
threading.o: $(LIB_OBJS)
	ld -r $^ -o $@

#########################################################################################
# Static library  (libethreads.a)
#########################################################################################

$(LIBNAME): $(LIB_OBJS)
	$(AR) rcs $@ $^

# Convenience alias.
lib: $(LIBNAME)

# Back-compat name.
threading.a: $(LIBNAME)
	cp -f $(LIBNAME) $@

#########################################################################################
# Install
#########################################################################################

install: $(LIBNAME)
	mkdir -p $(DESTDIR)$(PREFIX)/lib
	mkdir -p $(DESTDIR)$(PREFIX)/include
	cp -f $(LIBNAME) $(DESTDIR)$(PREFIX)/lib/
	cp -rf $(HEADERS)/. $(DESTDIR)$(PREFIX)/include/

#########################################################################################
# Task Scheduler Testing
#########################################################################################

threading-tester.o:
	$(CXX) $(CXXFLAGS) -c builds/test/threading_tester.cpp -o $@

threading-test: $(LIBNAME) threading-tester.o
	$(CXX) $(CXXFLAGS) threading-tester.o -o $@ $(LDFLAGS) -L. -l$(NAME) $(STATIC_LIBS)

#########################################################################################
# Coroutine Testing
#########################################################################################

coro-tester.o:
	$(CXX) $(CXXFLAGS) -c builds/test/coro_test.cpp -o $@

coro-test: $(LIBNAME) coro-tester.o
	$(CXX) $(CXXFLAGS) coro-tester.o -o $@ $(LDFLAGS) -L. -l$(NAME) $(STATIC_LIBS)

#########################################################################################
# Fibonacci Benchmark
#########################################################################################

fib-benchmark.o:
	$(CXX) $(CXXFLAGS) -c builds/test/fib_benchmark.cpp -o $@

fib-benchmark: $(LIBNAME) fib-benchmark.o
	$(CXX) $(CXXFLAGS) fib-benchmark.o -o $@ $(LDFLAGS) -L. -l$(NAME) $(STATIC_LIBS)

#########################################################################################
# Async Runtime Testing
#########################################################################################

async-runtime-tester.o:
	$(CXX) $(CXXFLAGS) -c builds/test/async_runtime_test.cpp -o $@

async-runtime-test: $(LIBNAME) async-runtime-tester.o
	$(CXX) $(CXXFLAGS) async-runtime-tester.o -o $@ $(LDFLAGS) -L. -l$(NAME) $(STATIC_LIBS)

#########################################################################################
# Fibonacci CORO_MAIN Example
#########################################################################################

fib-coro-main.o:
	$(CXX) $(CXXFLAGS) -c builds/test/fib_coro_main.cpp -o $@

fib-coro-main: $(LIBNAME) fib-coro-main.o
	$(CXX) $(CXXFLAGS) fib-coro-main.o -o $@ $(LDFLAGS) -L. -l$(NAME) $(STATIC_LIBS)

#########################################################################################
# Shared State Library Testing
#########################################################################################

shared-state-tester.o:
	$(CXX) $(CXXFLAGS) -c builds/test/shared_state_test.cpp -o $@

shared-state-test: $(LIBNAME) shared-state-tester.o
	$(CXX) $(CXXFLAGS) shared-state-tester.o -o $@ $(LDFLAGS) -L. -l$(NAME) $(STATIC_LIBS)

#########################################################################################
# Valgrind Debug Builds
#########################################################################################

task-scheduler-debug.o:
	$(CXX_DEBUG) $(CXXFLAGS_DEBUG) -c src/task_scheduler.cpp -o $@

coro-scheduler-debug.o:
	$(CXX_DEBUG) $(CXXFLAGS_DEBUG) -c src/coro_scheduler.cpp -o $@

async-runtime-debug.o:
	$(CXX_DEBUG) $(CXXFLAGS_DEBUG) -c src/async_runtime.cpp -o $@

timer-service-debug.o:
	$(CXX_DEBUG) $(CXXFLAGS_DEBUG) -c src/timer_service.cpp -o $@

io-uring-service-debug.o:
	$(CXX_DEBUG) $(CXXFLAGS_DEBUG) -c src/io_uring_service.cpp -o $@

allocator-debug.o:
	$(CXX_DEBUG) $(CXXFLAGS_DEBUG) -c src/allocator.cpp -o $@

threading-debug.o: task-scheduler-debug.o coro-scheduler-debug.o async-runtime-debug.o timer-service-debug.o io-uring-service-debug.o allocator-debug.o
	ld -r $^ -o $@

threading-tester-debug.o:
	$(CXX_DEBUG) $(CXXFLAGS_DEBUG) -c builds/test/threading_tester.cpp -o $@

coro-tester-debug.o:
	$(CXX_DEBUG) $(CXXFLAGS_DEBUG) -c builds/test/coro_test.cpp -o $@

async-runtime-tester-debug.o:
	$(CXX_DEBUG) $(CXXFLAGS_DEBUG) -c builds/test/async_runtime_test.cpp -o $@

threading-test-valgrind: threading-debug.o threading-tester-debug.o
	$(CXX_DEBUG) $(CXXFLAGS_DEBUG) $^ -o $@ $(STATIC_FLAGS) $(STATIC_LIBS)

coro-test-valgrind: threading-debug.o coro-tester-debug.o
	$(CXX_DEBUG) $(CXXFLAGS_DEBUG) $^ -o $@ $(STATIC_FLAGS) $(STATIC_LIBS)

async-runtime-test-valgrind: threading-debug.o async-runtime-tester-debug.o
	$(CXX_DEBUG) $(CXXFLAGS_DEBUG) $^ -o $@ $(STATIC_FLAGS) $(STATIC_LIBS)

valgrind-all: threading-test-valgrind coro-test-valgrind async-runtime-test-valgrind

helgrind-threading: threading-test-valgrind
	valgrind --tool=helgrind --suppressions=valgrind.supp ./threading-test-valgrind

helgrind-coro: coro-test-valgrind
	valgrind --tool=helgrind --suppressions=valgrind.supp ./coro-test-valgrind

helgrind-async: async-runtime-test-valgrind
	valgrind --tool=helgrind --suppressions=valgrind.supp ./async-runtime-test-valgrind

#########################################################################################
# Profiling Builds (optimized with frame pointers for accurate stack traces)
#########################################################################################

CFLAGS_PROFILE   = -march=native -O3 -g -fno-omit-frame-pointer -Wall -Wextra -pedantic $(INC)
CXXFLAGS_PROFILE = $(CFLAGS_PROFILE) -std=c++23

task-scheduler-profile.o:
	$(CXX) $(CXXFLAGS_PROFILE) -c src/task_scheduler.cpp -o $@

coro-scheduler-profile.o:
	$(CXX) $(CXXFLAGS_PROFILE) -c src/coro_scheduler.cpp -o $@

async-runtime-profile.o:
	$(CXX) $(CXXFLAGS_PROFILE) -c src/async_runtime.cpp -o $@

timer-service-profile.o:
	$(CXX) $(CXXFLAGS_PROFILE) -c src/timer_service.cpp -o $@

io-uring-service-profile.o:
	$(CXX) $(CXXFLAGS_PROFILE) -c src/io_uring_service.cpp -o $@

allocator-profile.o:
	$(CXX) $(CXXFLAGS_PROFILE) -c src/allocator.cpp -o $@

threading-profile.o: task-scheduler-profile.o coro-scheduler-profile.o async-runtime-profile.o timer-service-profile.o io-uring-service-profile.o allocator-profile.o
	ld -r $^ -o $@

#########################################################################################
# Dev tooling
#########################################################################################

# Generate compile_commands.json (replaces clangd.sh).
clangd:
	bear -- make all

all: threading-test coro-test fib-benchmark async-runtime-test fib-coro-main shared-state-test $(LIBNAME)

clean:
	-rm -f threading-test coro-test fib-benchmark async-runtime-test shared-state-test $(LIBNAME) threading.a *.o *-profile.o
	-rm -f threading-test-valgrind coro-test-valgrind async-runtime-test-valgrind fib-coro-main


# Position-independent code: required so each repo's static archive can be
# bundled into the eengine umbrella shared library (libeengine.so).
CFLAGS   += -fPIC
CXXFLAGS += -fPIC
.PHONY: lib install all clean clangd valgrind-all \
        helgrind-threading helgrind-coro helgrind-async
