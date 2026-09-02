#define _GNU_SOURCE
#include "test_framework.h"
#include "test_helper.h"

#include <errno.h>
#include <limits.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* PAGE_SIZE, MAP_NAMED_SWAP, and MIN_PAGE_NAMED_SWAP_MMAP are defined in test_helper.h. */

REGISTER_TEST(test_mremap_enlarge);
REGISTER_TEST(test_munmap_named_swap_deallocate);
REGISTER_TEST(test_mprotect_permissions);
REGISTER_TEST(test_mremap_shrink_from_right);
REGISTER_TEST(test_partial_munmap_shrink_file_right);
REGISTER_TEST(test_mmap_merge_enlarge_file_right);
REGISTER_TEST(test_single_anon_vma);
REGISTER_TEST(test_fork_anon_vma);
REGISTER_TEST(test_count_rmap_vmas);
REGISTER_TEST(test_swap_file_creation);
REGISTER_TEST(test_swap_file_delete_unmap);
REGISTER_TEST(test_swap_file_delete_exit);
REGISTER_TEST(test_zero_file);
REGISTER_TEST(test_read_first_fault);
REGISTER_TEST(test_read_fork_write_fault);
REGISTER_TEST(test_write_fault);
REGISTER_TEST(test_swapout_folio);
REGISTER_TEST(test_named_swap_alias_count_pageout);
REGISTER_TEST(test_file_mmap_pageout_preserves_data);
REGISTER_TEST(test_named_swap_pageout_preserves_data);
REGISTER_TEST(test_memory_pressure_in_disk);
REGISTER_TEST(test_memory_pressure);
/* Registered last so it runs first (list is prepended). */
REGISTER_TEST(test_named_swap_root_config);

static int should_check_pressure_rmap(int offset) {
    int page = offset / PAGE_SIZE;

    return offset == 0 ||
           offset + PAGE_SIZE >= VM_MEMORY ||
           page % 256 == 0;
}

/*
 * Must run before any other named-swap mapping so the root can still be
 * changed. Covers cmdline echo, invalid paths, missing parent, mount-before-
 * enable (ext4 loop — tmpfs lacks FALLOC_FL_ZERO_RANGE), custom root + EBUSY,
 * and remount-after-enable (old mapping stays valid).
 */
static int cmdline_named_swap_root(char *out, size_t out_len)
{
    FILE *fp;
    char line[4096];
    const char *key = "named_swap.root=";
    char *p, *end;

    fp = fopen("/proc/cmdline", "r");
    if (!fp)
        return 0;
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    p = strstr(line, key);
    if (!p)
        return 0;
    p += strlen(key);
    end = p;
    while (*end && *end != ' ' && *end != '\n' && *end != '\r')
        end++;
    if (end == p || (size_t)(end - p) >= out_len)
        return 0;
    memcpy(out, p, end - p);
    out[end - p] = '\0';
    return 1;
}

static int setup_ext4_loop(const char *img, const char *mnt, int size_mb,
                           int unmount_first)
{
    char cmd[512];

    unlink(img);
    if (unmount_first)
        umount(mnt); /* ignore errors */
    snprintf(cmd, sizeof(cmd),
             "mkdir -p '%s' && "
             "dd if=/dev/zero of='%s' bs=1M count=%d status=none && "
             "mkfs.ext4 -F -q '%s' && "
             "mount -o loop '%s' '%s'",
             mnt, img, size_mb, img, img, mnt);
    return system(cmd);
}

static void cleanup_ext4_loop(const char *img, const char *mnt)
{
    sync();
    /* Lazy unmount: named-swap may still hold lower-file refs briefly. */
    umount2(mnt, MNT_DETACH);
    umount2(mnt, MNT_DETACH);
    unlink(img);
}

