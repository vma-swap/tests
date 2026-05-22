#include "test_framework.h"
#include "test_util.h" // for make_swaps()
#include <sys/mman.h>
#include <getopt.h>
#include <unistd.h>
#include <string.h>

#define PAGE_SIZE 4096
// REGISTER_TEST(test_folio_file_info);
// REGISTER_TEST(test_folio_offset);
// REGISTER_TEST(test_multiple_swapfiles);
// REGISTER_TEST(test_multiple_swapfiles2);
// REGISTER_TEST(test_vma_si_allcation);
//REGISTER_TEST(test_vma_si_allcation_large);
// REGISTER_TEST(test_stack_vma_offset);
// REGISTER_TEST(test_stack_vma_enlarge);
// REGISTER_TEST(test_available_swapfile);
// REGISTER_TEST(test_vma_values);
// REGISTER_TEST(test_mul_vma_values);
// REGISTER_TEST(test_heap_enlarge);
// REGISTER_PERF_TEST(test_seq_swapout_throughput);
// REGISTER_PERF_TEST(test_rand_swapout_throughput);
// REGISTER_PERF_TEST(test_seq_swapin_throughput);
// REGISTER_PERF_TEST(test_rand_swapin_throughput);
// REGISTER_TEST(test_seq_alloc);
// REGISTER_TEST(test_large_seq_alloc);
// REGISTER_TEST(test_random_alloc);
REGISTER_TEST(test_swapfile_path);

// Memory-limited tests that trigger swapping
// REGISTER_MEMORY_TEST(test_vma_reclaim_window, "4M");
// REGISTER_MEMORY_TEST(test_vma_reclaim_loop, "2M");
// REGISTER_MEMORY_TEST(test_file, "2M");
// REGISTER_MEMORY_TEST(test_vma_reclaim_window_file, "4M");
/**TODO: 
    -add shared vma tests
    -add heap recude tests
    -add stack reduce tests
    -add vma merge tests. if merge to the right do not NULL the swap_info
**/

void test_heap_enlarge(void) {
    make_swaps(1, 0);
    char* addr = malloc(PAGE_SIZE * 10);
    ASSERT(addr != NULL);
    for (int i = 0; i < 10; i++) {
        addr[i * PAGE_SIZE] = i;
    }
    swapout_page(addr);
    int base_offset = get_swap_offset_from_page(addr);
    printf("Base offset: %d\n", base_offset);
    for (int i = 0; i < 10; i++) {
        swapout_page(addr + (i * PAGE_SIZE));
        ASSERT_EQ(get_swap_offset_from_page(addr + (i * PAGE_SIZE)), base_offset + i);
        ASSERT_EQ(addr[i * PAGE_SIZE], i);
    }
    char* addr2 = malloc(PAGE_SIZE * 10);
    ASSERT(addr2 != NULL);
    for (int i = 0; i < 10; i++) {
        addr2[i * PAGE_SIZE] = i;
    }
    for (int i = 0; i < 10; i++) {
        swapout_page(addr2 + (i * PAGE_SIZE));
        ASSERT_EQ(get_swap_offset_from_page(addr2 + (i * PAGE_SIZE)), base_offset + 10 + i);
        ASSERT_EQ(addr2[i * PAGE_SIZE], i);
    }

}
#define SWAP_FLAG_SYNCHRONOUS_IO_W	0x100000
void test_seq_swapin_throughput(void) {
    make_swaps(1, SWAP_FLAG_SYNCHRONOUS_IO_W);
    unsigned long long region_size = 1<<27; // 512MiB region
    unsigned long long pages = region_size / PAGE_SIZE;
    char *addr = map_large_anon_region(region_size);
    double* latency = malloc(pages * sizeof(double));
    for (unsigned long long i = 0; i < pages; i++) {
        latency[i] = 1;
    }
    ASSERT(addr != NULL);
    start_measurement();
    swapout_pages(addr, pages);
    double elapsed = stop_measurement();
    printf("took %.2f seconds to swap sync swapout %llu pages\n", elapsed, pages);
    sleep(1);
    elapsed = 0;
    for (unsigned long long i = 0; i < pages; i++) {
        start_measurement();
        unsigned long long * tmp_addr = (unsigned long long *)(addr+(i * PAGE_SIZE));
        (*tmp_addr)++;
        double cur_elapsed = stop_measurement();
        elapsed += cur_elapsed;
        latency[i] = cur_elapsed;
    }
    // printf("took %.2f seconds to swap in %llu pages\n", elapsed, pages);
    double throughput = (double)(region_size)/(1<<20) / elapsed; // MB per second
    printf("Sequential swapin throughput: %.2f MB/s\n", throughput);
    // for (unsigned long long i = 0; i < pages; i++) {
    //     printf("Page %llu latency: %.6f seconds\n", i, latency[i]);
    // }
    ASSERT_ABOVE(throughput, 180);
}


