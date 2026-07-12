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
//REGISTER_TEST(test_mprotect_split_middle);

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
    ASSERT_NEQ(before.path, NULL);
    ASSERT_EQ(before.file_size, len); /* Apparent size should be 3 pages */

    /* The filesystem allocates i_blocks in 512-byte units. */
    /* 3 pages * 4096 bytes = 12288 bytes. 12288 / 512 = 24 blocks. */
    unsigned long initial_blocks = before.allocated_blocks;
    ASSERT_ABOVE(initial_blocks, 0); 

    /* 3. Unmap the middle page to trigger named_swap_deallocate */
    ASSERT_EQ(munmap(addr + PAGE_SIZE, PAGE_SIZE), 0);

    /* 4. Verify the left and right pages are still intact */
    ASSERT_EQ_AT(addr, 0x11);
    ASSERT_EQ_AT(addr + PAGE_SIZE * 2, 0x33);

    /* 5. Verify accessing the unmapped middle page causes a segfault */
    ASSERT_SIGNAL(SIGSEGV) {
        addr[PAGE_SIZE] = 0x44;
    }

    /* 6. Verify the apparent backing file size remains unchanged due to KEEP_SIZE */
    struct swap_file_info after = get_swap_file_info(addr);
    ASSERT_EQ(after.file_size, len);

    /* 
     * 7. Verify the physical blocks decreased by exactly the unmapped size.
     * i_blocks are measured in 512-byte sectors.
     * We unmapped exactly 1 PAGE_SIZE (4096 bytes).
     * 4096 / 512 = 8 blocks should have been freed.
     */
    unsigned long after_blocks = after.allocated_blocks;
    unsigned long blocks_freed = PAGE_SIZE / 512;
    
    ASSERT_EQ(initial_blocks, after_blocks + blocks_freed);

    /* Cleanup the remaining left and right VMAs */
    munmap(addr, PAGE_SIZE);
    munmap(addr + PAGE_SIZE * 2, PAGE_SIZE);
}



/*
void test_mprotect_split_middle(void) {
    size_t len = PAGE_SIZE * 3;
    unsigned char *addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP,
                               -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;

    addr[0] = 0x11;
    addr[PAGE_SIZE] = 0x22;
    addr[PAGE_SIZE * 2] = 0x33;

    struct swap_file_info before = get_swap_file_info(addr);
    ASSERT_NEQ(before.path, NULL);
    ASSERT_EQ(before.file_size, len);

    ASSERT_EQ(mprotect(addr + PAGE_SIZE, PAGE_SIZE, PROT_READ), 0);

    ASSERT_EQ_AT(addr + 0, 0x11);
    ASSERT_EQ_AT(addr + PAGE_SIZE, 0x22);
    ASSERT_EQ_AT(addr + PAGE_SIZE * 2, 0x33);

    ASSERT_EQ(count_rmap_vmas(addr), count_rmap_vmas(addr + PAGE_SIZE * 2));

    struct vma_info_args left_vma = get_vma_info(addr);
    struct vma_info_args mid_vma = get_vma_info(addr + PAGE_SIZE);
    struct vma_info_args right_vma = get_vma_info(addr + PAGE_SIZE * 2);

    ASSERT(left_vma.vma_ptr != NULL);
    ASSERT(mid_vma.vma_ptr != NULL);
    ASSERT(right_vma.vma_ptr != NULL);
    ASSERT_NEQ(left_vma.vma_ptr, mid_vma.vma_ptr);
    ASSERT_NEQ(mid_vma.vma_ptr, right_vma.vma_ptr);

    ASSERT_SIGNAL(SIGSEGV) {
        addr[PAGE_SIZE] = 0x44;
    }

    struct swap_file_info after = get_swap_file_info(addr);
    ASSERT_EQ(after.file_size, len);

    munmap(addr, len);
}
*/

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
