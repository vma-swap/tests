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

//REGISTER_TEST(test_folio_anon_vma_allocation);
REGISTER_TEST(test_cow_rmap_walk);



void test_folio_anon_vma_allocation(void) {
    char *addr = map_anon_region(PAGE_SIZE);
    
    ASSERT_NEQ(addr, NULL);

    unsigned long returned_page_anon_vma = get_anon_vma_folio(addr);
    unsigned long returned_vma_anon_vma = get_anon_vma_vma(addr);
    
    ASSERT_NEQ(returned_page_anon_vma, 0); 
    ASSERT_NEQ(returned_vma_anon_vma, 0);

    ASSERT_EQ(returned_page_anon_vma, returned_vma_anon_vma);
}

void test_cow_rmap_walk(void){
    char *addr= map_anon_region(4*PAGE_SIZE);
    ASSERT_NEQ(addr, NULL);
    addr[0]=42;
    addr[PAGE_SIZE]=52;
    addr[2*PAGE_SIZE]=68;
    ASSERT_EQ(get_rmap_count(addr), 1);
    ASSERT_EQ(get_rmap_count(addr + PAGE_SIZE), 1);
    ASSERT_EQ(get_rmap_count(addr + 2 * PAGE_SIZE), 1);
    pid_t pid = fork();
    //child
    if (pid == 0) {
        addr[PAGE_SIZE]=99;
        addr[3*PAGE_SIZE]=57;
        ASSERT_EQ(get_rmap_count(addr), 2);
        ASSERT_EQ(get_rmap_count(addr + PAGE_SIZE), 1);
        ASSERT_EQ(get_rmap_count(addr + 3*PAGE_SIZE), 1);  
        //exit(0);
        while(1){sleep(1);} //wait for parent to send kill to child - so child will still map f1 in parent code and we assert 2 there
    }
    //parent
    else{
        //wait(NULL);
        usleep(50000); //50ms to let child do its writes and map the pages
        addr[2*PAGE_SIZE]=120;
        addr[3*PAGE_SIZE]=21;
        ASSERT_EQ(get_rmap_count(addr), 2);
        ASSERT_EQ(get_rmap_count(addr + 2*PAGE_SIZE), 1); 
        ASSERT_EQ(get_rmap_count(addr + 3*PAGE_SIZE), 1);
        kill(pid, SIGKILL); // Ensure child is killed
        wait(NULL);
    }
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