void test_named_swap_root_config(void) {
    const char *mnt = "/tmp/named_swap_mnt";
    const char *img = "/tmp/named_swap_mnt.img";
    const char *img2 = "/tmp/named_swap_overlay.img";
    const char *missing_parent = "/tmp/no_such_named_swap_parent/ns";
    char root[PATH_MAX];
    char expected[PATH_MAX];
    char cmdline_root[PATH_MAX];
    char long_path[512];
    unsigned char *addr;
    unsigned char *addr2;
    unsigned long index;
    struct swap_file_info info;
    struct anon_vma_info_args anon;
    size_t i;

    /* If booted with named_swap.root=, sysctl must match before we change it. */
    if (cmdline_named_swap_root(cmdline_root, sizeof(cmdline_root))) {
        ASSERT_EQ(named_swap_get_root(root, sizeof(root)), 0);
        ASSERT_EQ(strcmp(root, cmdline_root), 0);
    }

    /* Invalid paths are rejected with EINVAL while still disabled. */
    errno = 0;
    ASSERT_EQ(named_swap_set_root("relative/path"), -1);
    ASSERT_EQ(errno, EINVAL);

    errno = 0;
    ASSERT_EQ(named_swap_set_root("/tmp/named_swap_trail/"), -1);
    ASSERT_EQ(errno, EINVAL);

    memset(long_path, 'a', sizeof(long_path));
    long_path[0] = '/';
    long_path[sizeof(long_path) - 1] = '\0';
    errno = 0;
    ASSERT_EQ(named_swap_set_root(long_path), -1);
    ASSERT_EQ(errno, EINVAL);

    /* Parent directory missing: enable fails, mapping falls back to anon. */
    ASSERT_EQ(named_swap_set_root(missing_parent), 0);
    addr = mmap(NULL, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    addr[0] = 1;
    anon = get_anon_vma_info(addr);
    ASSERT_EQ(anon.named_swap_file == NULL, 1);
    info = get_swap_file_info(addr);
    ASSERT_EQ(info.path[0] == '\0', 1);
    munmap(addr, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP);

    /* Root is still changeable after a failed enable. Mount a real FS. */
    ASSERT_EQ(setup_ext4_loop(img, mnt, 32, 1), 0);
    ASSERT_EQ(named_swap_set_root(mnt), 0);
    ASSERT_EQ(named_swap_get_root(root, sizeof(root)), 0);
    ASSERT_EQ(strcmp(root, mnt), 0);

    addr = mmap(NULL, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED) {
        cleanup_ext4_loop(img, mnt);
        return;
    }

    for (i = 0; i < MIN_PAGE_NAMED_SWAP_MMAP; i++)
        addr[i * PAGE_SIZE] = (unsigned char)(0x40 + i);

    index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(expected, sizeof(expected), index);
    ASSERT_EQ(strncmp(expected, mnt, strlen(mnt)), 0);
    info = get_swap_file_info(addr);
    ASSERT_EQ(strcmp(info.path, expected), 0);
    ASSERT_EQ(access(expected, F_OK), 0);

    errno = 0;
    ASSERT_EQ(named_swap_set_root(NAMED_SWAP_DEFAULT_ROOT), -1);
    ASSERT_EQ(errno, EBUSY);

    /*
     * Remount-over after enable must not corrupt the already-open mapping.
     * New creates may land on the overlay; we only require old data survives.
     */
    ASSERT_EQ(setup_ext4_loop(img2, mnt, 16, 0), 0);
    for (i = 0; i < MIN_PAGE_NAMED_SWAP_MMAP; i++)
        ASSERT_EQ_AT(addr + i * PAGE_SIZE, (unsigned char)(0x40 + i));

    addr2 = mmap(NULL, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr2 != MAP_FAILED);
    if (addr2 != MAP_FAILED) {
        addr2[0] = 0xcd;
        ASSERT_EQ_AT(addr2, 0xcd);
        munmap(addr2, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP);
    }

    munmap(addr, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP);
    cleanup_ext4_loop(img2, mnt);
    cleanup_ext4_loop(img, mnt);
}

void test_file_mmap_pageout_preserves_data(void) {
    size_t len = PAGEOUT_TEST_SIZE;
    int fd = open(PAGEOUT_FILE_PATH,
                  O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    unsigned char *addr;
    int fallocate_ret;

    ASSERT_NEQ(fd, -1);
    if (fd < 0)
        return;

    fallocate_ret = posix_fallocate(fd, 0, (off_t)len);
    if (fallocate_ret) {
        errno = fallocate_ret;
        perror("posix_fallocate pageout test file");
        ASSERT_EQ(ftruncate(fd, (off_t)len), 0);
    }

    addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr != MAP_FAILED) {
        assert_pageout_preserves_data(addr, len);
        munmap(addr, len);
    }

    close(fd);
    unlink(PAGEOUT_FILE_PATH);
}

void test_named_swap_pageout_preserves_data(void) {
    size_t len = PAGEOUT_TEST_SIZE;
    unsigned char *addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP,
                               -1, 0);

    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;

    assert_pageout_preserves_data(addr, len);
    munmap(addr, len);
}

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
                if (should_check_pressure_rmap(i))
                    ASSERT_EQ(count_rmap_vmas(addr + i), 1);
            }
            addr[i]++;
            ASSERT_EQ_AT(addr + i, i%256 + (it + 1));
            if (should_check_pressure_rmap(i))
                ASSERT_EQ(count_rmap_vmas(addr + i), 1);
        }
    }

    munmap(addr, VM_MEMORY);
}
void test_memory_pressure_in_disk(void) {
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
                ASSERT_EQ(count_rmap_vmas(addr + i), 1);
            }
            addr[i]++;
            ASSERT_EQ_AT(addr + i, i%256 + (it + 1));
            ASSERT_EQ(count_rmap_vmas(addr + i), 1);
        }
    }
    struct swap_file_info swap_file_info = get_swap_file_info(addr);
    int fd = open(swap_file_info.path, O_RDONLY | O_DIRECT);
    unsigned char *disk_page = NULL;
    ASSERT_NEQ(fd, -1);
    ASSERT_EQ(posix_memalign((void **)&disk_page, PAGE_SIZE, PAGE_SIZE), 0);
    ASSERT(disk_page != NULL);
    if (fd >= 0)
        ASSERT_EQ(syncfs(fd), 0);
    for (int i = 0; i < VM_MEMORY; i+=PAGE_SIZE) {
        unsigned char expected = (unsigned char)(i%256 + 2);

        ASSERT_EQ(pread(fd, disk_page, PAGE_SIZE, i), PAGE_SIZE);
        ASSERT_EQ_AT(disk_page, expected);
    }
    if (fd >= 0)
        close(fd);
    free(disk_page);

    munmap(addr, VM_MEMORY);
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
        ASSERT_EQ(count_rmap_vmas(addr + i), 1);
        madvise(addr, PAGE_SIZE * 2, MADV_PAGEOUT);
        unsigned long pte_value = get_pte_value(addr + i);
        printf("pte_value: %lx\n", pte_value);
        ASSERT_EQ(get_named_swap_alias_count(addr + i), 1);
        addr[i]++;
        ASSERT_EQ_AT(addr + i, i%256 + 1);
        ASSERT_EQ(count_rmap_vmas(addr + i), 1);
        ASSERT_EQ(get_named_swap_alias_count(addr + i), 1);
    }

    munmap(addr, PAGE_SIZE * 2);
}

