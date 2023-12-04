CC = clang
CXX = clang++
LD = ld.lld
AR = llvm-ar
AS = llvm-as
RANLIB = llvm-ranlib


INC = -I./include -Ivendors/exstd/include

LIB =  -L. -L/usr/lib64 -L/usr/local/lib64
CFLAGS = -march=native -O3 -g -Wall -Wextra -Wno-missing-field-initializers -pedantic $(INC)
CXXFLAGS = $(CFLAGS) -std=c++20
LDFLAGS = $(LIB) -O3

# Threading

# TODO: Implement platform detection here
task-scheduler.o:
	${CXX} ${CXXFLAGS} -c src/task_scheduler.cpp -o $@

threading.o: task-scheduler.o
	${LD} -r $^ -o $@

#########################################################################################

# Task Scheduler Testing
#########################################################################################

threading-tester.o:
	${CXX} ${CXXFLAGS} -c builds/test/threading_tester.cpp -o $@

threading-test: threading.o threading-tester.o
	${CXX} ${CXXFLAGS} $^ -o $@

#########################################################################################

# Task Scheduler Static Library
#########################################################################################
threading.a: threading.o
	${AR} rcs builds/$@ $^

all: threading-test threading.a

clean:
	-rm -f threading-test builds/threading.a *.o