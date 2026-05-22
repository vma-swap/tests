#include "test_framework.h"
#include "test_util.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE_SIZE 4096

REGISTER_TEST(test_anon_vma);

void test_anon_vma(void) {
    char *addr = mmap(NULL, PAGE_SIZE * 10, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;

    addr[0] = 1;

    struct anon_vma_info_args parent_before = get_anon_vma_info(addr);
    ASSERT_NEQ(parent_before.anon_vma, NULL);
    ASSERT_NEQ(parent_before.root, NULL);

    int child_info_pipe[2];
    int release_pipe[2];
    if (pipe(child_info_pipe) < 0 || pipe(release_pipe) < 0) {
        perror("pipe");
        ASSERT(0);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        ASSERT(0);
        close(child_info_pipe[0]);
        close(child_info_pipe[1]);
        close(release_pipe[0]);
        close(release_pipe[1]);
        return;
    }

    if (pid == 0) {
        close(child_info_pipe[0]);
        close(release_pipe[1]);

        struct anon_vma_info_args child_info = get_anon_vma_info(addr);
        ASSERT_NEQ(child_info.anon_vma, NULL);
        ASSERT_NEQ(child_info.root, NULL);
        ASSERT_EQ(child_info.root, parent_before.root);
        ASSERT_NEQ(child_info.anon_vma, parent_before.anon_vma);
        ASSERT_EQ(child_info.vma_start, parent_before.vma_start);
        ASSERT_EQ(child_info.vma_end, parent_before.vma_end);

        if (write(child_info_pipe[1], &child_info, sizeof(child_info)) !=
            (ssize_t)sizeof(child_info)) {
            close(child_info_pipe[1]);
            close(release_pipe[0]);
            _exit(EXIT_FAILURE);
        }

        char token;
        if (read(release_pipe[0], &token, sizeof(token)) != sizeof(token)) {
            close(child_info_pipe[1]);
            close(release_pipe[0]);
            _exit(EXIT_FAILURE);
        }

        close(child_info_pipe[1]);
        close(release_pipe[0]);
        _exit(current_test_failed ? EXIT_FAILURE : EXIT_SUCCESS);
    }

    close(child_info_pipe[1]);
    close(release_pipe[0]);

    struct anon_vma_info_args child_info = {0};
    ssize_t bytes_read = read(child_info_pipe[0], &child_info, sizeof(child_info));
    ASSERT_EQ(bytes_read, (ssize_t)sizeof(child_info));
    close(child_info_pipe[0]);

    ASSERT_NEQ(child_info.anon_vma, NULL);
    ASSERT_EQ(child_info.root, parent_before.root);
    ASSERT_NEQ(child_info.anon_vma, parent_before.anon_vma);

    struct anon_vma_info_args parent_after_child = get_anon_vma_info(addr);
    ASSERT_EQ(parent_after_child.anon_vma, parent_before.anon_vma);
    ASSERT_EQ(parent_after_child.root, parent_before.root);

    struct rmap_walk_args rmap = get_rmap_walk_info(addr);
    int saw_parent_vma = 0;
    int saw_child_vma = 0;
    printf("rmap folio=%p nr_vmas=%u total_vmas=%u overflow=%u\n",
           rmap.folio_ptr, rmap.nr_vmas, rmap.total_vmas, rmap.overflow);
    for (unsigned int i = 0; i < rmap.nr_vmas; i++) {
        printf("rmap[%u] vma=%p addr=%lx range=%lx-%lx anon_vma=%p flags=%lx\n",
               i, rmap.vmas[i].vma_ptr, rmap.vmas[i].address,
               rmap.vmas[i].vma_start, rmap.vmas[i].vma_end,
               rmap.vmas[i].anon_vma, rmap.vmas[i].vm_flags);
        saw_parent_vma |= rmap.vmas[i].vma_ptr == parent_before.vma_ptr;
        saw_child_vma |= rmap.vmas[i].vma_ptr == child_info.vma_ptr;
    }
    ASSERT_NEQ(rmap.folio_ptr, NULL);
    ASSERT_ABOVE(rmap.total_vmas, 1);
    ASSERT(saw_parent_vma);
    ASSERT(saw_child_vma);

    char token = 1;
    ASSERT_EQ(write(release_pipe[1], &token, sizeof(token)), (ssize_t)sizeof(token));
    close(release_pipe[1]);

    int status;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);

    printf("parent anon_vma=%p root=%p parent=%p children=%lu active=%lu\n",
           parent_before.anon_vma, parent_before.root, parent_before.parent,
           parent_before.num_children, parent_before.num_active_vmas);
    printf("child anon_vma=%p root=%p parent=%p children=%lu active=%lu\n",
           child_info.anon_vma, child_info.root, child_info.parent,
           child_info.num_children, child_info.num_active_vmas);
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
