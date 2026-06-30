#ifndef TEST_HELPER_H
#define TEST_HELPER_H

#include <stddef.h>

#define PAGE_SIZE 4096
#define MAP_NAMED_SWAP 0x200000
#define MIN_PAGE_NAMED_SWAP_MMAP 256
#define VM_MEMORY (130 * (1024 * 1024))
#define PAGEOUT_TEST_SIZE (8 * 1024 * 1024)
#define PAGEOUT_FILE_PATH "/tmp/named_swap_pageout_test.dat"

void dirty_pageout_mapping(unsigned char *addr, size_t len);
unsigned long read_pageout_mapping(unsigned char *addr, size_t len);
unsigned long expected_pageout_sum(size_t len);
void assert_pageout_preserves_data(unsigned char *addr, size_t len);

#endif
