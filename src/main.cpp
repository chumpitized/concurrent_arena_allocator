#include <assert.h>
#include <cstring>
#include <iostream>
#include <windows.h>
#include <atomic>

#ifndef DEFAULT_ALIGNMENT
#define DEFAULT_ALIGNMENT sizeof(void *)
#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

typedef uintptr_t uptr;

typedef struct Arena Arena;
struct Arena {
	unsigned char *buffer;
	size_t length;
	std::atomic<size_t> curr_offset;
};

void arena_init(Arena *a, void *buffer, size_t length) {
	a->buffer = (unsigned char *)buffer;
	a->length = length;
	a->curr_offset.store(0);
}

bool is_power_of_two(size_t x) {
	return (x & (x - 1)) == 0;
}

uptr align_forward(uptr ptr, size_t align) {
	assert(is_power_of_two(align));

	uptr modulo = ptr & ((uptr)align - 1);

	if (modulo != 0) {
		ptr += (uptr)align - modulo;
	}

	return ptr;
}

void *arena_concurrent_alloc(Arena *a, size_t size, size_t align) {
	size_t curr_offset = a->curr_offset.load();

	while (true) {
		uptr curr_ptr = (uptr)a->buffer + (uptr)curr_offset;
		uptr offset = align_forward(curr_ptr, align);
		offset -= (uptr)a->buffer;

		if (offset + size > a->length) {
			return NULL;
		}

		if (a->curr_offset.compare_exchange_weak(curr_offset, offset + size)) {
			return &a->buffer[offset];
		}
	}
}

void arena_clear(Arena *a) {
	a->curr_offset.store(0);
}

int main() {
	size_t mb = 1024 * 1024;
	unsigned char buffer[mb]; 
	Arena a = {0};
	arena_init(&a, buffer, mb);

	for (int i = 0; i < 1024; ++i) {
		std::cout << "Allocating int..." << std::endl;
		arena_concurrent_alloc(&a, 4, DEFAULT_ALIGNMENT);
		std::cout << "Current offset is " << a.curr_offset << std::endl;
	}

	std::cout << std::endl;
	std::cout << "Allocating int..." << std::endl;
	arena_concurrent_alloc(&a, 4, DEFAULT_ALIGNMENT);
	std::cout << "Current offset is " << a.curr_offset << std::endl;

	std::cout << std::endl;
	std::cout << "Clearing the arena..." << std::endl;
	arena_clear(&a);
	std::cout << "Current offset is " << a.curr_offset << std::endl;

	return 0;
}