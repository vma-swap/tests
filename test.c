
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
//REGISTER_TEST(test_cow_rmap_walk);
//REGISTER_TEST(test_swap_bin_allocation);
REGISTER_TEST(test_swap_bin_fallback);


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

void test_swap_bin_allocation(void) {

    //bin N -> 2^N x 4k

    // 1. Create the swap files FIRST
    const size_t swapfile_size = 1UL * 1024 * 1024; // 1MB (Lands in Bin 7)
    make_swapfiles(1, swapfile_size, 0); //needed because swap area is cleaned in test_framework.h

    /*for(int i=0;i<35;i++)
    {
        printf("bin %d count = %d\n", i, get_bin_inventory(i));
    }*/

    // 2. Take the snapshot AFTER the bin is populated
    int initial_bin7_count = get_bin_inventory(7);
    ASSERT_NEQ(initial_bin7_count, -1);
    printf("Initial Bin 7 inventory: %d\n", initial_bin7_count); // Should print 1

    // 3. Map 4 pages
    char *addr = map_anon_region(4 * PAGE_SIZE);
    ASSERT_NEQ(addr, NULL);
    
    // Fault pages so anon_vma is established
    addr[0] = 42;
    addr[PAGE_SIZE] = 62;
    addr[2 * PAGE_SIZE] = 82;
    addr[3 * PAGE_SIZE] = 102;

    // 3. Force Swapout
    ASSERT_EQ(swapout_pages(addr, 4), 0); 

    // 4. Verify the VMA was assigned an si from the correct bin
    int assigned_bin = get_swap_bin(addr);
    ASSERT_EQ(assigned_bin, 7); // Log2(1 MB) = Bin 7

    // 5. Verify the Bin Inventory dropped by 1 
    // (Because acquire_si_from_bin took it out!)
    int active_bin7_count = get_bin_inventory(7);
    ASSERT_EQ(active_bin7_count, initial_bin7_count - 1);

    // 6. Free the memory (Trigger recycle_si_to_bin)
    munmap(addr, 4 * PAGE_SIZE);
    usleep(50000); // Give the kernel a fraction of a second to clean up

    // 7. Verify the SI was returned to the bin
    int final_bin7_count = get_bin_inventory(7);
    ASSERT_EQ(final_bin7_count, initial_bin7_count);
}

void test_swap_bin_fallback(void) {
    printf("Starting Bin Fallback Allocation Test...\n");

    // 1. Create our swap files (accounting for the 1-page header tax)
    // Target Bin 8 (256 usable pages) -> We need 257 total pages
    const size_t bin8_size = 257 * PAGE_SIZE; 
    // Target Bin 3 (8 usable pages) -> We need 9 total pages
    const size_t bin3_size = 9 * PAGE_SIZE;

    make_swapfiles(1, bin8_size, 0);
    make_swapfiles(1, bin3_size, 0);

    // 2. Snapshot the initial inventories
    int initial_bin8 = get_bin_inventory(8);
    int initial_bin3 = get_bin_inventory(3);
    
    ASSERT_EQ(initial_bin8, 1);
    ASSERT_EQ(initial_bin3, 1);
    printf("  -> Setup successful. Bin 8: %d, Bin 3: %d\n", initial_bin8, initial_bin3);

    // 3. Occupy the large swap file (Bin 8)
    char *addr_large = map_anon_region(256 * PAGE_SIZE);
    ASSERT_NEQ(addr_large, NULL);
    addr_large[0] = 42; // Fault a page to establish anon_vma
    
    // Swap it out. It asks for 256 pages, finds Bin 8, and takes it.
    ASSERT_EQ(swapout_pages(addr_large, 1), 0);
    ASSERT_EQ(get_swap_bin(addr_large), 8);
    
    // Verify Bin 8 is now empty
    int active_bin8 = get_bin_inventory(8);
    ASSERT_EQ(active_bin8, 0);
    printf("  -> Large VMA successfully occupied Bin 8.\n");

    // 4. Create a SECOND large mapping
    char *addr_fallback = map_anon_region(256 * PAGE_SIZE);
    ASSERT_NEQ(addr_fallback, NULL);
    addr_fallback[0] = 42; // Fault

    // 5. Force the Fallback
    // The kernel will calculate Bin 8. Bin 8 is empty. 
    // Upward scan (Bins 9-34) will fail. 
    // Downward scan will trigger and find our file in Bin 3!
    printf("  -> Forcing fallback allocation...\n");
    ASSERT_EQ(swapout_pages(addr_fallback, 1), 0);

    // 6. Verify the Fallback worked perfectly
    int assigned_bin = get_swap_bin(addr_fallback);
    ASSERT_EQ(assigned_bin, 3); // It must equal 3!
    
    // Verify Bin 3 was actually taken
    int active_bin3 = get_bin_inventory(3);
    ASSERT_EQ(active_bin3, 0);
    printf("  -> Fallback successful! VMA was assigned to Bin %d.\n", assigned_bin);

    // 7. Clean up and verify recycling
    munmap(addr_large, 256 * PAGE_SIZE);
    munmap(addr_fallback, 256 * PAGE_SIZE);
    usleep(100000); // Give the kernel 100ms to recycle the structs

    ASSERT_EQ(get_bin_inventory(8), initial_bin8);
    ASSERT_EQ(get_bin_inventory(3), initial_bin3);
    printf("  -> Memory freed. All structs successfully recycled back to their bins.\n");
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