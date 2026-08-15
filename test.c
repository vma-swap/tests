#define _GNU_SOURCE
#include "test_framework.h"
#include "test_util.h"

#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define PAGE_SIZE 4096
#define MAP_NAMED_SWAP 0x200000
#define MIN_PAGE_NAMED_SWAP_MMAP 256 // should be read from sysctl

REGISTER_TEST(test_mremap_enlarge);
REGISTER_TEST(test_munmap_named_swap_deallocate);
REGISTER_TEST(test_mprotect_permissions);
REGISTER_TEST(test_mremap_shrink_from_right);
REGISTER_TEST(test_partial_munmap_shrink_file_right);
REGISTER_TEST(test_mmap_merge_enlarge_file_right);

loff_t named_swap_file_size(struct file *file);
loff_t named_swap_file_blocks(struct file *file);

void test_mremap_enlarge(void){ 
    size_t initial_size = PAGE_SIZE * 4;
    size_t expanded_size = PAGE_SIZE * 8;


    void *reserved = mmap(NULL, expanded_size, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(reserved != MAP_FAILED);
    if (reserved == MAP_FAILED)
        return;

    /*
     * Free the whole region, leaving a hole of expanded_size.
     */
    int rc = munmap(reserved, expanded_size);
    ASSERT(rc == 0);
    if (rc != 0)
        return;
    // 1. Initial memory mapping with MAP_NAMED_SWAP
   
    unsigned char *addr = mmap(reserved, initial_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    
    printf("test_mremap_enlarge: mmap returned addr=%px\n", addr);

    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED) return;

    // 2. Fault in initial pages and verify anon_vma links
    for (int i = 0; i < initial_size; i += PAGE_SIZE) {
        addr[i] = i / PAGE_SIZE; // Trigger write fault
        
        // Check that the folio's anon_vma matches the VMA's anon_vma
        ASSERT_EQ_ANON_VMA(ANON_VMA_VMA, addr + i,
                           ANON_VMA_FOLIO, addr + i);
    }

    // 3. Verify the initial backing file size
    struct swap_file_info initial_info = get_swap_file_info(addr);
    ASSERT_EQ(initial_info.file_size, initial_size);

    // 4. Expand the mapping using mremap
    unsigned char *new_addr = mremap(addr, initial_size, expanded_size, 0);// MREMAP_MAYMOVE 

    printf("test_mremap_enlarge: mremap returned new_addr=%px\n", new_addr);

    ASSERT_EQ(new_addr, addr); // The address may change due to mremap
    if (new_addr == MAP_FAILED) {
    perror("mremap");
    printf("errno=%d\n", errno);
}
    ASSERT_NEQ(new_addr, MAP_FAILED);
    if (new_addr == MAP_FAILED) return;

    // 5. Fault in the newly expanded pages and verify anon_vma links
    for (int i = 0; i < expanded_size; i += PAGE_SIZE) {
        if (i < initial_size){
            ASSERT_EQ_AT(new_addr + i, i / PAGE_SIZE); // Verify existing pages
        }
        else {
            new_addr[i] = i / PAGE_SIZE; // Trigger write fault on new pages
        
            // The newly allocated folios should share the same anon_vma
            ASSERT_EQ_ANON_VMA(ANON_VMA_VMA, new_addr + i,
                            ANON_VMA_FOLIO, new_addr + i);
        }
    }

   // 6. Verify the backing file was enlarged by the kernel
    struct swap_file_info expanded_info = get_swap_file_info(new_addr);
    ASSERT_EQ(expanded_info.file_size, expanded_size);

    munmap(new_addr, expanded_size); 
}


/* Helper to write to debugfs/proc files */
static void write_sys_file(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        write(fd, val, strlen(val));
        close(fd);
    } else {
        fprintf(stderr, "Failed to open %s\n", path);
    }
}

