CXX = zig c++
AR = zig ar


INC = -I./include -I./vendors/exstd/include

LIB =  -L. -L/usr/lib64 -L/usr/local/lib64
CFLAGS = -march=native -O3 -g -Wall -Wextra -pedantic $(INC)
CXXFLAGS = $(CFLAGS) -std=c++23
LDFLAGS = $(LIB) -O3

# Debug flags for Valgrind (no optimization for accurate line numbers)
# Use g++ for debug builds since it's more widely available
CXX_DEBUG = g++
CFLAGS_DEBUG = -O0 -g3 -Wall -Wextra -pedantic $(INC)
CXXFLAGS_DEBUG = $(CFLAGS_DEBUG) -std=c++23

# Threading

# TODO: Implement platform detection here
task-scheduler.o:
	${CXX} ${CXXFLAGS} -c src/task_scheduler.cpp -o $@

coro-scheduler.o:
	${CXX} ${CXXFLAGS} -c src/coro_scheduler.cpp -o $@

async-runtime.o:
	${CXX} ${CXXFLAGS} -c src/async_runtime.cpp -o $@

threading.o: task-scheduler.o coro-scheduler.o async-runtime.o
	ld -r $^ -o $@

#########################################################################################

# Task Scheduler Testing
#########################################################################################

threading-tester.o:
	${CXX} ${CXXFLAGS} -c builds/test/threading_tester.cpp -o $@

threading-test: threading.o threading-tester.o
	${CXX} ${CXXFLAGS} $^ -o $@

#########################################################################################

# Coroutine Testing
#########################################################################################

coro-tester.o:
	${CXX} ${CXXFLAGS} -c builds/test/coro_test.cpp -o $@

coro-test: threading.o coro-tester.o
	${CXX} ${CXXFLAGS} $^ -o $@

#########################################################################################

# Fibonacci Benchmark
#########################################################################################

fib-benchmark.o:
	${CXX} ${CXXFLAGS} -c builds/test/fib_benchmark.cpp -o $@

fib-benchmark: threading.o fib-benchmark.o
	${CXX} ${CXXFLAGS} $^ -o $@

#########################################################################################

# Async Runtime Testing
#########################################################################################

async-runtime-tester.o:
	${CXX} ${CXXFLAGS} -c builds/test/async_runtime_test.cpp -o $@

async-runtime-test: threading.o async-runtime-tester.o
	${CXX} ${CXXFLAGS} $^ -o $@

#########################################################################################

# Fibonacci CORO_MAIN Example
#########################################################################################

fib-coro-main.o:
	${CXX} ${CXXFLAGS} -c builds/test/fib_coro_main.cpp -o $@

fib-coro-main: threading.o fib-coro-main.o
	${CXX} ${CXXFLAGS} $^ -o $@

#########################################################################################

# Shared State Library Testing
#########################################################################################

shared-state-tester.o:
	${CXX} ${CXXFLAGS} -c builds/test/shared_state_test.cpp -o $@

shared-state-test: threading.o shared-state-tester.o
	${CXX} ${CXXFLAGS} $^ -o $@

#########################################################################################

# Task Scheduler Static Library
#########################################################################################
threading.a: threading.o
	${AR} rcs $@ $^

#########################################################################################

# Valgrind Debug Builds
#########################################################################################

task-scheduler-debug.o:
	${CXX_DEBUG} ${CXXFLAGS_DEBUG} -c src/task_scheduler.cpp -o $@

coro-scheduler-debug.o:
	${CXX_DEBUG} ${CXXFLAGS_DEBUG} -c src/coro_scheduler.cpp -o $@

async-runtime-debug.o:
	${CXX_DEBUG} ${CXXFLAGS_DEBUG} -c src/async_runtime.cpp -o $@

threading-debug.o: task-scheduler-debug.o coro-scheduler-debug.o async-runtime-debug.o
	ld -r $^ -o $@

threading-tester-debug.o:
	${CXX_DEBUG} ${CXXFLAGS_DEBUG} -c builds/test/threading_tester.cpp -o $@

coro-tester-debug.o:
	${CXX_DEBUG} ${CXXFLAGS_DEBUG} -c builds/test/coro_test.cpp -o $@

async-runtime-tester-debug.o:
	${CXX_DEBUG} ${CXXFLAGS_DEBUG} -c builds/test/async_runtime_test.cpp -o $@

threading-test-valgrind: threading-debug.o threading-tester-debug.o
	${CXX_DEBUG} ${CXXFLAGS_DEBUG} $^ -o $@

coro-test-valgrind: threading-debug.o coro-tester-debug.o
	${CXX_DEBUG} ${CXXFLAGS_DEBUG} $^ -o $@

async-runtime-test-valgrind: threading-debug.o async-runtime-tester-debug.o
	${CXX_DEBUG} ${CXXFLAGS_DEBUG} $^ -o $@

valgrind-all: threading-test-valgrind coro-test-valgrind async-runtime-test-valgrind

helgrind-threading: threading-test-valgrind
	valgrind --tool=helgrind --suppressions=valgrind.supp ./threading-test-valgrind

helgrind-coro: coro-test-valgrind
	valgrind --tool=helgrind --suppressions=valgrind.supp ./coro-test-valgrind

helgrind-async: async-runtime-test-valgrind
	valgrind --tool=helgrind --suppressions=valgrind.supp ./async-runtime-test-valgrind

all: threading-test coro-test fib-benchmark async-runtime-test fib-coro-main shared-state-test threading.a

clean:
	-rm -f threading-test coro-test fib-benchmark async-runtime-test shared-state-test builds/threading.a *.o
	-rm -f threading-test-valgrind coro-test-valgrind async-runtime-test-valgrind fib-coro-main
