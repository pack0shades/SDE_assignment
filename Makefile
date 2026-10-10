CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -fno-strict-aliasing
HDRS     := src/common.h src/pager.h src/bptree.h

all: build/bench build/test build/test_small

build/bench: src/bench.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<

build/test: src/test.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<

# Same tests with 256-byte pages: 3 records per leaf, 21 children per internal node.
build/test_small: src/test.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DBPT_PAGE_SIZE=256 -o $@ $<

test: build/test build/test_small
	./build/test
	./build/test_small

bench: build/bench
	./run_all.sh

clean:
	rm -rf build *.idx

.PHONY: all test bench clean
