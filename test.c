#define _GNU_SOURCE
#include "test_framework.h"
#include "test_helper.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

REGISTER_TEST(test_single_anon_vma);
REGISTER_TEST(test_fork_anon_vma);
REGISTER_TEST(test_count_rmap_vmas);
REGISTER_TEST(test_swap_file_creation);
REGISTER_TEST(test_swap_file_delete_unmap);
REGISTER_TEST(test_swap_file_delete_exit);
REGISTER_TEST(test_zero_file);
REGISTER_TEST(test_read_first_fault);
REGISTER_TEST(test_read_fork_write_fault);
REGISTER_TEST(test_named_swap_fork_ra_file_isolation);
REGISTER_TEST(test_write_fault);
REGISTER_TEST(test_swapout_folio);
REGISTER_TEST(test_named_swap_alias_count_pageout);
REGISTER_TEST(test_file_mmap_pageout_preserves_data);
REGISTER_TEST(test_named_swap_pageout_preserves_data);
REGISTER_TEST(test_memory_pressure_in_disk);
REGISTER_TEST(test_memory_pressure);
REGISTER_TEST(test_named_swap_usage_mmap_munmap);
REGISTER_TEST(test_named_swap_usage_mremap_grow);
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

/*
 * After fork+COW, neighboring PTEs can encode different named-swap files.
 * Sequential read must RA from the faulting PTE's file; fault-around must
 * not install a folio into a PTE that names a different file. Treat that as
 * an RA "miss" for those PTEs (data + file index stay correct).
 */
void test_named_swap_fork_ra_file_isolation(void) {
    const size_t nr_pages = MIN_PAGE_NAMED_SWAP_MMAP;
    const size_t half = nr_pages / 2;
    const size_t len = nr_pages * PAGE_SIZE;
    int child_ready[2] = {-1, -1};
    int parent_go[2] = {-1, -1};
    int child_done[2] = {-1, -1};
    pid_t pid;
    int status = 0;
    unsigned long parent_index = 0;
    unsigned long child_index = 0;
    unsigned long got_child_index = 0;
    size_t i;
    unsigned char *addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP,
                               -1, 0);

    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;

    for (i = 0; i < nr_pages; i++) {
        unsigned char *p = addr + i * PAGE_SIZE;

        p[0] = (unsigned char)(0x10 + (i & 0x2f));
        ASSERT_EQ_AT(p, (unsigned char)(0x10 + (i & 0x2f)));
    }
    parent_index = assert_named_swap_file_for_addr(addr);
    ASSERT_EQ(assert_named_swap_file_for_addr(addr + half * PAGE_SIZE),
              parent_index);

    ASSERT_EQ(pipe(child_ready), 0);
    ASSERT_EQ(pipe(parent_go), 0);
    ASSERT_EQ(pipe(child_done), 0);
    if (child_ready[0] < 0 || parent_go[0] < 0 || child_done[0] < 0) {
        munmap(addr, len);
        return;
    }

    pid = fork();
    ASSERT_NEQ(pid, -1);
    if (pid < 0) {
        close(child_ready[0]);
        close(child_ready[1]);
        close(parent_go[0]);
        close(parent_go[1]);
        close(child_done[0]);
        close(child_done[1]);
        munmap(addr, len);
        return;
    }

    if (pid == 0) {
        close(child_ready[0]);
        close(parent_go[1]);
        close(child_done[0]);

        /* COW second half into a child-owned named-swap file. */
        for (i = half; i < nr_pages; i++) {
            unsigned char *p = addr + i * PAGE_SIZE;

            p[0] = (unsigned char)(0x80 + (i & 0x2f));
            ASSERT_EQ_AT(p, (unsigned char)(0x80 + (i & 0x2f)));
        }
        child_index = assert_named_swap_file_for_addr(addr + half * PAGE_SIZE);
        ASSERT_NEQ(child_index, parent_index);
        ASSERT_EQ(assert_named_swap_file_for_addr(addr), parent_index);

        if (!current_test_failed)
            ASSERT_EQ(write(child_ready[1], &child_index, sizeof(child_index)),
                      (ssize_t)sizeof(child_index));
        close(child_ready[1]);

        ASSERT_EQ(wait_fd(parent_go[0]), 0);
        close(parent_go[0]);

        ASSERT_EQ(madvise(addr, len, MADV_PAGEOUT), 0);

        /* Sequential read across both files' PTE ranges (triggers RA). */
        for (i = 0; i < nr_pages; i++) {
            unsigned char *p = addr + i * PAGE_SIZE;
            unsigned char expect = (i < half)
                    ? (unsigned char)(0x10 + (i & 0x2f))
                    : (unsigned char)(0x80 + (i & 0x2f));
            unsigned long expect_idx = (i < half) ? parent_index : child_index;

            ASSERT_EQ_AT(p, expect);
            ASSERT_EQ(assert_named_swap_file_for_addr(p), expect_idx);
        }

        if (!current_test_failed)
            ASSERT_EQ(write(child_done[1], "d", 1), 1);
        close(child_done[1]);
        _exit(current_test_failed ? EXIT_FAILURE : EXIT_SUCCESS);
    }

    close(child_ready[1]);
    close(parent_go[0]);
    close(child_done[1]);

    ASSERT_EQ(read(child_ready[0], &got_child_index, sizeof(got_child_index)),
              (ssize_t)sizeof(got_child_index));
    close(child_ready[0]);
    ASSERT_NEQ(got_child_index, parent_index);

    /* Parent keeps first-half ownership; page out before child refaults. */
    ASSERT_EQ(madvise(addr, len, MADV_PAGEOUT), 0);
    ASSERT_EQ(signal_fd(parent_go[1]), 0);
    close(parent_go[1]);

    ASSERT_EQ(wait_fd(child_done[0]), 0);
    close(child_done[0]);

    for (i = 0; i < half; i++) {
        unsigned char *p = addr + i * PAGE_SIZE;

        ASSERT_EQ_AT(p, (unsigned char)(0x10 + (i & 0x2f)));
        ASSERT_EQ(assert_named_swap_file_for_addr(p), parent_index);
    }

    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT(WIFEXITED(status));
    if (WIFEXITED(status))
        ASSERT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);

    munmap(addr, len);
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

