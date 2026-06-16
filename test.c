#include "test_framework.h"
#include "test_util.h"

#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE_SIZE 4096
#define MAP_NAMED_SWAP 0x200000
#define MIN_PAGE_NAMED_SWAP_MMAP 256 // should be read from sysctl
#define VM_MEMORY 130*(1024*1024) // 130MB

REGISTER_TEST(test_single_anon_vma);
REGISTER_TEST(test_fork_anon_vma);
REGISTER_TEST(test_count_rmap_vmas);
REGISTER_TEST(test_swap_file_creation);
REGISTER_TEST(test_swap_file_delete_unmap);
REGISTER_TEST(test_swap_file_delete_exit);
REGISTER_TEST(test_zero_file);
REGISTER_TEST(test_read_first_fault);
REGISTER_TEST(test_write_fault);
REGISTER_TEST(test_swapout_folio);
REGISTER_TEST(test_memory_pressure);

void test_memory_pressure(void) {
    unsigned char *addr = mmap(NULL, VM_MEMORY, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    for (int it = 0; it < 2; it++) {
        for (int i = 0; i < VM_MEMORY; i+=PAGE_SIZE) {
            if (it == 0) {
                addr[i] = i%256;
                ASSERT_EQ_AT(addr + i, i%256);
                ASSERT_EQ(get_folio_mapcount(addr + i), 1);
            }
            unsigned long pte_value = get_pte_value(addr + i);
            // printf("pte_value: %lx\n", pte_value);
            ASSERT_EQ(get_folio_mapcount(addr + i), 1);
            addr[i]++;
            ASSERT_EQ_AT(addr + i, i%256 + (it + 1));
            ASSERT_EQ(get_folio_mapcount(addr + i), 1);
        }
    }

    munmap(addr, PAGE_SIZE * 2);
}

void test_swapout_folio(void) {
    unsigned char *addr = mmap(NULL, PAGE_SIZE * 2, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    for (int i = 0; i < PAGE_SIZE * 2; i+=PAGE_SIZE) {
        addr[i] = i%256;
        ASSERT_EQ_AT(addr + i, i%256);
        ASSERT_EQ(get_folio_mapcount(addr + i), 1);
        madvise(addr, PAGE_SIZE * 2, MADV_PAGEOUT);
        unsigned long pte_value = get_pte_value(addr + i);
        printf("pte_value: %lx\n", pte_value);
        ASSERT_EQ(get_folio_mapcount(addr + i), 1);
        addr[i]++;
        ASSERT_EQ_AT(addr + i, i%256 + 1);
        ASSERT_EQ(get_folio_mapcount(addr + i), 1);
    }

    munmap(addr, PAGE_SIZE * 2);
}

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
    int child_ready[2] = {-1, -1};
    int parent_done[2] = {-1, -1};
    pid_t pid;
    int status = 0;
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
    ASSERT_EQ(pipe(child_ready), 0);
    ASSERT_EQ(pipe(parent_done), 0);
    if (child_ready[0] < 0 || child_ready[1] < 0 ||
        parent_done[0] < 0 || parent_done[1] < 0)
        return;

    pid = fork();
    ASSERT_NEQ(pid, -1);
    if (pid < 0) {
        close(child_ready[0]);
        close(child_ready[1]);
        close(parent_done[0]);
        close(parent_done[1]);
        return;
    }

    if (pid){
        close(child_ready[1]);
        close(parent_done[0]);
        ASSERT_EQ(wait_fd(child_ready[0]), 0);
        close(child_ready[0]);
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
        ASSERT_EQ(signal_fd(parent_done[1]), 0);
        close(parent_done[1]);
        ASSERT_EQ(waitpid(pid, &status, 0), pid);
        ASSERT(WIFEXITED(status));
        if (WIFEXITED(status))
            ASSERT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);
    }
    else {
        close(child_ready[0]);
        close(parent_done[1]);
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
        ASSERT_EQ(signal_fd(child_ready[1]), 0);
        close(child_ready[1]);
        ASSERT_EQ(wait_fd(parent_done[0]), 0);
        close(parent_done[0]);
        exit(current_test_failed ? EXIT_FAILURE : EXIT_SUCCESS);
    }

}

