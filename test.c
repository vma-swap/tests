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

//REGISTER_TEST(test_single_anon_vma);
//REGISTER_TEST(test_fork_anon_vma);
//REGISTER_TEST(test_count_rmap_vmas);
//REGISTER_TEST(test_swap_file_creation);
//REGISTER_TEST(test_swap_file_delete_unmap);
//REGISTER_TEST(test_swap_file_delete_exit);
//REGISTER_TEST(test_zero_file);
//REGISTER_TEST(test_read_first_fault);
//REGISTER_TEST(test_write_fault);
//REGISTER_TEST(test_mremap_enlarge);
//REGISTER_TEST(test_mremap_failure_shrink);
REGISTER_TEST(test_munmap_named_swap_deallocate);
//REGISTER_TEST(test_mprotect_permissions);
//REGISTER_TEST(test_single_vma_growsdown);
//REGISTER_TEST(test_mremap_left_enlarge_only_fails_deliberately);
//REGISTER_TEST(test_mremap_left_enlarge_only_evict_and_resume);
//REGISTER_TEST(test_simple_mremap_to_the_left_case);

loff_t named_swap_file_size(struct file *file);
loff_t named_swap_file_blocks(struct file *file);

void test_write_fault(void) {
    unsigned char *addr = mmap(NULL, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    for (int i = 0; i < PAGE_SIZE*MIN_PAGE_NAMED_SWAP_MMAP; i++) {
        addr[i] = i%256;
        ASSERT_EQ_AT(addr + i, i%256);
        if (i % PAGE_SIZE == 0) {
            ASSERT_EQ_ANON_VMA(ANON_VMA_VMA, addr + i,
                               ANON_VMA_FOLIO, addr + i);
        }
    }
    munmap(addr, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP);
}

void test_zero_file(void) {
    unsigned char *addr = mmap(NULL, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    for (int i = 0; i < PAGE_SIZE*MIN_PAGE_NAMED_SWAP_MMAP; i++) {
        ASSERT_EQ_AT(addr + i, 0);
        if (i % PAGE_SIZE == 0) {
            ASSERT_EQ_ANON_VMA(ANON_VMA_VMA, addr + i,
                               ANON_VMA_FOLIO, addr + i);
        }
    }
    struct swap_file_info swap_file_info = get_swap_file_info(addr);
    ASSERT_NEQ(swap_file_info.path, NULL);
    munmap(addr, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP);
}

void test_read_first_fault(void) {
    unsigned char *addr = mmap(NULL, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    for (int i = 0; i < PAGE_SIZE*MIN_PAGE_NAMED_SWAP_MMAP; i++) {
        ASSERT_EQ_AT(addr + i, 0);
        if (i % PAGE_SIZE == 0) {
            ASSERT_EQ_ANON_VMA(ANON_VMA_VMA, addr + i,
                               ANON_VMA_FOLIO, addr + i);
        }
        addr[i] = i%256;
        ASSERT_EQ_AT(addr + i, i%256);
        if (i % PAGE_SIZE == 0) {
            ASSERT_EQ_ANON_VMA(ANON_VMA_VMA, addr + i,
                               ANON_VMA_FOLIO, addr + i);
        }
    }
    struct swap_file_info swap_file_info = get_swap_file_info(addr);
    ASSERT_NEQ(swap_file_info.path, NULL);
    munmap(addr, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP);
}

void test_swap_file_delete_exit(void) {
    int pipefd[2] = {-1, -1};
    pid_t pid;
    unsigned long index = 0;
    ssize_t bytes_read;
    int status = 0;
    char path[PATH_MAX];

    ASSERT_EQ(pipe(pipefd), 0);
    if (pipefd[0] < 0 || pipefd[1] < 0)
        return;

    pid = fork();
    ASSERT_NEQ(pid, -1);
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    if (pid == 0) {
        close(pipefd[0]);
        char *addr = mmap(NULL, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP,
                          -1, 0);
        ASSERT(addr != MAP_FAILED);
        if (addr == MAP_FAILED)
            _exit(EXIT_FAILURE);

        addr[0] = 1;
        index = assert_named_swap_file_for_addr(addr);
        if (!current_test_failed)
            write(pipefd[1], &index, sizeof(index));
        close(pipefd[1]);
        _exit(current_test_failed ? EXIT_FAILURE : EXIT_SUCCESS);
    }

    close(pipefd[1]);
    bytes_read = read(pipefd[0], &index, sizeof(index));
    close(pipefd[0]);
    ASSERT_EQ(bytes_read, (ssize_t)sizeof(index));

    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT(WIFEXITED(status));
    if (WIFEXITED(status))
        ASSERT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);

    if (bytes_read == (ssize_t)sizeof(index)) {
        named_swap_path_for_index(path, sizeof(path), index);
        ASSERT_EQ(access(path, F_OK), -1);
    }
}

void test_swap_file_delete_unmap(void) {
    char *addr = mmap(NULL, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    addr[0] = 1;
    unsigned long index = assert_named_swap_file_for_addr(addr);
    char path[PATH_MAX];
    named_swap_path_for_index(path, sizeof(path), index);
    munmap(addr, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP);
    ASSERT_EQ(access(path, F_OK), -1);
}

void test_swap_file_creation(void) {
    char *addr = mmap(NULL, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    addr[0] = 1;
    assert_named_swap_file_for_addr(addr);
    munmap(addr, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP);
}

void test_single_anon_vma(void) {
    char *addr = mmap(NULL, PAGE_SIZE * 10, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    for (int i = 0; i < 10; i++) {
        struct anon_vma_info_args anon_vma_info = get_anon_vma_info_from_vma(addr + (i * PAGE_SIZE));
        ASSERT_EQ(anon_vma_info.anon_vma, NULL);
    }
    addr[0] = 1;
    struct anon_vma_info_args first_anon_vma_info = get_anon_vma_info_from_vma(addr);
    for (int i = 1; i < 10; i++) {
        addr[i * PAGE_SIZE] = i;
        struct anon_vma_info_args anon_vma_info = get_anon_vma_info(addr + (i * PAGE_SIZE));
        ASSERT_EQ_ANON_VMA(ANON_VMA_FOLIO, addr + (i * PAGE_SIZE),
                           ANON_VMA_FOLIO, addr);
        ASSERT_EQ(anon_vma_info.root, first_anon_vma_info.anon_vma);
        ASSERT_EQ(anon_vma_info.parent, first_anon_vma_info.anon_vma);
        ASSERT_EQ(anon_vma_info.num_children, 1);
        ASSERT_EQ(anon_vma_info.num_active_vmas, 1);
    }
}

void test_fork_anon_vma(void) {
    char *addr = mmap(NULL, PAGE_SIZE * 4, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    addr[0] = 1;     // will be shared
    addr[PAGE_SIZE] = 2;  // will be cowed by child
    addr[PAGE_SIZE * 2] = 3;  // will be cowed by parent
    //addr[PAGE_SIZE * 3]     // will be allocated by each
    struct anon_vma_info_args parent_original = get_anon_vma_info(addr);
    if (fork()){
        sleep(1); // to make sure son already finished his code
        //checks on the shared page
        struct anon_vma_info_args page_1 = get_anon_vma_info(addr);
        ASSERT_EQ(page_1.anon_vma, parent_original.anon_vma);
        ASSERT(page_1.anon_vma != NULL);
        ASSERT_EQ(page_1.root, parent_original.anon_vma);
        ASSERT_EQ(page_1.parent, parent_original.anon_vma);
        ASSERT_EQ(page_1.num_children, 3);
        ASSERT_EQ(page_1.num_active_vmas, 0);
        //checks on page that the child cowed, identical to page 1
        struct anon_vma_info_args page_2 = get_anon_vma_info(addr + PAGE_SIZE);
        ASSERT_EQ_ANON_VMA(ANON_VMA_FOLIO, addr + PAGE_SIZE,
                           ANON_VMA_FOLIO, addr);
        ASSERT_EQ(page_2.anon_vma, parent_original.anon_vma);
        ASSERT(page_2.anon_vma != NULL);
        ASSERT_EQ(page_2.root, parent_original.anon_vma);
        ASSERT_EQ(page_2.parent, parent_original.anon_vma);
        ASSERT_EQ(page_2.num_children, 3);
        ASSERT_EQ(page_2.num_active_vmas, 0);
        //checks on page that the parent cowed
        addr[PAGE_SIZE * 2]++;
        struct anon_vma_info_args page_3 = get_anon_vma_info(addr + PAGE_SIZE * 2);
        ASSERT_NEQ_ANON_VMA(ANON_VMA_FOLIO, addr + PAGE_SIZE * 2,
                            ANON_VMA_FOLIO, addr);
        ASSERT(page_3.anon_vma != parent_original.anon_vma);
        ASSERT(page_3.anon_vma != NULL);
        ASSERT_EQ(page_3.root, parent_original.anon_vma);
        ASSERT_EQ(page_3.parent, parent_original.anon_vma);
        ASSERT_EQ(page_3.num_children, 0);
        ASSERT_EQ(page_3.num_active_vmas, 1);
        //checks on page that the parent allocated
        addr[PAGE_SIZE * 3] = 4;
        struct anon_vma_info_args page_4 = get_anon_vma_info(addr + PAGE_SIZE * 3);
        ASSERT_EQ_ANON_VMA(ANON_VMA_FOLIO, addr + PAGE_SIZE * 3,
                           ANON_VMA_FOLIO, addr + PAGE_SIZE * 2);
        ASSERT(page_4.anon_vma != NULL);
        ASSERT_EQ(page_4.root, parent_original.anon_vma);
        ASSERT_EQ(page_4.parent, parent_original.anon_vma);
        ASSERT_EQ(page_4.num_children, 0);
        ASSERT_EQ(page_4.num_active_vmas, 1);
    }
    else {
        //checks on the shared page
        struct anon_vma_info_args page_1 = get_anon_vma_info(addr);
        ASSERT_EQ(page_1.anon_vma, parent_original.anon_vma);
        ASSERT(page_1.anon_vma != NULL);
        ASSERT_EQ(page_1.root, parent_original.anon_vma);
        ASSERT_EQ(page_1.parent, parent_original.anon_vma);
        ASSERT_EQ(page_1.num_children, 3);
        ASSERT_EQ(page_1.num_active_vmas, 0);
        //checks on page that the child cowed, private to the child
        addr[PAGE_SIZE]++;
        struct anon_vma_info_args page_2 = get_anon_vma_info(addr + PAGE_SIZE);
        ASSERT_NEQ_ANON_VMA(ANON_VMA_FOLIO, addr + PAGE_SIZE,
                            ANON_VMA_FOLIO, addr);
        ASSERT(page_2.anon_vma != parent_original.anon_vma);
        ASSERT(page_2.anon_vma != NULL);
        ASSERT_EQ(page_2.root, parent_original.anon_vma);
        ASSERT_EQ(page_2.parent, parent_original.anon_vma);
        ASSERT_EQ(page_2.num_children, 0);
        ASSERT_EQ(page_2.num_active_vmas, 1);
        //checks on page that the parent cowed
        struct anon_vma_info_args page_3 = get_anon_vma_info(addr + PAGE_SIZE*2);
        ASSERT_EQ_ANON_VMA(ANON_VMA_FOLIO, addr + PAGE_SIZE * 2,
                           ANON_VMA_FOLIO, addr);
        ASSERT_EQ(page_3.anon_vma, parent_original.anon_vma);
        ASSERT(page_3.anon_vma != NULL);
        ASSERT_EQ(page_3.root, parent_original.anon_vma);
        ASSERT_EQ(page_3.parent, parent_original.anon_vma);
        ASSERT_EQ(page_3.num_children, 3);
        ASSERT_EQ(page_3.num_active_vmas, 0);
        //checks on page that the child allocated
        addr[PAGE_SIZE * 3] = 4;
        struct anon_vma_info_args page_4 = get_anon_vma_info(addr + PAGE_SIZE * 3);
        ASSERT_EQ_ANON_VMA(ANON_VMA_FOLIO, addr + PAGE_SIZE * 3,
                           ANON_VMA_FOLIO, addr + PAGE_SIZE);
        ASSERT(page_4.anon_vma != NULL);
        ASSERT_EQ(page_4.root, parent_original.anon_vma);
        ASSERT_EQ(page_4.parent, parent_original.anon_vma);
        ASSERT_EQ(page_4.num_children, 0);
        ASSERT_EQ(page_4.num_active_vmas, 1);
        sleep(1.5);
        exit(0);
    }

}

void test_count_rmap_vmas(void) {
    char *addr = mmap(NULL, PAGE_SIZE * 4, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    addr[0] = 1;     // will be shared
    addr[PAGE_SIZE] = 2;  // will be cowed by child
    addr[PAGE_SIZE * 2] = 3;  // will be cowed by parent
    //addr[PAGE_SIZE * 3]     // will be allocated by each
    ASSERT_EQ(count_rmap_vmas(addr), 1);
    if (fork()){
        sleep(1); // to make sure son already finished his code
        //checks on the shared page
        ASSERT_EQ(count_rmap_vmas(addr), 2);
        //checks on page that the child cowed, identical to page 1
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE), 2);
        //checks on page that the parent cowed
        addr[PAGE_SIZE * 2]++;
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE * 2), 1);
        //checks on page that the parent allocated
        addr[PAGE_SIZE * 3] = 4;
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE * 3), 1);
    }
    else {
        //checks on the shared page
        ASSERT_EQ(count_rmap_vmas(addr), 2);
        //checks on page that the child cowed, private to the child
        addr[PAGE_SIZE]++;
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE), 1);
        //checks on page that the parent cowed
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE * 2), 2);
        //checks on page that the child allocated
        addr[PAGE_SIZE * 3] = 4;
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE * 3), 1);
        sleep(1.5);
        exit(0);
    }

}
void test_mulcount_rmap_vmas_multi_fork(void) {
    char *addr = mmap(NULL, PAGE_SIZE * 4, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    addr[0] = 1;     // will be shared
    addr[PAGE_SIZE] = 2;  // will be cowed by child
    addr[PAGE_SIZE * 2] = 3;  // will be cowed by parent
    //addr[PAGE_SIZE * 3]     // will be allocated by each
    ASSERT_EQ(count_rmap_vmas(addr), 1);
    if (fork()){
        sleep(1); // to make sure son already finished his code
        //checks on the shared page
        ASSERT_EQ(count_rmap_vmas(addr), 3);
        //checks on page that the child cowed, identical to page 1
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE), 3);
        //checks on page that the parent cowed
        addr[PAGE_SIZE * 2]++;
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE * 2), 1);
        //checks on page that the parent allocated
        addr[PAGE_SIZE * 3] = 4;
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE * 3), 1);
        if(!fork()) {
            sleep(1.5);
            exit(0);
        }
        //checks on the shared page
        ASSERT_EQ(count_rmap_vmas(addr), 3);
        //checks on page that the child cowed, identical to page 1
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE), 3);
        //checks on page that the parent cowed
        addr[PAGE_SIZE * 2]++;
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE * 2), 1);
        //checks on page that the parent allocated
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE * 3), 2);
    }
    else {
        //checks on the shared page
        ASSERT_EQ(count_rmap_vmas(addr), 2);
        //checks on page that the child cowed, private to the child
        addr[PAGE_SIZE]++;
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE), 1);
        //checks on page that the parent cowed
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE * 2), 2);
        //checks on page that the child allocated
        addr[PAGE_SIZE * 3] = 4;
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE * 3), 1);
        if(!fork()) {
            sleep(1.5);
            exit(0);
        }
        sleep(1.5);
        //checks on the shared page
        ASSERT_EQ(count_rmap_vmas(addr), 4);
        //checks on page that the child cowed, private to the child
        addr[PAGE_SIZE]++;
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE), 1);
        //checks on page that the parent cowed
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE * 2), 4);
        //checks on page that the child allocated
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE * 3), 2);
        exit(0);
    }

}

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