void test_stack_vma_offset(void) {
    make_swaps(1, 0);
    char* stack = mmap(NULL, PAGE_SIZE * 10, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN, -1, 0);
    // stack += PAGE_SIZE;
    ASSERT(stack != MAP_FAILED);
    for (int i = 0; i < 10; i++) {
        stack[i * PAGE_SIZE] = i;
    }
    for (int i = 0; i < 10; i++) {
        swapout_page(stack + (i * PAGE_SIZE));
        ASSERT_EQ(get_swap_offset_from_page(stack + (i * PAGE_SIZE)), 9-i);
        stack[i * PAGE_SIZE] = i;
        
    }
}
void test_stack_vma_enlarge(void) {
    char dummy[PAGE_SIZE * 10];
    struct vma_info_args vma_info = get_vma_info(dummy);
    int base_offset = DIV_ROUND_UP(vma_info.vma_end - (unsigned long)dummy, PAGE_SIZE) - 1;
    printf("Base offset: %d\n", base_offset);

    void test_func(){
        char dummy [PAGE_SIZE];
        dummy[0] = 10;
        swapout_page(dummy);
        ASSERT_EQ(get_swap_offset_from_page(dummy), base_offset+1);
        ASSERT_EQ(dummy[0], 10);

    }
    // get the base offset from the vma using get_vma_info

    
    make_swaps(1, 0);
    for (int i = 0; i < 10; i++) {
        dummy[i * PAGE_SIZE] = i;
        swapout_page(dummy + (i * PAGE_SIZE));
        ASSERT_EQ(get_swap_offset_from_page(dummy + (i * PAGE_SIZE)), (base_offset) -i);
        ASSERT_EQ(dummy[i * PAGE_SIZE], i);
    }
    test_func();
}

void test_vma_si_allcation(void) {
    make_swaps(1, 0);
    char *addr = map_anon_region(PAGE_SIZE * 10);
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(vma_has_swap_info(addr + (i * PAGE_SIZE)), 0);
    }
    swapout_page(addr);
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(vma_has_swap_info(addr + (i * PAGE_SIZE)), 1);
    }
}
//
void test_check_swapfile(void) {
    make_swaps(1, 0);
    char *addr = map_anon_region(PAGE_SIZE);
    ASSERT(addr != NULL);
    swapout_page(addr);
    ASSERT(get_swapfile_count() == 0);
}

void test_swapfile_path(void) {
    make_swaps(1, 0);
    
    char *addr = map_anon_region(PAGE_SIZE);
    ASSERT(addr != NULL);
    addr[0] = 42;
    swapout_page(addr);
    
    char returned_path[256];
    int ret = get_swapfile_path(addr, returned_path);
    ASSERT(ret == 0);
    printf("Swap file path from kernel: %s\n", returned_path);
    ASSERT(strstr(returned_path, "/scratch/vma_swaps/swapfile_") != NULL);
}
//

