CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wno-deprecated-declarations -Iinclude
LDFLAGS  = -lpthread
BUILDDIR = build
LIB_OBJ  = $(BUILDDIR)/my_malloc.o

all: $(BUILDDIR)/my_malloc $(BUILDDIR)/test_malloc $(BUILDDIR)/bench_malloc \
     $(BUILDDIR)/test_threaded

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(LIB_OBJ): src/my_malloc.cpp include/my_malloc.h | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/my_malloc: src/main.cpp $(LIB_OBJ) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/test_malloc: tests/test_malloc.cpp $(LIB_OBJ) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/test_threaded: tests/test_threaded.cpp $(LIB_OBJ) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/bench_malloc: tests/bench_malloc.cpp $(LIB_OBJ) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -O2 -o $@ $^ $(LDFLAGS)

test: $(BUILDDIR)/test_malloc $(BUILDDIR)/test_threaded
	./$(BUILDDIR)/test_malloc
	./$(BUILDDIR)/test_threaded

bench: $(BUILDDIR)/bench_malloc
	./$(BUILDDIR)/bench_malloc

run: $(BUILDDIR)/my_malloc
	./$(BUILDDIR)/my_malloc

clean:
	rm -rf $(BUILDDIR)

.PHONY: all test run clean
