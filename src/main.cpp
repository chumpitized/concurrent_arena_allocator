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
	std::atomic<size_t> committed;
	size_t length;
	std::atomic<size_t> curr_offset;
};

void arena_init(Arena *a, size_t length) {
	a->buffer = (unsigned char *)VirtualAlloc(NULL, length, MEM_RESERVE, PAGE_READWRITE);
	a->committed = 0;
	a->length = length;
	a->curr_offset = 0;
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

size_t commit_memory(Arena *a, size_t offset, size_t size) {
	size_t total_alloc = offset + size;

	if (total_alloc <= a->committed) return 0;

	size_t diff = total_alloc - a->committed;

	return (diff + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

size_t concurrent_commit_memory(size_t committed, size_t offset, size_t size) {
	size_t total_alloc = offset + size;

	if (total_alloc <= committed) return 0;

	size_t diff = total_alloc - committed;

	return (diff + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
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

			size_t committed = a->committed.load();

			while (true) {
				size_t to_commit = concurrent_commit_memory(committed, offset, size);
				
				if (!to_commit) break;
				
				if (a->committed.compare_exchange_weak(committed, committed + to_commit)) {
					VirtualAlloc(&a->buffer[committed], to_commit, MEM_COMMIT, PAGE_READWRITE);
					break;
				}
			}

			return &a->buffer[offset];
		}
	}
}

void *arena_alloc(Arena *a, size_t size, size_t align) {
	uptr curr_ptr = (uptr)a->buffer + (uptr)a->curr_offset;
	uptr offset = align_forward(curr_ptr, align);
	offset -= (uptr)a->buffer;

	if (offset + size <= a->length) {
		size_t to_commit = commit_memory(a, offset, size);
		if (to_commit) {
			VirtualAlloc(&a->buffer[a->committed], to_commit, MEM_COMMIT, PAGE_READWRITE);
			a->committed += to_commit;
		}

		void *ptr = &a->buffer[offset];

		a->curr_offset = offset + size;

		return memset(ptr, 0, size);
	}

	return NULL;
}

void arena_clear(Arena *a) {
	a->curr_offset = 0;
}

int main() {
	size_t mb = 1024 * 1024;
	Arena a = {0};
	arena_init(&a, mb);

	//This loop allocates 1024 ints in the arena.
	//You might expect these allocations to consume only 4096 bytes (i.e., 1024 * sizeof(int)),
	//but because we're aligning allocations on 8-byte boundaries (the size of a void pointer),
	//each int allocation effectively consumes 8 bytes. The same thing would be true for chars, 
	//which are only 1 byte wide.

	//Not that the final allocation puts the curr_offset at 8188, just shy of 8192, which is
	//exactly PAGE_SIZE * 2. I did that on purpose.
	for (int i = 0; i < 1024; ++i) {
		std::cout << "Allocating int..." << std::endl;
		arena_alloc(&a, 4, DEFAULT_ALIGNMENT);
		std::cout << "Current offset is " << a.curr_offset << std::endl;
	}

	//Because we're allocating 4096 bytes worth of ints and 4092 bytes worth of alignment padding (8188 bytes total),
	//we will need to commit two pages of virtual memory. We confirm that here.
	std::cout << std::endl;
	std::cout << "Committed should be 8192: " << a.committed << std::endl;

	//The very next int allocation should cause our arena to commit a third page of virtual memory.
	std::cout << std::endl;
	std::cout << "Allocating int..." << std::endl;
	arena_alloc(&a, 4, DEFAULT_ALIGNMENT);
	std::cout << "Current offset is " << a.curr_offset << std::endl;

	//And finally we confirm the commitment of the third page of virtual memory. Cool!
	std::cout << std::endl;
	std::cout << "Committed should be 12288: " << a.committed << std::endl;

	//Now let's clear the arena. This won't uncommit any of our committed memory, but it will
	//return curr_offset to 0 and allow us to allocate up to 12288 bytes without any new
	//virtual allocations.
	std::cout << std::endl;
	std::cout << "Clearing the arena..." << std::endl;
	arena_clear(&a);
	std::cout << "Current offset is " << a.curr_offset << std::endl;

	std::cout << std::endl;
	std::cout << "Committed should be 12288: " << a.committed << std::endl;

	return 0;
}