void test_vma_si_allcation_large(void) {
    make_swaps(1, 0);
    unsigned long long region_size = PAGE_SIZE * 262144;
    char *addr = mmap(NULL, region_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < 262144; i++) {
        addr[i * PAGE_SIZE]++;
    }
    ASSERT_EQ(vma_has_swap_info(addr), 0);
    swapout_page(addr);
    swapout_page(addr + (PAGE_SIZE * 262143));
    ASSERT_EQ(vma_has_swap_info(addr), 1);
    addr[PAGE_SIZE*262143]++;
}
void test_available_swapfile(void) {
    ASSERT(get_swapfile_count() == 0);
    make_swaps(1, 0);
    ASSERT(get_swapfile_count() == 1);
    // mmap an anon region
    void *addr = map_anon_region(PAGE_SIZE);
    ASSERT(addr != NULL);
    swapout_page(addr);
    ASSERT(get_swapfile_count() == 0);
}
void test_seq_alloc(void) {
    make_swaps(1, 0);
    char *addr = map_anon_region(PAGE_SIZE*10);
    ASSERT(addr != NULL);
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(is_folio_seq(addr + (i * PAGE_SIZE)), 1);
    }
}
void test_large_seq_alloc(void) {
    make_swaps(1, 0);
    char *addr = map_large_anon_region(PAGE_SIZE*262144);
    ASSERT(addr != NULL);
    for (int i = 0; i < 262144; i++) {
        ASSERT_EQ(is_folio_seq(addr + (i * PAGE_SIZE)), 1);
    }
}
void test_random_alloc(void) {
    make_swaps(1, 0);
    char sz_in_pages = 10;
    char *addr = mmap(NULL, sz_in_pages*PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != NULL);
    for (int i = 0; i < 10; i+=2) {
        addr[i*PAGE_SIZE] = i;
    }
    for (int i = 1; i < 10; i+=2) {
        addr[i*PAGE_SIZE] = i;
    }
    //first is always seq
    ASSERT_EQ(is_folio_seq(addr), 1);
    for (int i = 1; i < 10; i++) {
        ASSERT_EQ(is_folio_seq(addr + (i * PAGE_SIZE)), 0);
    }
}
void test_folio_offset(void) {
    make_swaps(1, 0);
    char *addr = map_anon_region(PAGE_SIZE*10);
    ASSERT(addr != NULL);
    for (int i = 0; i < 10; i++) {
        swapout_page(addr + (i * PAGE_SIZE));
        ASSERT_EQ(get_swap_offset_from_page(addr + (i * PAGE_SIZE)), i);
        ASSERT_EQ(addr[i * PAGE_SIZE], i);
    }
    for (int i = 0; i < 10; i++) {
        swapout_page(addr + (i * PAGE_SIZE));
    }
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(get_swap_offset_from_page(addr + (i * PAGE_SIZE)), i);
        ASSERT_EQ(addr[i * PAGE_SIZE], i);
    }
}
void test_multiple_swapfiles(void) {
    make_swaps(3, 0);
    ASSERT_EQ(get_swapfile_count(), 3);
    char *addr = map_anon_region(PAGE_SIZE * 10);
    ASSERT(addr != NULL);
    for (int i = 0; i < 10; i++) {
        swapout_page(addr + (i * PAGE_SIZE));
        ASSERT_EQ(get_swap_offset_from_page(addr + (i * PAGE_SIZE)), i);
        ASSERT_EQ(addr[i*PAGE_SIZE], i);
    }
    ASSERT_EQ(get_swapfile_count(), 2);
    char *addr2 = map_anon_region(PAGE_SIZE * 10);
    ASSERT(addr2 != NULL);
    for (int i = 0; i < 10; i++) {
        swapout_page(addr2 + (i * PAGE_SIZE));
        ASSERT_EQ(get_swap_offset_from_page(addr2 + (i * PAGE_SIZE)), i);
        ASSERT_EQ(addr2[i*PAGE_SIZE], i);
    }
    ASSERT_EQ(get_swapfile_count(), 1);
    char* addr3 = map_anon_region(PAGE_SIZE * 10);
    ASSERT(addr3 != NULL);
    for (int i = 0; i < 10; i++) {
        swapout_page(addr3 + (i * PAGE_SIZE));
        ASSERT_EQ(get_swap_offset_from_page(addr3 + (i * PAGE_SIZE)), i);
        ASSERT_EQ(addr3[i*PAGE_SIZE], i);
    }
    ASSERT_EQ(get_swapfile_count(), 0);
}