void test_named_swap_alias_count_pageout(void) {
    unsigned char *addr = mmap(NULL, PAGE_SIZE * 2, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;

    addr[0] = 7;
    ASSERT_EQ(count_rmap_vmas(addr), 1);
    ASSERT_EQ(get_named_swap_alias_count(addr), 1);
    ASSERT_EQ(madvise(addr, PAGE_SIZE, MADV_PAGEOUT), 0);
    ASSERT_EQ(get_named_swap_alias_count(addr), 1);

    addr[0]++;
    ASSERT_EQ_AT(addr, 8);
    ASSERT_EQ(count_rmap_vmas(addr), 1);
    ASSERT_EQ(get_named_swap_alias_count(addr), 1);

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
            struct anon_vma_info_args vma_info =
                get_anon_vma_info_from_vma(addr + i);

            ASSERT(vma_info.anon_vma != NULL);
            ASSERT(vma_info.named_swap_file != NULL);
            ASSERT_EQ(get_folio_mapcount(addr + i), 0);
        }
    }
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
            struct anon_vma_info_args vma_info =
                get_anon_vma_info_from_vma(addr + i);

            ASSERT(vma_info.anon_vma != NULL);
            ASSERT(vma_info.named_swap_file != NULL);
            ASSERT_EQ(get_folio_mapcount(addr + i), 0);
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