static void assert_usage_charged_once(unsigned long before_swap,
                                      unsigned long before_fs,
                                      unsigned long after_swap,
                                      unsigned long after_fs,
                                      unsigned long pages)
{
    unsigned long dswap = after_swap - before_swap;
    unsigned long dfs = after_fs - before_fs;

    ASSERT(after_swap >= before_swap);
    ASSERT(after_fs >= before_fs);
    ASSERT_EQ(dswap + dfs, pages);
    ASSERT((dswap == pages && dfs == 0) || (dswap == 0 && dfs == pages));
}

void test_named_swap_usage_mmap_munmap(void)
{
    const unsigned long pages = 4096;
    const size_t len = pages * PAGE_SIZE;
    unsigned long before_swap, before_fs, after_swap, after_fs;
    unsigned long done_swap, done_fs;
    char *addr;

    before_swap = named_swap_swap_usage();
    before_fs = named_swap_fs_usage();
    addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    addr[0] = 1;
    after_swap = named_swap_swap_usage();
    after_fs = named_swap_fs_usage();
    assert_usage_charged_once(before_swap, before_fs, after_swap, after_fs,
                              pages);

    ASSERT_EQ(munmap(addr, len), 0);
    done_swap = named_swap_swap_usage();
    done_fs = named_swap_fs_usage();
    ASSERT_EQ(done_swap + done_fs, before_swap + before_fs);
}

void test_named_swap_usage_mremap_grow(void)
{
    const unsigned long pages = 1024;
    const unsigned long grow = 1024;
    const size_t len = pages * PAGE_SIZE;
    const size_t new_len = (pages + grow) * PAGE_SIZE;
    unsigned long before_swap, before_fs, mid_swap, mid_fs;
    unsigned long after_swap, after_fs, done;
    char *addr;
    char *grown;

    before_swap = named_swap_swap_usage();
    before_fs = named_swap_fs_usage();
    addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    addr[0] = 1;
    mid_swap = named_swap_swap_usage();
    mid_fs = named_swap_fs_usage();
    assert_usage_charged_once(before_swap, before_fs, mid_swap, mid_fs, pages);

    grown = mremap(addr, len, new_len, MREMAP_MAYMOVE);
    ASSERT(grown != MAP_FAILED);
    if (grown == MAP_FAILED) {
        munmap(addr, len);
        return;
    }
    grown[len] = 2;
    after_swap = named_swap_swap_usage();
    after_fs = named_swap_fs_usage();
    assert_usage_charged_once(mid_swap, mid_fs, after_swap, after_fs, grow);

    ASSERT_EQ(munmap(grown, new_len), 0);
    done = named_swap_pool_usage();
    ASSERT_EQ(done, before_swap + before_fs);
}