void test_multiple_swapfiles2(void) {
    make_swaps(2, 0);
    ASSERT_EQ(get_swapfile_count(), 2);
    char *addr = map_anon_region(PAGE_SIZE * 10);
    ASSERT(addr != NULL);
    for (int i = 0; i < 10; i++) {
        swapout_page(addr + (i * PAGE_SIZE));
        ASSERT_EQ(get_swap_offset_from_page(addr + (i * PAGE_SIZE)), i);
        ASSERT_EQ(addr[i*PAGE_SIZE], i);
    }
    ASSERT_EQ(get_swapfile_count(), 1);
    char *addr2 = map_anon_region(PAGE_SIZE * 10);
    ASSERT(addr2 != NULL);
    for (int i = 0; i < 10; i++) {
        swapout_page(addr2 + (i * PAGE_SIZE));
        ASSERT_EQ(get_swap_offset_from_page(addr2 + (i * PAGE_SIZE)), i);
        ASSERT_EQ(addr2[i*PAGE_SIZE], i);
    }
    ASSERT_EQ(get_swapfile_count(), 0);
    // go back to the first address and swapout again
    for (int i = 0; i < 10; i++) {
        swapout_page(addr + (i * PAGE_SIZE));
        ASSERT_EQ(get_swap_offset_from_page(addr + (i * PAGE_SIZE)), i);
        ASSERT_EQ(addr[i*PAGE_SIZE], i);
    }

}
void test_vma_values(void){
    make_swaps(1, 0);
    char* addr = map_anon_region(PAGE_SIZE * 10);
    struct vma_info_args vma_info = get_vma_info(addr);
    ASSERT_EQ(vma_info.vma_start, (unsigned long)addr);
    ASSERT_EQ(vma_info.virtual_address, addr);
    ASSERT_EQ(vma_info.swap_info, NULL);
    swapout_page(addr);
    struct vma_info_args vma_info2 = get_vma_info(addr);
    ASSERT_EQ(vma_info2.vma_start, (unsigned long)addr);
    ASSERT_EQ(vma_info2.virtual_address, addr);
    ASSERT_NEQ(vma_info2.swap_info, NULL);
    swapout_page(addr + PAGE_SIZE * 5);
    struct vma_info_args vma_info3 = get_vma_info(addr + PAGE_SIZE * 5);
    ASSERT_EQ(vma_info3.vma_start, (unsigned long)addr);
    ASSERT_EQ(vma_info3.virtual_address, addr + PAGE_SIZE * 5);
    ASSERT_NEQ(vma_info3.swap_info, NULL);
    ASSERT_EQ(vma_info3.swap_info, vma_info2.swap_info);
}

void test_mul_vma_values(void){
    make_swaps(2, 0);
    char* addr = map_anon_region(PAGE_SIZE * 10);
    struct vma_info_args vma_info = get_vma_info(addr);
    ASSERT_EQ(vma_info.vma_start, (unsigned long)addr);
    ASSERT_EQ(vma_info.virtual_address, addr);
    ASSERT_EQ(vma_info.swap_info, NULL);
    swapout_page(addr);
    struct vma_info_args vma_info2 = get_vma_info(addr);
    ASSERT_NEQ(vma_info2.swap_info, NULL);

    char* addr2 = map_anon_region(PAGE_SIZE * 10);
    struct vma_info_args vma_info3 = get_vma_info(addr2);
    ASSERT_EQ(vma_info3.virtual_address, addr2);
    ASSERT_EQ(vma_info3.swap_info, NULL);
    // read the first again
    struct vma_info_args vma_info4 = get_vma_info(addr);
    //check if merged
    if(vma_info3.vma_start == vma_info4.vma_start){
        printf("Merged VMA: %lx - %lx\n", vma_info3.vma_start, vma_info3.vma_end);
        ASSERT_EQ(vma_info4.vma_end, vma_info3.vma_end);
        ASSERT_EQ(vma_info4.swap_info, NULL);
    } else {
        ASSERT_NEQ(vma_info4.vma_end, vma_info3.vma_end);
        ASSERT_NEQ(vma_info4.swap_info, NULL);
    }
    swapout_page(addr2);
    //read both again
    struct vma_info_args vma_info5 = get_vma_info(addr);
    struct vma_info_args vma_info6 = get_vma_info(addr2);
    if(vma_info5.vma_start == vma_info6.vma_start) {
        printf("Merged VMA: %lx - %lx\n", vma_info5.vma_start, vma_info5.vma_end);
        ASSERT_EQ(vma_info5.vma_end, vma_info6.vma_end);
        ASSERT_NEQ(vma_info5.swap_info, NULL);
        ASSERT_NEQ(vma_info6.swap_info, NULL);
        ASSERT_EQ(vma_info5.swap_info, vma_info6.swap_info);
    } else {
        ASSERT_NEQ(vma_info5.vma_end, vma_info6.vma_end);
        ASSERT_NEQ(vma_info5.swap_info, NULL);
        ASSERT_NEQ(vma_info6.swap_info, NULL);
        ASSERT_NEQ(vma_info5.swap_info, vma_info6.swap_info);
    }
}
void test_vma_reclaim_loop(void) {
    int sz_in_pages = 1024;
    char *addr = mmap(NULL, sz_in_pages*PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != NULL);
    printf("Mapped address: %p\n", addr);
    volatile int value = 0;
    struct vma_info_args vma_info = get_vma_info(addr);
    ASSERT_EQ(vma_info.window_start, 0);
    ASSERT_EQ(vma_info.window_end, 0);
    ASSERT_EQ(vma_info.swap_ahead_size, 64);
    for (int i = 0; i < sz_in_pages; i++) {
        addr[i*PAGE_SIZE] = i;
        ASSERT_EQ(is_folio_seq(addr + (i * PAGE_SIZE)), 1);
    }
    for (int loop = 0; loop < 2; loop++) {
        for (int i = 0; i < sz_in_pages; i++) {
            value += addr[i*PAGE_SIZE];
        }
    }
    printf("Final value: %d\n", value);
}