void test_munmap_named_swap_deallocate(void){
    size_t len = PAGE_SIZE * 3;
    unsigned char *addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED) return;

    /* 1. Fault in all pages to allocate backing blocks */
    addr[0] = 0x11;
    addr[PAGE_SIZE] = 0x22;
    addr[PAGE_SIZE * 2] = 0x33;

    /* 2. Retrieve initial file state */
    struct swap_file_info before = get_swap_file_info(addr);
    ASSERT(before.path[0] != '\0');
    ASSERT_EQ(before.file_size, len); /* Apparent size should be 3 pages */

    /* The filesystem allocates i_blocks in 512-byte units. */
    /* 3 pages * 4096 bytes = 12288 bytes. 12288 / 512 = 24 blocks. */
    unsigned long initial_blocks = before.allocated_blocks;
    ASSERT_ABOVE(initial_blocks, 0); 

    /* 4. Unmap the middle page to trigger named_swap_deallocate */
    ASSERT_EQ(munmap(addr + PAGE_SIZE, PAGE_SIZE), 0);

    /* 5. Validate through /proc/self/maps that exactly two distinct VMAs now exist at these boundaries */
    ASSERT(check_vma_in_maps(addr, addr + PAGE_SIZE) == 1);
    ASSERT(check_vma_in_maps(addr + PAGE_SIZE * 2, addr + PAGE_SIZE * 3) == 1);
    
    /* Ensure the middle VMA is completely gone */
    ASSERT(check_vma_in_maps(addr + PAGE_SIZE, addr + PAGE_SIZE * 2) == 0);
    /* ---------------------- */

    /* 6. Verify the left and right pages are still intact */
    ASSERT_EQ_AT(addr, 0x11);
    ASSERT_EQ_AT(addr + PAGE_SIZE * 2, 0x33);


    /* 7. Verify the apparent backing file size remains unchanged due to KEEP_SIZE */
    struct swap_file_info after = get_swap_file_info(addr);
    ASSERT_EQ(after.file_size, len);

    /* 
     * 8. Verify the physical blocks decreased by exactly the unmapped size.
     * i_blocks are measured in 512-byte sectors.
     * We unmapped exactly 1 PAGE_SIZE (4096 bytes).
     * 4096 / 512 = 8 blocks should have been freed.
     */
    unsigned long after_blocks = after.allocated_blocks;
    unsigned long blocks_freed = PAGE_SIZE / 512;
    
    ASSERT_EQ(initial_blocks, after_blocks + blocks_freed);
    

    /* 9. Verify accessing the unmapped middle page causes a segfault */
    ASSERT_SIGNAL(SIGSEGV) {
        addr[PAGE_SIZE] = 0x44;
    }

    /* Cleanup the remaining left and right VMAs */
    munmap(addr, PAGE_SIZE);
    munmap(addr + PAGE_SIZE * 2, PAGE_SIZE);
}

