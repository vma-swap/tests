#include "test_framework.h"
#include "test_util.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE_SIZE 4096

REGISTER_TEST(test_single_anon_vma);
REGISTER_TEST(test_fork_anon_vma);
REGISTER_TEST(test_count_rmap_vmas);

void test_single_anon_vma(void) {
    char *addr = mmap(NULL, PAGE_SIZE * 10, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    for (int i = 0; i < 10; i++) {
        struct anon_vma_info_args anon_vma_info = get_anon_vma_info(addr + (i * PAGE_SIZE));
        ASSERT_EQ(anon_vma_info.anon_vma, NULL);
    }
    addr[0] = 1;
    struct anon_vma_info_args first_anon_vma_info = get_anon_vma_info(addr);
    for (int i = 1; i < 10; i++) {
        addr[i * PAGE_SIZE] = i;
        struct anon_vma_info_args anon_vma_info = get_anon_vma_info(addr + (i * PAGE_SIZE));
        ASSERT_EQ(anon_vma_info.anon_vma, first_anon_vma_info.anon_vma);
        ASSERT_EQ(anon_vma_info.root, first_anon_vma_info.anon_vma);
        ASSERT_EQ(anon_vma_info.parent, first_anon_vma_info.anon_vma);
        ASSERT_EQ(anon_vma_info.num_children, 1);
        ASSERT_EQ(anon_vma_info.num_active_vmas, 1);
    }
}

void test_fork_anon_vma(void) {
    char *addr = mmap(NULL, PAGE_SIZE * 4, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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
        ASSERT_EQ(page_2.anon_vma, parent_original.anon_vma);
        ASSERT(page_2.anon_vma != NULL);
        ASSERT_EQ(page_2.root, parent_original.anon_vma);
        ASSERT_EQ(page_2.parent, parent_original.anon_vma);
        ASSERT_EQ(page_2.num_children, 3);
        ASSERT_EQ(page_2.num_active_vmas, 0);
        //checks on page that the parent cowed
        addr[PAGE_SIZE * 2]++;
        struct anon_vma_info_args page_3 = get_anon_vma_info(addr + PAGE_SIZE * 2);
        ASSERT(page_3.anon_vma != parent_original.anon_vma);
        ASSERT(page_3.anon_vma != NULL);
        ASSERT_EQ(page_3.root, parent_original.anon_vma);
        ASSERT_EQ(page_3.parent, parent_original.anon_vma);
        ASSERT_EQ(page_3.num_children, 0);
        ASSERT_EQ(page_3.num_active_vmas, 1);
        //checks on page that the parent allocated
        addr[PAGE_SIZE * 3] = 4;
        struct anon_vma_info_args page_4 = get_anon_vma_info(addr + PAGE_SIZE * 3);
        ASSERT_EQ(page_4.anon_vma, page_3.anon_vma);
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
        ASSERT(page_2.anon_vma != parent_original.anon_vma);
        ASSERT(page_2.anon_vma != NULL);
        ASSERT_EQ(page_2.root, parent_original.anon_vma);
        ASSERT_EQ(page_2.parent, parent_original.anon_vma);
        ASSERT_EQ(page_2.num_children, 0);
        ASSERT_EQ(page_2.num_active_vmas, 1);
        //checks on page that the parent cowed
        struct anon_vma_info_args page_3 = get_anon_vma_info(addr + PAGE_SIZE*2);
        ASSERT_EQ(page_3.anon_vma, parent_original.anon_vma);
        ASSERT(page_3.anon_vma != NULL);
        ASSERT_EQ(page_3.root, parent_original.anon_vma);
        ASSERT_EQ(page_3.parent, parent_original.anon_vma);
        ASSERT_EQ(page_3.num_children, 3);
        ASSERT_EQ(page_3.num_active_vmas, 0);
        //checks on page that the child allocated
        addr[PAGE_SIZE * 3] = 4;
        struct anon_vma_info_args page_4 = get_anon_vma_info(addr + PAGE_SIZE * 3);
        ASSERT_EQ(page_4.anon_vma, page_2.anon_vma);
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
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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