void test_read_fork_write_fault(void) {
    int child_ready[2] = {-1, -1};
    int parent_done[2] = {-1, -1};
    pid_t pid;
    int status = 0;
    unsigned long child_index = 0;
    unsigned long parent_index = 0;
    unsigned char *addr = mmap(NULL, PAGE_SIZE * 2, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);

    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;

    ASSERT_EQ_AT(addr, 0);
    ASSERT_EQ_AT(addr + PAGE_SIZE, 0);
    ASSERT_EQ(get_folio_mapcount(addr), 0);
    ASSERT_EQ(get_folio_mapcount(addr + PAGE_SIZE), 0);

    ASSERT_EQ(pipe(child_ready), 0);
    ASSERT_EQ(pipe(parent_done), 0);
    if (child_ready[0] < 0 || child_ready[1] < 0 ||
        parent_done[0] < 0 || parent_done[1] < 0) {
        munmap(addr, PAGE_SIZE * 2);
        return;
    }

    pid = fork();
    ASSERT_NEQ(pid, -1);
    if (pid < 0) {
        close(child_ready[0]);
        close(child_ready[1]);
        close(parent_done[0]);
        close(parent_done[1]);
        munmap(addr, PAGE_SIZE * 2);
        return;
    }

    if (pid == 0) {
        close(child_ready[0]);
        close(parent_done[1]);

        ASSERT_EQ_AT(addr, 0);
        ASSERT_EQ_AT(addr + PAGE_SIZE, 0);
        ASSERT_EQ(get_folio_mapcount(addr), 0);

        addr[0] = 0x5a;
        ASSERT_EQ_AT(addr, 0x5a);
        ASSERT_EQ(get_folio_mapcount(addr), 1);
        ASSERT_EQ_ANON_VMA(ANON_VMA_VMA, addr, ANON_VMA_FOLIO, addr);
        child_index = assert_named_swap_file_for_addr(addr);

        if (!current_test_failed)
            ASSERT_EQ(write(child_ready[1], &child_index,
                            sizeof(child_index)),
                      (ssize_t)sizeof(child_index));
        close(child_ready[1]);

        ASSERT_EQ(wait_fd(parent_done[0]), 0);
        close(parent_done[0]);
        ASSERT_EQ_AT(addr, 0x5a);
        ASSERT_EQ_AT(addr + PAGE_SIZE, 0);
        _exit(current_test_failed ? EXIT_FAILURE : EXIT_SUCCESS);
    }

    close(child_ready[1]);
    close(parent_done[0]);

    ASSERT_EQ(read(child_ready[0], &child_index, sizeof(child_index)),
              (ssize_t)sizeof(child_index));
    close(child_ready[0]);

    ASSERT_EQ_AT(addr, 0);
    ASSERT_EQ_AT(addr + PAGE_SIZE, 0);
    ASSERT_EQ(get_folio_mapcount(addr + PAGE_SIZE), 0);

    addr[PAGE_SIZE] = 0xa5;
    ASSERT_EQ_AT(addr + PAGE_SIZE, 0xa5);
    ASSERT_EQ(get_folio_mapcount(addr + PAGE_SIZE), 1);
    ASSERT_EQ_ANON_VMA(ANON_VMA_VMA, addr + PAGE_SIZE,
                       ANON_VMA_FOLIO, addr + PAGE_SIZE);
    parent_index = assert_named_swap_file_for_addr(addr + PAGE_SIZE);
    ASSERT_NEQ(parent_index, child_index);

    ASSERT_EQ(signal_fd(parent_done[1]), 0);
    close(parent_done[1]);

    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT(WIFEXITED(status));
    if (WIFEXITED(status))
        ASSERT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);

    munmap(addr, PAGE_SIZE * 2);
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
    munmap(addr, PAGE_SIZE * 10);
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
        parent_done[0] < 0 || parent_done[1] < 0) {
        munmap(addr, PAGE_SIZE * 4);
        return;
    }

    pid = fork();
    ASSERT_NEQ(pid, -1);
    if (pid < 0) {
        close(child_ready[0]);
        close(child_ready[1]);
        close(parent_done[0]);
        close(parent_done[1]);
        munmap(addr, PAGE_SIZE * 4);
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
        munmap(addr, PAGE_SIZE * 4);
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
        parent_done[0] < 0 || parent_done[1] < 0) {
        munmap(addr, PAGE_SIZE * 4);
        return;
    }

    pid = fork();
    ASSERT_NEQ(pid, -1);
    if (pid < 0) {
        close(child_ready[0]);
        close(child_ready[1]);
        close(parent_done[0]);
        close(parent_done[1]);
        munmap(addr, PAGE_SIZE * 4);
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
        munmap(addr, PAGE_SIZE * 4);
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
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP | MAP_FIXED, -1, 0);
    
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

    // 3. Verify the initial backing file size and capture the path
    struct swap_file_info initial_info = get_swap_file_info(addr);
    ASSERT_EQ(initial_info.file_size, initial_size);
    
    char initial_path[PATH_MAX];
    strncpy(initial_path, initial_info.path, PATH_MAX);

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

   // 6. Verify the backing file was enlarged by the kernel AND remains the exact same file
    struct swap_file_info expanded_info = get_swap_file_info(new_addr);
    ASSERT_EQ(expanded_info.file_size, expanded_size);
    ASSERT_EQ(strcmp(initial_path, expanded_info.path), 0);

    munmap(new_addr, expanded_size); 
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

    /* Save the original path to verify it survives the split */
    char original_path[PATH_MAX];
    strncpy(original_path, before.path, PATH_MAX);

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

    /* 7. Verify both remaining VMAs point to the EXACT same original file */
    struct swap_file_info left_after = get_swap_file_info(addr);
    struct swap_file_info right_after = get_swap_file_info(addr + PAGE_SIZE * 2);
    
    ASSERT_EQ(strcmp(left_after.path, original_path), 0);
    ASSERT_EQ(strcmp(right_after.path, original_path), 0);

    /* 8. Verify the apparent backing file size remains unchanged due to KEEP_SIZE */
    ASSERT_EQ(left_after.file_size, len);
    ASSERT_EQ(right_after.file_size, len);

    /* 
     * 9. Verify the physical blocks decreased by exactly the unmapped size.
     * i_blocks are measured in 512-byte sectors.
     * We unmapped exactly 1 PAGE_SIZE (4096 bytes).
     * 4096 / 512 = 8 blocks should have been freed.
     */
    unsigned long after_blocks = left_after.allocated_blocks;
    unsigned long blocks_freed = PAGE_SIZE / 512;
    
    ASSERT_EQ(initial_blocks, after_blocks + blocks_freed);
    
    /* 10. Verify accessing the unmapped middle page causes a segfault */
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

    /* 8. Ensure the first page is still physically writable */
    addr[0] = 0xDD;
    ASSERT_EQ_AT(addr, 0xDD);

    /* 9. Validate the hardware/page-fault level actually triggers a segfault on write */
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

    /* Save the original file path before shrinking */
    char initial_path[PATH_MAX];
    strncpy(initial_path, file_before.path, PATH_MAX);

    /* 4. Shrink the VMA from the right using mremap */
    void *res = mremap(addr, initial_size, shrunk_size, 0);
    
    /* Ensure the syscall succeeded and kept the same base address */
    ASSERT(res != MAP_FAILED);
    ASSERT_EQ(res, addr);

    /* 5. Inspect the underlying file to verify VFS truncate occurred */
    struct swap_file_info file_after = get_swap_file_info(addr);
    
    /* PROOF: If vfs_truncate was called, the overall file size will decrease. 
              If named_swap_deallocate (hole punch) was called, the size would remain 2 pages. */
    ASSERT_EQ(file_after.file_size, shrunk_size);

    /* Assert the file path remained exactly the same after the shrink */
    ASSERT_EQ(strcmp(initial_path, file_after.path), 0);

    /* 6. Verify accessing the truncated memory area causes a segfault */
    ASSERT_SIGNAL(SIGSEGV) {
        addr[page_size] = 0x33;
    }

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

    /* Save the original file path before shrinking */
    char initial_path[PATH_MAX];
    strncpy(initial_path, file_before.path, PATH_MAX);

    /* 4. Shrink the VMA from the right by unmapping the second page */
    int rc = munmap(addr + page_size, page_size);
    ASSERT_EQ(rc, 0);

    /* 5. Inspect the underlying file of the remaining VMA to verify VFS truncate occurred */
    struct swap_file_info file_after = get_swap_file_info(addr);
    
    /* PROOF: If vfs_truncate was called by the vma.c cleanup logic, the overall file size will decrease. 
              If named_swap_deallocate (hole punch) was called, the size would remain 2 pages. */
    ASSERT_EQ(file_after.file_size, shrunk_size);

    /* Assert the file path remained exactly the same after the partial munmap */
    ASSERT_EQ(strcmp(initial_path, file_after.path), 0);

    /* Verify accessing the unmapped memory area causes a segfault */
    ASSERT_SIGNAL(SIGSEGV) {
        addr[page_size] = 0x33;
    }

    /* 6. Cleanup the remaining 1-page VMA */
    munmap(addr, shrunk_size);
}

void test_mmap_merge_enlarge_file_right(void) {
    size_t page_size = PAGE_SIZE;

    /* 1. Create a 2-page gap to reserve contiguous virtual memory */
    unsigned char *gap = mmap(NULL, page_size * 2, PROT_NONE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(gap != MAP_FAILED);
    if (gap == MAP_FAILED) return;
    
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

    /* Save the original file path before the merge */
    char initial_path[PATH_MAX];
    strncpy(initial_path, left_file_before.path, PATH_MAX);

    /* Capture the original anon_vma structure of the left VMA */
    struct anon_vma_info_args anon_before = get_anon_vma_info_from_vma(left_addr);

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

    /* Assert the file path of the merged VMA is identical to the original left VMA's file */
    ASSERT_EQ(strcmp(initial_path, merged_file.path), 0);

    /* Assert the merged VMA is still using the original left VMA's anon_vma */
    struct anon_vma_info_args anon_after = get_anon_vma_info_from_vma(left_addr);
    ASSERT_EQ(anon_after.anon_vma, anon_before.anon_vma);

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