void test_mprotect_permissions(void) {
    size_t len = PAGE_SIZE * 2;
    unsigned char *addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED) return;

    /* 1. Fault both pages */
    addr[0] = 0xAA;
    addr[PAGE_SIZE] = 0xBB;

    /* 2. Capture initial file state before any VMA splits */
    struct swap_file_info file_before = get_swap_file_info(addr);
    ASSERT(file_before.path[0] != '\0');

    /* 3. Verify initial permissions from kernel (both should be writable) */
    struct page_prot_args prot_page1_before = get_page_prot(addr);
    struct page_prot_args prot_page2_before = get_page_prot(addr + PAGE_SIZE);

    ASSERT_EQ(prot_page1_before.is_writable, 1);
    ASSERT_EQ(prot_page2_before.is_writable, 1);

    /* 4. Apply mprotect to the second page to make it read-only */
    int rc = mprotect(addr + PAGE_SIZE, PAGE_SIZE, PROT_READ);
    ASSERT_EQ(rc, 0);

    /* 5. Verify new permissions from the kernel */
    struct page_prot_args prot_page1_after = get_page_prot(addr);
    struct page_prot_args prot_page2_after = get_page_prot(addr + PAGE_SIZE);

    /* First page should remain writable */
    ASSERT_EQ(prot_page1_after.is_writable, 1);
    /* Second page should NO LONGER be writable */
    ASSERT_EQ(prot_page2_after.is_writable, 0);

    /* 
     * 6. Verify the underlying named_swap files after the split.
     * We query both the left VMA and the right VMA to ensure they remain 
     * tethered to the same backing file despite the metadata split.
     */
    struct swap_file_info file_after_page1 = get_swap_file_info(addr);
    struct swap_file_info file_after_page2 = get_swap_file_info(addr + PAGE_SIZE);

    /* Assert both VMAs point to the exact same file path */
    ASSERT(strcmp(file_after_page1.path, file_after_page2.path) == 0);
    
    /* Assert the file size didn't randomly truncate or expand */
    ASSERT_EQ(file_after_page1.file_size, file_before.file_size);
    ASSERT_EQ(file_after_page2.file_size, file_before.file_size);

    /* Assert logical offsets are strictly maintained */
    ASSERT_EQ(file_after_page2.offset, file_after_page1.offset + PAGE_SIZE);

    /* 
     * 7. Verify memory contents are strictly intact. 
     * Since the second page is PROT_READ, we can safely read it here without 
     * triggering a segfault to prove the file mapping is unbroken.
     */
    ASSERT_EQ_AT(addr, 0xAA);
    ASSERT_EQ_AT(addr + PAGE_SIZE, 0xBB);

    /* Ensure the first page is still physically writable */
    addr[0] = 0xDD;
    ASSERT_EQ_AT(addr, 0xDD);

    sleep(3);

    /* 8. Validate the hardware/page-fault level actually triggers a segfault on write */
    ASSERT_SIGNAL(SIGSEGV) {
        addr[PAGE_SIZE] = 0xCC; /* Attempting to write to read-only page */
    }

    munmap(addr, len);
}

void test_mremap_shrink_from_right(void) {
    size_t page_size = PAGE_SIZE;
    size_t initial_size = page_size * 2;
    size_t shrunk_size = page_size;

    /* 1. Map a 2-page VMA backed by a named swap file */
    unsigned char *addr = mmap(NULL, initial_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED) return;

    /* 2. Fault both pages to ensure the backend file is fully populated and blocks are allocated */
    addr[0] = 0x11;
    addr[page_size] = 0x22;

    /* 3. Verify initial file size is exactly 2 pages */
    struct swap_file_info file_before = get_swap_file_info(addr);
    ASSERT_EQ(file_before.file_size, initial_size);

    /* 4. Shrink the VMA from the right using mremap (no MAYMOVE needed) */
    void *res = mremap(addr, initial_size, shrunk_size, 0);
    
    /* Ensure the syscall succeeded and kept the same base address */
    ASSERT(res != MAP_FAILED);
    ASSERT_EQ(res, addr);

    /* 5. Inspect the underlying file to verify VFS truncate occurred */
    struct swap_file_info file_after = get_swap_file_info(addr);
    
    /* PROOF: If vfs_truncate was called, the overall file size will decrease. 
              If named_swap_deallocate (hole punch) was called, the size would remain 2 pages. */
    ASSERT_EQ(file_after.file_size, shrunk_size);

    /* Cleanup the remaining 1-page VMA */
    munmap(addr, shrunk_size);
}