void test_count_rmap_vmas(void) {
    int child_ready[2] = {-1, -1};
    int parent_done[2] = {-1, -1};
    pid_t pid;
    int status = 0;
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
    ASSERT_EQ(pipe(child_ready), 0);
    ASSERT_EQ(pipe(parent_done), 0);
    if (child_ready[0] < 0 || child_ready[1] < 0 ||
        parent_done[0] < 0 || parent_done[1] < 0)
        return;

    pid = fork();
    ASSERT_NEQ(pid, -1);
    if (pid < 0) {
        close(child_ready[0]);
        close(child_ready[1]);
        close(parent_done[0]);
        close(parent_done[1]);
        return;
    }

    if (pid){
        close(child_ready[1]);
        close(parent_done[0]);
        ASSERT_EQ(wait_fd(child_ready[0]), 0);
        close(child_ready[0]);
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
        ASSERT_EQ(signal_fd(parent_done[1]), 0);
        close(parent_done[1]);
        ASSERT_EQ(waitpid(pid, &status, 0), pid);
        ASSERT(WIFEXITED(status));
        if (WIFEXITED(status))
            ASSERT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);
    }
    else {
        close(child_ready[0]);
        close(parent_done[1]);
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
        ASSERT_EQ(signal_fd(child_ready[1]), 0);
        close(child_ready[1]);
        ASSERT_EQ(wait_fd(parent_done[0]), 0);
        close(parent_done[0]);
        exit(current_test_failed ? EXIT_FAILURE : EXIT_SUCCESS);
    }

}
void test_mulcount_rmap_vmas_multi_fork(void) {
    int child_ready[2] = {-1, -1};
    int child_continue[2] = {-1, -1};
    int parent_gc_ready[2] = {-1, -1};
    int parent_gc_done[2] = {-1, -1};
    pid_t child_pid;
    pid_t parent_gc_pid = -1;
    int status = 0;
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
    ASSERT_EQ(pipe(child_ready), 0);
    ASSERT_EQ(pipe(child_continue), 0);
    ASSERT_EQ(pipe(parent_gc_ready), 0);
    ASSERT_EQ(pipe(parent_gc_done), 0);
    if (child_ready[0] < 0 || child_ready[1] < 0 ||
        child_continue[0] < 0 || child_continue[1] < 0 ||
        parent_gc_ready[0] < 0 || parent_gc_ready[1] < 0 ||
        parent_gc_done[0] < 0 || parent_gc_done[1] < 0)
        return;

    child_pid = fork();
    ASSERT_NEQ(child_pid, -1);
    if (child_pid < 0)
        return;

    if (child_pid){
        close(child_ready[1]);
        close(child_continue[0]);
        close(parent_gc_ready[1]);
        close(parent_gc_done[0]);

        ASSERT_EQ(wait_fd(child_ready[0]), 0);
        close(child_ready[0]);
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
        //checks on the shared page
        ASSERT_EQ(count_rmap_vmas(addr), 3);
        //checks on page that the child cowed, identical to page 1
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE), 3);
        //checks on page that the parent cowed
        addr[PAGE_SIZE * 2]++;
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE * 2), 1);

        parent_gc_pid = fork();
        ASSERT_NEQ(parent_gc_pid, -1);
        if (parent_gc_pid < 0) {
            ASSERT_EQ(signal_fd(child_continue[1]), 0);
            close(child_continue[1]);
            ASSERT_EQ(waitpid(child_pid, &status, 0), child_pid);
            return;
        }
        if (parent_gc_pid == 0) {
            close(child_continue[1]);
            close(parent_gc_ready[0]);
            close(parent_gc_done[1]);
            ASSERT_EQ(signal_fd(parent_gc_ready[1]), 0);
            close(parent_gc_ready[1]);
            ASSERT_EQ(wait_fd(parent_gc_done[0]), 0);
            close(parent_gc_done[0]);
            exit(current_test_failed ? EXIT_FAILURE : EXIT_SUCCESS);
        }
        ASSERT_EQ(wait_fd(parent_gc_ready[0]), 0);
        close(parent_gc_ready[0]);

        //checks on page that the parent allocated
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE * 3), 2);
        ASSERT_EQ(signal_fd(child_continue[1]), 0);
        close(child_continue[1]);
        ASSERT_EQ(waitpid(child_pid, &status, 0), child_pid);
        ASSERT(WIFEXITED(status));
        if (WIFEXITED(status))
            ASSERT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);
        ASSERT_EQ(signal_fd(parent_gc_done[1]), 0);
        close(parent_gc_done[1]);
        ASSERT_EQ(waitpid(parent_gc_pid, &status, 0), parent_gc_pid);
        ASSERT(WIFEXITED(status));
        if (WIFEXITED(status))
            ASSERT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);
    }
    else {
        int child_gc_ready[2] = {-1, -1};
        int child_gc_done[2] = {-1, -1};
        pid_t child_gc_pid;

        close(child_ready[0]);
        close(child_continue[1]);
        close(parent_gc_ready[0]);
        close(parent_gc_ready[1]);
        close(parent_gc_done[0]);
        close(parent_gc_done[1]);

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

        ASSERT_EQ(pipe(child_gc_ready), 0);
        ASSERT_EQ(pipe(child_gc_done), 0);
        if (child_gc_ready[0] < 0 || child_gc_ready[1] < 0 ||
            child_gc_done[0] < 0 || child_gc_done[1] < 0)
            exit(EXIT_FAILURE);

        child_gc_pid = fork();
        ASSERT_NEQ(child_gc_pid, -1);
        if (child_gc_pid < 0)
            exit(EXIT_FAILURE);
        if(!child_gc_pid) {
            close(child_ready[1]);
            close(child_continue[0]);
            close(child_gc_ready[0]);
            close(child_gc_done[1]);
            ASSERT_EQ(signal_fd(child_gc_ready[1]), 0);
            close(child_gc_ready[1]);
            ASSERT_EQ(wait_fd(child_gc_done[0]), 0);
            close(child_gc_done[0]);
            exit(current_test_failed ? EXIT_FAILURE : EXIT_SUCCESS);
        }

        close(child_gc_ready[1]);
        close(child_gc_done[0]);
        ASSERT_EQ(wait_fd(child_gc_ready[0]), 0);
        close(child_gc_ready[0]);
        ASSERT_EQ(signal_fd(child_ready[1]), 0);
        close(child_ready[1]);
        ASSERT_EQ(wait_fd(child_continue[0]), 0);
        close(child_continue[0]);
        //checks on the shared page
        ASSERT_EQ(count_rmap_vmas(addr), 4);
        //checks on page that the child cowed, private to the child
        addr[PAGE_SIZE]++;
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE), 1);
        //checks on page that the parent cowed
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE * 2), 4);
        //checks on page that the child allocated
        ASSERT_EQ(count_rmap_vmas(addr + PAGE_SIZE * 3), 2);
        ASSERT_EQ(signal_fd(child_gc_done[1]), 0);
        close(child_gc_done[1]);
        ASSERT_EQ(waitpid(child_gc_pid, &status, 0), child_gc_pid);
        ASSERT(WIFEXITED(status));
        if (WIFEXITED(status))
            ASSERT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);
        exit(current_test_failed ? EXIT_FAILURE : EXIT_SUCCESS);
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