void test_file(void) {
    int sz_in_pages = 1024;
    char *addr = mmap(NULL, sz_in_pages*PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != NULL);
    printf("Mapped address: %p\n", addr);
    volatile int value = 0;
    struct vma_info_args vma_info = get_vma_info(addr);
    ASSERT_EQ(vma_info.window_start, 0);
    ASSERT_EQ(vma_info.window_end, 0);
    ASSERT_EQ(vma_info.swap_ahead_size, 64);
    for (int i = 0; i < sz_in_pages; i++) {
        addr[i*PAGE_SIZE] = i;
        ASSERT_EQ(is_folio_seq(addr + (i * PAGE_SIZE)), 1);
    }
    for (int loop = 0; loop < 2; loop++) {
        for (int i = 0; i < sz_in_pages; i++) {
            value += addr[i*PAGE_SIZE];
        }
    }
    printf("Final value: %d\n", value);
}
// Memory-limited test implementations
void test_vma_reclaim_window(void) {
    char sz_in_pages = 64;
    char *addr = mmap(NULL, sz_in_pages*PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != NULL);
    printf("Mapped address: %p\n", addr);
    char *addr2 = mmap(NULL, 1024*PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr2 != NULL);
    printf("Mapped address: %p\n", addr2);
    struct vma_info_args vma_info = get_vma_info(addr);
    ASSERT_EQ(vma_info.window_start, 0);
    ASSERT_EQ(vma_info.window_end, 0);
    ASSERT_EQ(vma_info.swap_ahead_size, 64);
    for (int i = 0; i < sz_in_pages; i++) {
        addr[i*PAGE_SIZE] = i;
        ASSERT_EQ(is_folio_seq(addr + (i * PAGE_SIZE)), 1);
    }
    //now completly swap out the first region
    for (int i = 0; i < 1024; i++) {
        addr2[i*PAGE_SIZE] = i;
    }
    vma_info = get_vma_info(addr);
    ASSERT_EQ(vma_info.window_start, 0);
    ASSERT_EQ(vma_info.window_end, 64);
    ASSERT_EQ(vma_info.swap_ahead_size, 128);
}