void test_mremap_failure_shrink(void) {
    size_t initial_size = PAGE_SIZE * 4;
    size_t expanded_size = PAGE_SIZE * 8;

    /* 1. Setup the reserved region and initial mapping */
    void *reserved = mmap(NULL, expanded_size, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(reserved != MAP_FAILED);
    int rc = munmap(reserved, expanded_size);
    ASSERT(rc == 0);

    unsigned char *addr = mmap(reserved, initial_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    
    printf("test_mremap_failure_shrink: mmap returned addr=%px\n", addr);

    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED) return;

    /* 2. Fault in initial pages and verify anon_vma links */
    for (int i = 0; i < initial_size; i += PAGE_SIZE) {
        addr[i] = i / PAGE_SIZE; // Trigger write fault
        
        // Check that the folio's anon_vma matches the VMA's anon_vma
        ASSERT_EQ_ANON_VMA(ANON_VMA_VMA, addr + i,
                           ANON_VMA_FOLIO, addr + i);
    }

    /* 3. Verify the initial backing file size */
    struct swap_file_info initial_info = get_swap_file_info(addr);
    ASSERT_EQ(initial_info.file_size, initial_size);

    /* 4. ENABLE FUNCTION ERROR INJECTION */
    // Tell the kernel to intercept vma_merge_extend
    write_sys_file("/sys/kernel/debug/fail_function/inject", "vma_merge_extend");
    // Force the return value to be 0 (NULL)
    write_sys_file("/sys/kernel/debug/fail_function/vma_merge_extend/retval", "0");

    // Make the injection deterministic for this test run
    write_sys_file("/sys/kernel/debug/fail_function/probability", "100\n");
    write_sys_file("/sys/kernel/debug/fail_function/times", "1\n");
    write_sys_file("/sys/kernel/debug/fail_function/interval", "0\n");
    write_sys_file("/sys/kernel/debug/fail_function/space", "0\n");
    write_sys_file("/sys/kernel/debug/fail_function/verbose", "1\n");

    /* 5. Attempt the expansion */
    // named_swap_enlarge will succeed, but vma_merge_extend will instantly return NULL
    unsigned char *new_addr = mremap(addr, initial_size, expanded_size, 0);

    /* 6. DISABLE FUNCTION ERROR INJECTION */
    // Clear the injection so it doesn't affect subsequent tests or system stability
    write_sys_file("/sys/kernel/debug/fail_function/inject", "");

    /* 7. Verify mremap gracefully failed */
    ASSERT_EQ(new_addr, MAP_FAILED);
    
    if (new_addr == MAP_FAILED) {
        /* 8. Verify the backing file was successfully shrunk back to its initial size */
        struct swap_file_info shrunk_info = get_swap_file_info(addr);
        ASSERT_EQ(shrunk_info.file_size, initial_size);
    } else {
        // Cleanup if the test fails and actually expands the memory
        munmap(new_addr, expanded_size);
        fprintf(stderr, "Test invalid: mremap succeeded, kernel fault injection missed.\n");
    }

    munmap(addr, initial_size);
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

void test_single_vma_growsdown(void) {
    size_t initial_size = PAGE_SIZE;
    
    /* 
     * The kernel enforces a stack_guard_gap (default 256 pages).
     * We reserve enough space for the guard gap plus our target VMA.
     */
    size_t lower_vma_size = PAGE_SIZE * 256;
    size_t total_reserved_size = lower_vma_size + initial_size;

    /* 1. Find a safe, contiguous region in the address space */
    unsigned char *reserved = mmap(NULL, total_reserved_size, PROT_NONE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(reserved != MAP_FAILED);
    if (reserved == MAP_FAILED) return;

    /* Unmap it so we can safely carve it up using MAP_FIXED */
    ASSERT_EQ(munmap(reserved, total_reserved_size), 0);

    /* 2. Map the "earlier" (lower) VMA to reserve the physical layout */
    unsigned char *lower_vma = mmap(reserved, lower_vma_size, PROT_NONE, 
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    ASSERT(lower_vma == reserved);

    /* 3. Map our actual downward-growing named swap VMA directly above it */
    unsigned char *addr = mmap(reserved + lower_vma_size, initial_size, 
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP | MAP_GROWSDOWN | MAP_FIXED, 
                               -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED) return;

    /* 4. Unmap the earlier VMA to guarantee the space below is 100% free */
    ASSERT_EQ(munmap(lower_vma, lower_vma_size), 0);

    /* Fault the initial allocated page */
    addr[0] = 0xAA;

    struct vma_info_args initial_vma = get_vma_info(addr);
    struct swap_file_info initial_file = get_swap_file_info(addr);

    ASSERT_EQ(initial_vma.vma_start, (unsigned long)addr);
    ASSERT_EQ(initial_file.file_size, initial_size); 

    /* 5. Target the page immediately below the current mapping */
    unsigned char *lower_addr = addr - PAGE_SIZE;

    printf("Targeting fault at address: %p\n", lower_addr);
    printf("--> PAUSED: Switch to host GDB, set your conditional breakpoint, then press Enter here.\n");
    
    /* Flush stdout so you definitely see the message before it hangs */
    fflush(stdout); 
    
    /* Wait for you to press Enter */
    getchar();

    /* 
     * 6. Trigger downward growth natively. 
     * Because we unmapped the lower VMA, this space is guaranteed empty
     * and free from stack_guard_gap collisions.
     */
    lower_addr[0] = 0xBB;

    /* 7. Verify the VMA successfully grew down */
    struct vma_info_args expanded_vma = get_vma_info(lower_addr);
    
    /* The start address should have shifted down by exactly one page */
    ASSERT_EQ(expanded_vma.vma_start, (unsigned long)lower_addr);
    /* The end address must remain anchored to the original boundary */
    ASSERT_EQ(expanded_vma.vma_end, initial_vma.vma_end);

    /* 8. Verify the underlying named swap file expanded to match */
    struct swap_file_info expanded_file = get_swap_file_info(lower_addr);
    
    /* Assert the exact expanded file size */
    ASSERT_EQ(expanded_file.file_size, initial_size + PAGE_SIZE);

    /* 9. Cleanup the entire expanded VMA */
    munmap(expanded_vma.vma_start, expanded_vma.vma_end - expanded_vma.vma_start);
}

void test_mremap_left_enlarge_only_fails_deliberately(void) {
    size_t page_size = PAGE_SIZE;
    size_t total_size = page_size * 20;

    /* 1. Map continuous region for anon_vma compatibility */
    unsigned char *addr = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED) return;

    for (int i = 0; i < 20; i++) {
        addr[i * page_size] = (unsigned char)i;
    }

    /* 
     * 2. Sculpt the memory landscape.
     * Page 0: Left Anchor (Case 1)
     * Page 1: Hole
     * Page 2: Right Anchor (Case 1)
     * Page 3: Hole
     * Page 4: Moving Source (Case 2 & 5)
     * Page 5: Hole
     * Page 6: Isolated Target (Case 2)
     * Page 7: Hole
     * Page 8: Left Anchor (Case 3)
     * Page 9: Target (Case 3)
     * Page 10: Hole
     * Page 11: Target (Case 5)
     * Page 12: Right Anchor (Case 5)
     * Page 13: Hole
     * Page 14: Left Anchor (Case 4)
     * Page 15: Target (Case 4)
     * Page 16: Right Anchor (Case 4)
     * Page 17: Moving Source (Case 3 & 4)
     */
    munmap(addr + 1 * page_size, 1 * page_size);
    munmap(addr + 3 * page_size, 1 * page_size);
    munmap(addr + 5 * page_size, 1 * page_size);
    munmap(addr + 7 * page_size, 1 * page_size);
    munmap(addr + 10 * page_size, 1 * page_size);
    munmap(addr + 13 * page_size, 1 * page_size);

    void *res;

    /* 
     * CASE 1: Standard In-Place Expansion (Expands Right)
     * Expand Page 0 into Hole 1. Should consume Page 2.
     * i.e that validates that in mremap no MAYMOVE, the left VMA survives and the right VMA is destroyed.
     */
    printf("CASE 1: In-Place Expansion (Expands Right)\n");
    struct vma_info_args c1_left = get_vma_info(addr + 0);
    struct vma_info_args c1_right = get_vma_info(addr + 2 * page_size);
    ASSERT(c1_left.vma_ptr != NULL && c1_right.vma_ptr != NULL);
    ASSERT_NEQ(c1_left.vma_ptr, c1_right.vma_ptr);

    res = mremap(addr + 0, page_size, 2 * page_size, 0);
    ASSERT_EQ(res, addr + 0);

    struct vma_info_args c1_merged = get_vma_info(addr + 0);
    ASSERT_EQ(c1_merged.vma_start, c1_left.vma_start);
    /* PROOF: Left structure survived, Right structure was destroyed - trough the existence of the vma struct*/
    ASSERT_EQ(c1_merged.vma_ptr, c1_left.vma_ptr);
    ASSERT_NEQ(c1_merged.vma_ptr, c1_right.vma_ptr);


    /* 
     * CASE 2: MREMAP_MAYMOVE to isolated hole (No Neighbors)
     * Move Page 4 to Hole 6.
     * when moved to a hole with no neighbors, a new vma is born - we dont inspect vma_ptr
     */
    printf("CASE 2: MREMAP_MAYMOVE to isolated hole (No Neighbors)\n");
    struct vma_info_args c2_src = get_vma_info(addr + 4 * page_size);
    res = mremap(addr + 4 * page_size, page_size, page_size, MREMAP_MAYMOVE | MREMAP_FIXED, addr + 6 * page_size);
    ASSERT_EQ(res, addr + 6 * page_size);
    
    struct vma_info_args c2_target = get_vma_info(addr + 6 * page_size);
    ASSERT_EQ(c2_target.vma_start, (unsigned long)(addr + 6 * page_size)); 


    /* 
     * CASE 3: MREMAP_MAYMOVE to hole with ONLY Left Neighbor
     * Move Page 17 to Hole 9. Left neighbor is Page 8.
     */
    printf("CASE 3: MREMAP_MAYMOVE to hole with ONLY Left Neighbor\n");
    struct vma_info_args c3_left = get_vma_info(addr + 8 * page_size);
    
    res = mremap(addr + 17 * page_size, page_size, page_size, MREMAP_MAYMOVE | MREMAP_FIXED, addr + 9 * page_size);
    ASSERT_EQ(res, addr + 9 * page_size);
    
    struct vma_info_args c3_merged = get_vma_info(addr + 8 * page_size);
    ASSERT_EQ(c3_merged.vma_start, c3_left.vma_start); 
    /* PROOF: Existing Left structure consumed the incoming mapping */
    ASSERT_EQ(c3_merged.vma_ptr, c3_left.vma_ptr); 


    /* 
     * CASE 4: MREMAP_MAYMOVE to hole with BOTH Left and Right Neighbors
     * Move Page 18 to Hole 15. Neighbors: 14 (Left) and 16 (Right).
     */
    printf("CASE 4: MREMAP_MAYMOVE to hole with BOTH Left and Right Neighbors\n");
    struct vma_info_args c4_left = get_vma_info(addr + 14 * page_size);
    struct vma_info_args c4_right = get_vma_info(addr + 16 * page_size);
    
    res = mremap(addr + 18 * page_size, page_size, page_size, MREMAP_MAYMOVE | MREMAP_FIXED, addr + 15 * page_size);
    ASSERT_EQ(res, addr + 15 * page_size);
    
    struct vma_info_args c4_merged = get_vma_info(addr + 14 * page_size);
    ASSERT_EQ(c4_merged.vma_start, c4_left.vma_start); 
    /* PROOF: Left structure consumed BOTH the incoming mapping and the Right structure */
    ASSERT_EQ(c4_merged.vma_ptr, c4_left.vma_ptr);
    ASSERT_NEQ(c4_merged.vma_ptr, c4_right.vma_ptr);


    /* 
     * CASE 5: MREMAP_MAYMOVE to hole with ONLY Right Neighbor
     * Move Page 6 (from Case 2) to Hole 11. Right neighbor is Page 12.
     * THIS IS THE EXCLUSIVE LEFT ENLARGEMENT SCENARIO.
     */
    printf("CASE 5: MREMAP_MAYMOVE to hole with ONLY Right Neighbor\n");
    struct vma_info_args c5_right = get_vma_info(addr + 12 * page_size);
    
    res = mremap(addr + 6 * page_size, page_size, page_size, MREMAP_MAYMOVE | MREMAP_FIXED, addr + 11 * page_size);
    ASSERT_EQ(res, addr + 11 * page_size);
    
    struct vma_info_args c5_merged = get_vma_info(addr + 11 * page_size);
    
    /* PROOF 1: The Right VMA is the surviving metadata structure */
    ASSERT_EQ(c5_merged.vma_ptr, c5_right.vma_ptr); 
    
    /* PROOF 2: The surviving structure's start boundary physically shifted Left */
    ASSERT_EQ(c5_merged.vma_start, c5_right.vma_start - page_size);

    /* Clean up */
    munmap(addr, total_size);
}

void test_mremap_left_enlarge_only_evict_and_resume(void) {
    size_t page_size = PAGE_SIZE;

    /* 
     * Allocate a continuous base block to guarantee anon_vma compatibility 
     * and continuous pgoff across all pages.
     */
    unsigned char *base = mmap(NULL, 10 * page_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(base != MAP_FAILED);

    /* 
     * Allocate and IMMEDIATELY UNMAP the scratch space.
     * This reserves a known-valid virtual address range that is guaranteed 
     * to be completely empty, avoiding any unintended VMA merges during eviction.
     */
    unsigned char *scratch = mmap(NULL, 10 * page_size, PROT_NONE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(scratch != MAP_FAILED);
    munmap(scratch, 10 * page_size);

    /* Fault in the base pages to establish kernel structures and anon_vma */
    for(int i = 0; i < 10; i++) {
        base[i * page_size] = (unsigned char)i;
    }

    /* 
     * Isolate our test islands by punching holes so they do not interfere.
     * Island 1 (Pages 0-1): For Right VMA expanding left
     * Island 2 (Pages 3-4): For Left VMA expanding right
     * Island 3 (Pages 6-8): For Left VMA consuming both
     */
    munmap(base + 2 * page_size, page_size);
    munmap(base + 5 * page_size, page_size);
    munmap(base + 9 * page_size, page_size);

    void *res;

    /* =====================================================================
     * SCENARIO 1: Right VMA Expands Left (THE EXCLUSIVE LEFT ENLARGEMENT)
     * Island 1: base[0], base[1]
     * ===================================================================== */
    
    /* Step 1: Evict base[0]. Leaves base[1] isolated. */
    printf("SCENARIO 1: Right VMA Expands Left (Exclusive Left Enlargement)\n");
    res = mremap(base + 0, page_size, page_size, MREMAP_MAYMOVE | MREMAP_FIXED, scratch + 0);
    ASSERT_EQ(res, scratch + 0);
    
    struct vma_info_args orig_right_c1 = get_vma_info(base + 1 * page_size);
    
    /* Step 2: Return base[0] to its home. Neighbor is ONLY base[1] (Right). */
    res = mremap(scratch + 0, page_size, page_size, MREMAP_MAYMOVE | MREMAP_FIXED, base + 0);
    ASSERT_EQ(res, base + 0);
    
    struct vma_info_args merged_c1 = get_vma_info(base + 0);
    
    /* PROOF: The Right VMA metadata pointer survived, and its start boundary shifted left */
    ASSERT_EQ(merged_c1.vma_ptr, orig_right_c1.vma_ptr); 
    ASSERT_EQ(merged_c1.vma_start, orig_right_c1.vma_start - page_size); 


    /* =====================================================================
     * SCENARIO 2: Left VMA Expands Right (Standard Consume)
     * Island 2: base[3], base[4]
     * ===================================================================== */
     
    /* Step 1: Evict base[4]. Leaves base[3] isolated. */
    printf("SCENARIO 2: Left VMA Expands Right (Standard Consume)\n");
    res = mremap(base + 4 * page_size, page_size, page_size, MREMAP_MAYMOVE | MREMAP_FIXED, scratch + 4 * page_size);
    ASSERT_EQ(res, scratch + 4 * page_size);
    
    struct vma_info_args orig_left_c2 = get_vma_info(base + 3 * page_size);
    
    /* 
     * Step 2: Return base[4] to its home. 
     * Neighbor is ONLY base[3] (Left) because base[5] is a hole! 
     */
    res = mremap(scratch + 4 * page_size, page_size, page_size, MREMAP_MAYMOVE | MREMAP_FIXED, base + 4 * page_size);
    ASSERT_EQ(res, base + 4 * page_size);
    
    struct vma_info_args merged_c2 = get_vma_info(base + 3 * page_size);
    
    /* PROOF: The Left VMA metadata pointer survived, and its start boundary did not move */
    ASSERT_EQ(merged_c2.vma_ptr, orig_left_c2.vma_ptr); 
    ASSERT_EQ(merged_c2.vma_start, orig_left_c2.vma_start); 


    /* =====================================================================
     * SCENARIO 3: Left VMA Expands Both (Dual Consume)
     * Island 3: base[6], base[7], base[8]
     * ===================================================================== */
     
    /* Step 1: Evict the middle piece (base[7]). Leaves base[6] (Left) and base[8] (Right). */
    printf("SCENARIO 3: Left VMA Expands Both (Dual Consume)\n");
    res = mremap(base + 7 * page_size, page_size, page_size, MREMAP_MAYMOVE | MREMAP_FIXED, scratch + 7 * page_size);
    ASSERT_EQ(res, scratch + 7 * page_size);
    
    struct vma_info_args orig_both_l = get_vma_info(base + 6 * page_size);
    struct vma_info_args orig_both_r = get_vma_info(base + 8 * page_size);
    
    /* Step 2: Return base[7] to its home. Neighbors are base[6] and base[8]. */
    res = mremap(scratch + 7 * page_size, page_size, page_size, MREMAP_MAYMOVE | MREMAP_FIXED, base + 7 * page_size);
    ASSERT_EQ(res, base + 7 * page_size);
    
    struct vma_info_args merged_c3 = get_vma_info(base + 6 * page_size);
    
    /* PROOF: The Left VMA metadata pointer survived, consumed Right, and start boundary did not move */
    ASSERT_EQ(merged_c3.vma_ptr, orig_both_l.vma_ptr); 
    ASSERT_NEQ(merged_c3.vma_ptr, orig_both_r.vma_ptr); 
    ASSERT_EQ(merged_c3.vma_start, orig_both_l.vma_start); 

    /* =====================================================================
     * SCENARIO 4: Unfaulted Merge Attempt (The "Cheat Code" - kenel invents vm_pgoff 
     * while moving unfaulted vma's with no fault to increase likelihood of merge)
     * 
     * We guarantee spatial isolation so the kernel cannot preemptively merge.
     * ===================================================================== */
    
    printf("SCENARIO 4: Unfaulted Merge Attempt (The \"Cheat Code\")\n");
    
    /* 1. Create a massive 10-page arena and unmap it so we control the exact addresses */
    unsigned char *arena = mmap(NULL, 10 * page_size, PROT_NONE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(arena != MAP_FAILED);
    munmap(arena, 10 * page_size);

    /* 2. Map Source far away on the right (Page 8). Add (to fail) or remove (to success) MAP_NAMED_SWAP to inspect differene in behavior */
    unsigned char *unfaulted_src = mmap(arena + 8 * page_size, page_size, 
                                        PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    ASSERT(unfaulted_src != MAP_FAILED);

    /* 3. Map Right Anchor near the left (Page 1). Add (to fail) or remove (to success) MAP_NAMED_SWAP to inspect differene in behavior  */
    unsigned char *unfaulted_right = mmap(arena + 1 * page_size, page_size, 
                                          PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    ASSERT(unfaulted_right != MAP_FAILED);
    
    /* The destination is exactly to the left of the Right Anchor (Page 0) */
    unsigned char *unfaulted_dest_area = arena;

    /* Capture initial state */
    struct vma_info_args orig_unfaulted_src = get_vma_info(unfaulted_src);
    struct vma_info_args orig_unfaulted_right = get_vma_info(unfaulted_right);

    printf("  [Before Move]\n");
    printf("  Source VMA  -> ptr: %p, start: %p\n", 
           (void*)orig_unfaulted_src.vma_ptr, (void*)orig_unfaulted_src.vma_start);
    printf("  Right VMA   -> ptr: %p, start: %p\n", 
           (void*)orig_unfaulted_right.vma_ptr, (void*)orig_unfaulted_right.vma_start);

    /* 4. Move Source directly to the left of the Right Anchor */
    void *reso = mremap(unfaulted_src, page_size, page_size, MREMAP_MAYMOVE | MREMAP_FIXED, unfaulted_dest_area);
    ASSERT_EQ(reso, unfaulted_dest_area);

    struct vma_info_args merged_c4 = get_vma_info(unfaulted_dest_area);

    printf("  [After Move]\n");
    printf("  Result VMA  -> ptr: %p, start: %p\n", 
           (void*)merged_c4.vma_ptr, (void*)merged_c4.vma_start);

    /* 
     * PROOF: The cheat code dynamically forged the offset and forced the merge!
     */
    ASSERT_EQ(merged_c4.vma_ptr, orig_unfaulted_right.vma_ptr);
    ASSERT_EQ(merged_c4.vma_start, orig_unfaulted_right.vma_start - page_size);

    /* Clean up */
    munmap(arena, 10 * page_size);
}

void test_simple_mremap_to_the_left_case(void) {
    size_t page_size = PAGE_SIZE;
    size_t total_size = page_size * 2;

    /* 1. Map and immediately unmap 2 pages to reserve a contiguous virtual address area */
    unsigned char *initial_area = mmap(NULL, total_size, PROT_NONE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(initial_area != MAP_FAILED);
    if (initial_area == MAP_FAILED) return;
    
    ASSERT_EQ(munmap(initial_area, total_size), 0);

    /* 2. Map exactly one page into the RIGHT half of this reserved area (the second page) */
    unsigned char *old_address = initial_area + page_size;
    unsigned char *addr = mmap(old_address, page_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP | MAP_FIXED, -1, 0);
    
    ASSERT_EQ(addr, old_address);
    if (addr == MAP_FAILED) return;

    /* Fault the page to establish kernel structures */
    addr[0] = 0xAA;

    /* 3. mremap the page to the LEFT half (the first page), expanding it to cover both pages */
    unsigned char *new_start = initial_area;
    
    void *res = mremap(old_address, 
                       page_size, 
                       total_size, 
                       MREMAP_MAYMOVE | MREMAP_FIXED, 
                       new_start);

    /* 4. Verify mremap did not fail and returned the expected left-shifted address */
    ASSERT(res != MAP_FAILED);
    ASSERT_EQ(res, new_start);

    /* 5. Cleanup */
    if (res != MAP_FAILED) {
        munmap(res, total_size);
    } else {
        munmap(old_address, page_size);
    }
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