void test_partial_munmap_shrink_file_right(void) {
    size_t page_size = PAGE_SIZE;
    size_t initial_size = page_size * 2;
    size_t shrunk_size = page_size;

    /* 1. Map a 2-page VMA backed by a named swap file */
    unsigned char *addr = mmap(NULL, initial_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED) return;

    /* 2. Fault both pages to ensure the backend file is fully populated */
    addr[0] = 0x11;
    addr[page_size] = 0x22;

    /* 3. Verify initial file size is exactly 2 pages */
    struct swap_file_info file_before = get_swap_file_info(addr);
    ASSERT_EQ(file_before.file_size, initial_size);

    /* 4. Shrink the VMA from the right by unmapping the second page */
    int rc = munmap(addr + page_size, page_size);
    ASSERT_EQ(rc, 0);

    /* 5. Inspect the underlying file of the remaining VMA to verify VFS truncate occurred */
    struct swap_file_info file_after = get_swap_file_info(addr);
    
    /* PROOF: If vfs_truncate was called by the vma.c cleanup logic, the overall file size will decrease. 
              If named_swap_deallocate (hole punch) was called, the size would remain 2 pages. */
    ASSERT_EQ(file_after.file_size, shrunk_size);

    /* 6. Cleanup the remaining 1-page VMA */
    munmap(addr, shrunk_size);
}

void test_mmap_merge_enlarge_file_right(void) {
    size_t page_size = PAGE_SIZE;

    /* 1. Create a 2-page gap to reserve contiguous virtual memory */
    unsigned char *gap = mmap(NULL, page_size * 2, PROT_NONE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(gap != MAP_FAILED);
    
    /* Unmap to free the virtual addresses back to the kernel */
    ASSERT_EQ(munmap(gap, page_size * 2), 0);

    /* 2. Map the LEFT VMA (Page 1) WITH MAP_NAMED_SWAP */
    unsigned char *left_addr = mmap(gap, page_size, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP | MAP_FIXED, -1, 0);
    ASSERT_EQ(left_addr, gap);
    if (left_addr == MAP_FAILED) return;

    /* Fault the page to trigger file creation and allocate blocks */
    left_addr[0] = 0x11;

    /* Capture the baseline file size of the left VMA (Expect 1 page) */
    struct swap_file_info left_file_before = get_swap_file_info(left_addr);
    ASSERT_EQ(left_file_before.file_size, page_size);
    unsigned long blocks_before = left_file_before.allocated_blocks;

    /* 3. Map the RIGHT VMA (Page 2) WITH MAP_NAMED_SWAP to trigger a positive merge */
    /* Because of Early Neighbor Adoption, this will adopt the left VMA's file and calculate pgoff natively */
    unsigned char *right_addr = mmap(gap + page_size, page_size, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP | MAP_FIXED, -1, 0);
    ASSERT_EQ(right_addr, gap + page_size);
    if (right_addr == MAP_FAILED) return;

    /* Fault the right page */
    right_addr[0] = 0x22;

    /* 4. Inspect the file size by querying the address of our original left VMA */
    struct swap_file_info merged_file = get_swap_file_info(left_addr);
    
    /* 
     * PROOF: Because both VMAs have MAP_NAMED_SWAP and the vm_file/pgoff matched,
     * the kernel merged them. The file size must have increased to cover both pages.
     */
    ASSERT_EQ(merged_file.file_size, page_size * 2); 
    ASSERT_ABOVE(merged_file.allocated_blocks, blocks_before);

    /* 5. Verify the VMA metadata successfully merged the boundaries */
    struct vma_info_args vma_after = get_vma_info(left_addr);
    ASSERT_EQ(vma_after.vma_start, (unsigned long)left_addr);
    ASSERT_EQ(vma_after.vma_end, (unsigned long)right_addr + page_size);

    /* 6. Verify data integrity across the merged VMA */
    ASSERT_EQ_AT(left_addr, 0x11);
    ASSERT_EQ_AT(right_addr, 0x22);

    /* Cleanup: Since they merged, one munmap cleans up the whole 2-page region */
    munmap(left_addr, page_size * 2);
}

static void print_usage(char *argv0) {
    printf("Usage: %s [--trace]\n", argv0);
}

#ifndef COMPILE_TESTS_ONLY
int main(int argc, char *argv[]) {
    int enable_traces = 0;
    struct option long_options[] = {
        {"trace", no_argument, NULL, 't'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "th", long_options, NULL)) != -1) {
        switch (opt) {
        case 't':
            enable_traces = 1;
            break;
        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    return run_all_tests(enable_traces);
}
#endif
