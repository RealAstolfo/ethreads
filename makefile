CXX = zig c++
AR = zig ar


INC = -I./include -I./vendors/exstd/include

LIB =  -L. -L/usr/lib64 -L/usr/local/lib64
CFLAGS = -march=native -O3 -g -Wall -Wextra -pedantic $(INC)
CXXFLAGS = $(CFLAGS) -std=c++23
LDFLAGS = $(LIB) -O3

# Threading

# TODO: Implement platform detection here
task-scheduler.o:
	${CXX} ${CXXFLAGS} -c src/task_scheduler.cpp -o $@

coro-scheduler.o:
	${CXX} ${CXXFLAGS} -c src/coro_scheduler.cpp -o $@

threading.o: task-scheduler.o coro-scheduler.o
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

# Task Scheduler Static Library
#########################################################################################
threading.a: threading.o
	${AR} rcs $@ $^

all: threading-test coro-test fib-benchmark threading.a

clean:
	-rm -f threading-test coro-test fib-benchmark builds/threading.a *.o
