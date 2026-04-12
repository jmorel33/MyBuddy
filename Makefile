CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread

TESTS = tests/test_basic tests/test_threads tests/test_huge tests/test_string_view tests/test_usable_size tests/test_multithread_stress

all: $(TESTS)

tests/test_basic: tests/test_basic.c mybuddy.h
	$(CC) $(CFLAGS) $< -o $@

tests/test_threads: tests/test_threads.c mybuddy.h
	$(CC) $(CFLAGS) $< -o $@

tests/test_huge: tests/test_huge.c mybuddy.h
	$(CC) $(CFLAGS) $< -o $@

tests/test_string_view: tests/test_string_view.c mybuddy.h mbd_strings.h
	$(CC) $(CFLAGS) $< -o $@

tests/test_usable_size: tests/test_usable_size.c mybuddy.h
	$(CC) $(CFLAGS) $< -o $@

tests/test_multithread_stress: tests/test_multithread_stress.c mybuddy.h
	$(CC) $(CFLAGS) $< -o $@

test: $(TESTS)
	@echo "Running basic tests..."
	@./tests/test_basic
	@echo "Running thread tests..."
	@./tests/test_threads
	@echo "Running huge allocation tests..."
	@./tests/test_huge
	@echo "Running string view tests..."
	@./tests/test_string_view
	@echo "Running usable size tests..."
	@./tests/test_usable_size
	@echo "Running multi-thread stress tests..."
	@./tests/test_multithread_stress
	@echo "All tests passed successfully!"

clean:
	rm -f $(TESTS)

.PHONY: all test clean
