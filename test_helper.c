#include "test_helper.h"
#include "test_framework.h"

#include <sys/mman.h>

void dirty_pageout_mapping(unsigned char *addr, size_t len) {
    for (size_t off = 0; off < len; off += PAGE_SIZE)
        addr[off] = (unsigned char)((off / PAGE_SIZE) + 1);
}

unsigned long read_pageout_mapping(unsigned char *addr, size_t len) {
    unsigned long sum = 0;

    for (size_t off = 0; off < len; off += PAGE_SIZE)
        sum += addr[off];

    return sum;
}

unsigned long expected_pageout_sum(size_t len) {
    unsigned long sum = 0;

    for (size_t off = 0; off < len; off += PAGE_SIZE)
        sum += (unsigned char)((off / PAGE_SIZE) + 1);

    return sum;
}

void assert_pageout_preserves_data(unsigned char *addr, size_t len) {
    unsigned long expected = expected_pageout_sum(len);

    dirty_pageout_mapping(addr, len);
    ASSERT_EQ(read_pageout_mapping(addr, len), expected);
    ASSERT_EQ(madvise(addr, len, MADV_PAGEOUT), 0);
    ASSERT_EQ(read_pageout_mapping(addr, len), expected);
}
