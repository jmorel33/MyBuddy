CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread

TESTS = tests/test_basic tests/test_threads tests/test_huge tests/test_usable_size

all: $(TESTS)

tests/test_basic: tests/test_basic.c mybuddy.h
	$(CC) $(CFLAGS) $< -o $@

tests/test_threads: tests/test_threads.c mybuddy.h
	$(CC) $(CFLAGS) $< -o $@

tests/test_huge: tests/test_huge.c mybuddy.h
	$(CC) $(CFLAGS) $< -o $@

tests/test_usable_size: tests/test_usable_size.c mybuddy.h
	$(CC) $(CFLAGS) $< -o $@

test: $(TESTS)
	@echo "Running basic tests..."
	@./tests/test_basic
	@echo "Running thread tests..."
	@./tests/test_threads
	@echo "Running huge allocation tests..."
	@./tests/test_huge
	@echo "Running usable size tests..."
	@./tests/test_usable_size
	@echo "All tests passed successfully!"

clean:
	rm -f $(TESTS)

.PHONY: all test clean
