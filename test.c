#include "test_framework.h"
#include "test_util.h" // for make_swaps()
#include <sys/mman.h>
#include <getopt.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>


#define PAGE_SIZE 4096
// REGISTER_TEST(test_folio_file_info);
// REGISTER_TEST(test_folio_offset);
// REGISTER_TEST(test_multiple_swapfiles);
// REGISTER_TEST(test_multiple_swapfiles2);
//REGISTER_TEST(test_vma_si_allcation);
//REGISTER_TEST(test_vma_si_allcation_large);
// REGISTER_TEST(test_stack_vma_offset);
// REGISTER_TEST(test_stack_vma_enlarge);
// REGISTER_TEST(test_available_swapfile);
// REGISTER_TEST(test_vma_values);
//REGISTER_TEST(test_mul_vma_values);
// REGISTER_TEST(test_heap_enlarge);
// REGISTER_PERF_TEST(test_seq_swapout_throughput);
// REGISTER_PERF_TEST(test_rand_swapout_throughput);
// REGISTER_PERF_TEST(test_seq_swapin_throughput);
// REGISTER_PERF_TEST(test_rand_swapin_throughput);
// REGISTER_TEST(test_seq_alloc);
// REGISTER_TEST(test_large_seq_alloc);
// REGISTER_TEST(test_random_alloc);
//REGISTER_TEST(test_swapfile_path);
//REGISTER_TEST(test_basic_no_cow);
REGISTER_TEST(test_folio_anon_vma_allocation);

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

void test_folio_anon_vma_allocation(void) {
    char *addr = map_anon_region(PAGE_SIZE);
    
    ASSERT_NEQ(addr, NULL);

    unsigned long returned_page_anon_vma = get_anon_vma_folio(addr);
    unsigned long returned_vma_anon_vma = get_anon_vma_vma(addr);
    
    // Check if the functions actually retrieved values successfully
    ASSERT_NEQ(returned_page_anon_vma, 0); 
    ASSERT_NEQ(returned_vma_anon_vma, 0);

    // Run your comparison!
    ASSERT_EQ(returned_page_anon_vma, returned_vma_anon_vma);
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