void test_vma_reclaim_window_file(void) {
    char sz_in_pages = 64;
    // drop_caches();
    int fd1 = open("/scratch/swap_tests/256K.file", O_RDWR);
    ASSERT(fd1 > 0);
    if (fd1 < 0)
    {
        perror("open");
        exit(EXIT_FAILURE);
    }
    char *addr = mmap(NULL, sz_in_pages*PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd1, 0);
    ASSERT(addr != NULL);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED) {
        perror("mmap failed");
        exit(EXIT_FAILURE);
    }
    printf("Mapped address: %p\n", addr);
    // drop_caches();
    int fd2 = open("/scratch/swap_tests/4M.file", O_RDWR);
    ASSERT(fd2 > 0);
    if (fd2 < 0)
    {
        perror("open");
        exit(EXIT_FAILURE);
    }
    char *addr2 = mmap(NULL, 1024*PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
    ASSERT(addr2 != NULL);
    ASSERT(addr2 != MAP_FAILED);
    if (addr2 == MAP_FAILED) {
        perror("mmap failed");
        exit(EXIT_FAILURE);
    }
    printf("Mapped address: %p\n", addr2);
    // drop_caches();
    struct vma_info_args vma_info = get_vma_info(addr);
    ASSERT_EQ(vma_info.window_start, 0);
    ASSERT_EQ(vma_info.window_end, 0);
    ASSERT_EQ(vma_info.swap_ahead_size, 64);
    for (int i = 0; i < sz_in_pages; i++) {
        addr[i*PAGE_SIZE] = i;
        ASSERT_EQ(is_folio_seq(addr + (i * PAGE_SIZE)), 1);
        ASSERT(is_folio_file(addr + (i * PAGE_SIZE)));
        ASSERT_EQ(is_folio_anon(addr + (i * PAGE_SIZE)), 0);
        ASSERT(folio_has_mapping(addr + (i * PAGE_SIZE)));
        ASSERT_EQ(get_folio_memcg_id(addr + (i * PAGE_SIZE)), get_current_memcg_id());
        // ASSERT_EQ(get_folio_memcg_id(addr + (i * PAGE_SIZE)), (unsigned short)get_current_memcg_id_fs());
    }
    //now completly swap out the first region
    for (int i = 0; i < 1024; i++) {
        addr2[i*PAGE_SIZE] = i;
        ASSERT(is_folio_file(addr2 + (i * PAGE_SIZE)));
        ASSERT_EQ(is_folio_anon(addr2 + (i * PAGE_SIZE)), 0);
        ASSERT(folio_has_mapping(addr2 + (i * PAGE_SIZE)));
        ASSERT_EQ(get_folio_memcg_id(addr2 + (i * PAGE_SIZE)), get_current_memcg_id());
        // ASSERT_EQ(get_folio_memcg_id(addr2 + (i * PAGE_SIZE)), (unsigned short)get_current_memcg_id_fs());
    }
    vma_info = get_vma_info(addr);
    ASSERT_EQ(vma_info.window_start, 0);
    ASSERT_EQ(vma_info.window_end, 64);
    ASSERT_EQ(vma_info.swap_ahead_size, 128);
}
void test_folio_file_info(void){
    char sz_in_pages = 64;
    drop_caches();
    int fd1 = open("/scratch/swap_tests/256K.file", O_RDWR);
    ASSERT(fd1 > 0);
    if (fd1 < 0)
    {
        perror("open");
        exit(EXIT_FAILURE);
    }
    char *addr = mmap(NULL, sz_in_pages*PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd1, 0);
    ASSERT(addr != NULL);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED) {
        perror("mmap failed");
        exit(EXIT_FAILURE);
    }
    printf("Mapped address: %p\n", addr);
    drop_caches();
    for (int i = 0; i < sz_in_pages; i++) {
        addr[i*PAGE_SIZE] = i;
        ASSERT(is_folio_file(addr + (i * PAGE_SIZE)));
        ASSERT_EQ(is_folio_anon(addr + (i * PAGE_SIZE)), 0);
        ASSERT(folio_has_mapping(addr + (i * PAGE_SIZE)));
        ASSERT_EQ(get_folio_memcg_id(addr + (i * PAGE_SIZE)), get_current_memcg_id());
        ASSERT_EQ(get_folio_memcg_id(addr + (i * PAGE_SIZE)), (unsigned short)get_current_memcg_id_fs());
    }
    drop_caches();


}

void print_usage(char* argv0) {
    printf("Usage: %s [OPTIONS]\n", argv0);
    printf("Options:\n");
    printf("  --trace                          Enable tracing with trace-cmd\n");
    printf("  --perf                          Run performance tests\n");
    printf("  -m, --memory                    Run memory-limited tests\n");
    printf("  -h, --help                      Show this help message\n");
    printf("\nEach memory test specifies its own memory limit.\n");
}

#ifndef COMPILE_TESTS_ONLY
int main(int argc, char *argv[]) {
    // add cli with getopt
    static int minimal_swapfile_num = 1;
    static int enable_traces = 0;
    static int will_run_perf_tests = 0;
    static int will_run_memory_tests = 0;
    
    static struct option long_options[] = {
        {"trace", no_argument, &enable_traces, 't'},
        {"perf", no_argument, 0, 'p'},
        {"memory", no_argument, 0, 'm'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "thpm", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'p':
                will_run_perf_tests = 1;
                break;
            case 't':
                enable_traces = 1;
                break;
            case 'm':
                will_run_memory_tests = 1;
                break;
            case 'h':
                print_usage(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        }
    
    // Single memory test option no longer needed - we use temp binaries instead
        
    set_minimal_swapfile_num(minimal_swapfile_num);
    run_all_tests(enable_traces);
    
    if (will_run_perf_tests) {
        run_perf_tests(enable_traces);
    }
    
    if (will_run_memory_tests) {
        run_memory_tests(enable_traces);
    }
    
    return 0;
}
#endif