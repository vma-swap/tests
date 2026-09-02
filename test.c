#define _GNU_SOURCE
#include "test_framework.h"
#include "test_helper.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MADV_NAMED_SWAP
#define MADV_NAMED_SWAP 26
#endif
#ifndef MADV_NO_NAMED_SWAP
#define MADV_NO_NAMED_SWAP 27
#endif

REGISTER_TEST(test_mremap_enlarge);
REGISTER_TEST(test_munmap_named_swap_deallocate);
REGISTER_TEST(test_mprotect_permissions);
REGISTER_TEST(test_mremap_shrink_from_right);
REGISTER_TEST(test_partial_munmap_shrink_file_right);
REGISTER_TEST(test_mmap_merge_enlarge_file_right);
REGISTER_TEST(test_single_anon_vma);
REGISTER_TEST(test_fork_anon_vma);
REGISTER_TEST(test_count_rmap_vmas);
REGISTER_TEST(test_named_swap_growsdown_stack);
REGISTER_TEST(test_named_swap_growsdown_shrink_back);
REGISTER_TEST(test_named_swap_enlarge_right_shrink);
REGISTER_TEST(test_named_swap_mremap_maymove_grow);
REGISTER_TEST(test_named_swap_fork_pgoff_guard);
REGISTER_TEST(test_named_swap_fork_pgoff_middle);
REGISTER_TEST(test_named_swap_fork_artifact_sparse);
REGISTER_TEST(test_named_swap_fork_artifact_reclaim);
REGISTER_TEST(test_named_swap_fork_artifact_holes);
REGISTER_TEST(test_named_swap_fork_artifact_no_alloc);
REGISTER_TEST(test_named_swap_prot_none_mprotect);
REGISTER_TEST(test_named_swap_prot_none_incremental_mprotect);
REGISTER_TEST(test_named_swap_prot_none_mmap_over);
REGISTER_TEST(test_named_swap_mprotect_middle_punch);
REGISTER_TEST(test_named_swap_mprotect_end_truncate);
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
REGISTER_TEST(test_named_swap_flush_anon);
REGISTER_TEST(test_named_swap_flush_file);
REGISTER_TEST(test_named_swap_flush_background);
REGISTER_TEST(test_named_swap_mglru_types);
REGISTER_TEST(test_named_swap_flush_policy);
REGISTER_TEST(test_named_swap_root_config);
REGISTER_TEST(test_named_swap_convert_roundtrip);
REGISTER_TEST(test_named_swap_convert_einval);
REGISTER_TEST(test_named_swap_convert_subrange);
REGISTER_TEST(test_named_swap_storage);

static int should_check_pressure_rmap(int offset) {
    int page = offset / PAGE_SIZE;

    return offset == 0 ||
           offset + PAGE_SIZE >= VM_MEMORY ||
           page % 256 == 0;
}

static ssize_t read_timeout(int fd, void *buf, size_t n, int seconds)
{
    fd_set rfds;
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    ssize_t got = 0;
    char *p = buf;

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0)
        return -1;
    while ((size_t)got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r <= 0)
            return got ? got : r;
        got += r;
    }
    return got;
}

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

static int read_sysctl_int(const char *path)
{
    FILE *fp;
    int value = -1;

    fp = fopen(path, "r");
    if (!fp)
        return -1;
    if (fscanf(fp, "%d", &value) != 1)
        value = -1;
    fclose(fp);
    return value;
}

static int write_sysctl_int(const char *path, int value)
{
    FILE *fp;

    fp = fopen(path, "w");
    if (!fp)
        return -1;
    if (fprintf(fp, "%d\n", value) < 0) {
        fclose(fp);
        return -1;
    }
    if (fclose(fp))
        return -1;
    return 0;
}

static int read_named_swap_flush(void)
{
    return read_sysctl_int("/proc/sys/vm/named_swap_flush");
}

static int write_named_swap_flush(int mode)
{
    return write_sysctl_int("/proc/sys/vm/named_swap_flush", mode);
}

static unsigned int count_folio_dirty(unsigned char *addr, size_t len)
{
    size_t i;
    unsigned int dirty = 0;

    for (i = 0; i < len; i += PAGE_SIZE) {
        struct folio_info_args info = get_folio_info(addr + i);

        if (info.dirty || info.writeback)
            dirty++;
    }
    return dirty;
}

static unsigned int sample_lru_gen_type(unsigned char *addr)
{
    return get_folio_info(addr).lru_gen_type;
}

static int wait_dirty_drop(unsigned char *addr, size_t len, unsigned int start,
                           int timeout_ms)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
    int waited = 0;
    unsigned int now = start;

    while (waited < timeout_ms) {
        now = count_folio_dirty(addr, len);
        if (now * 2 < start)
            return (int)now;
        nanosleep(&ts, NULL);
        waited += 50;
    }
    return (int)now;
}

static void dirty_pages(unsigned char *addr, size_t len)
{
    size_t i;

    for (i = 0; i < len; i += PAGE_SIZE)
        addr[i] = (unsigned char)(i / PAGE_SIZE);
}

#define FLUSH_CTRL_FILE "/tmp/nswap_flush_ctrl.dat"
#define FLUSH_CTRL_SIZE (512 * 1024)
#define FLUSH_DIRTY_BG_BYTES (256 * 1024)
#define FLUSH_DIRTY_BYTES (512 * 1024)

static void restore_dirty_limits(int bg_bytes, int bytes, int bg_ratio, int ratio)
{
    if (bytes > 0)
        write_sysctl_int("/proc/sys/vm/dirty_bytes", bytes);
    else if (ratio >= 0)
        write_sysctl_int("/proc/sys/vm/dirty_ratio", ratio);
    if (bg_bytes > 0)
        write_sysctl_int("/proc/sys/vm/dirty_background_bytes", bg_bytes);
    else if (bg_ratio >= 0)
        write_sysctl_int("/proc/sys/vm/dirty_background_ratio", bg_ratio);
}

static void drop_caches(void)
{
    write_sysctl_int("/proc/sys/vm/drop_caches", 3);
}

static unsigned char *map_dirty_file(const char *path, size_t len, int *fd_out)
{
    int fd;
    unsigned char *addr;

    *fd_out = -1;
    fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        return MAP_FAILED;
    if (ftruncate(fd, (off_t)len)) {
        close(fd);
        return MAP_FAILED;
    }
    addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        return MAP_FAILED;
    }
    dirty_pages(addr, len);
    *fd_out = fd;
    return addr;
}

struct lru_gen_totals {
    unsigned long anon;
    unsigned long file;
    unsigned long named;
};

static int read_lru_gen_totals(struct lru_gen_totals *out)
{
    FILE *fp;
    char line[256];

    out->anon = out->file = out->named = 0;
    fp = fopen("/sys/kernel/debug/lru_gen", "r");
    if (!fp) {
        if (mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL) &&
            errno != EBUSY && errno != EEXIST)
            return -1;
        fp = fopen("/sys/kernel/debug/lru_gen", "r");
    }
    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp)) {
        char *p;
        unsigned long seq, age, cols[3];
        int n = 0;

        for (p = line; *p; p++) {
            if (*p == 'x' || *p == 'X')
                *p = ' ';
        }
        if (sscanf(line, " %lu %lu %lu %lu %lu", &seq, &age,
                   &cols[0], &cols[1], &cols[2]) == 5) {
            out->anon += cols[0];
            out->file += cols[1];
            out->named += cols[2];
            n = 1;
        }
        (void)n;
    }
    fclose(fp);
    return 0;
}

void test_named_swap_flush_policy(void)
{
    int saved = read_named_swap_flush();
    int fd;
    int mode;

    ASSERT_NEQ(saved, -1);
    ASSERT_EQ(saved, 2);

    ASSERT_EQ(write_named_swap_flush(0), 0);
    ASSERT_EQ(read_named_swap_flush(), 0);
    ASSERT_EQ(write_named_swap_flush(1), 0);
    ASSERT_EQ(read_named_swap_flush(), 1);

    errno = 0;
    fd = open("/proc/sys/vm/named_swap_flush", O_WRONLY | O_CLOEXEC);
    ASSERT_NEQ(fd, -1);
    if (fd >= 0) {
        ASSERT_EQ(write(fd, "3\n", 2), -1);
        ASSERT_EQ(errno, EINVAL);
        close(fd);
    }
    ASSERT_EQ(read_named_swap_flush(), 1);

    ASSERT_EQ(write_named_swap_flush(saved >= 0 ? saved : 2), 0);
    mode = read_named_swap_flush();
    ASSERT_EQ(mode, 2);
}

void test_named_swap_mglru_types(void)
{
    const size_t ns_len = PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP;
    unsigned char *named, *file_map = MAP_FAILED, *anon = MAP_FAILED;
    int fd;
    int memfd = -1;
    struct lru_gen_totals totals;

    named = mmap(NULL, ns_len, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(named != MAP_FAILED);
    if (named == MAP_FAILED)
        return;
    dirty_pages(named, ns_len);
    ASSERT_EQ(sample_lru_gen_type(named), TEST_LRU_GEN_NAMED);
    ASSERT_EQ(get_folio_info(named).is_file, 1);

    fd = open(PAGEOUT_FILE_PATH, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    ASSERT_NEQ(fd, -1);
    if (fd < 0) {
        munmap(named, ns_len);
        return;
    }
    ASSERT_EQ(ftruncate(fd, (off_t)PAGE_SIZE), 0);
    file_map = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT(file_map != MAP_FAILED);
    if (file_map != MAP_FAILED) {
        file_map[0] = 0x5a;
        ASSERT_EQ(sample_lru_gen_type(file_map), TEST_LRU_GEN_FILE);
    }

    memfd = memfd_create("nswap-anon-type", 0);
    ASSERT_NEQ(memfd, -1);
    if (memfd >= 0) {
        ASSERT_EQ(ftruncate(memfd, (off_t)PAGE_SIZE), 0);
        anon = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                    memfd, 0);
        ASSERT(anon != MAP_FAILED);
        if (anon != MAP_FAILED) {
            anon[0] = 0xa5;
            ASSERT_EQ(sample_lru_gen_type(anon), TEST_LRU_GEN_ANON);
            ASSERT_EQ(get_folio_info(anon).is_file, 0);
        }
    }

    ASSERT_EQ(read_lru_gen_totals(&totals), 0);
    ASSERT_ABOVE(totals.named, 0);
    ASSERT_ABOVE(totals.file, 0);
    ASSERT_ABOVE(totals.anon, 0);

    if (anon != MAP_FAILED)
        munmap(anon, PAGE_SIZE);
    if (file_map != MAP_FAILED)
        munmap(file_map, PAGE_SIZE);
    if (memfd >= 0)
        close(memfd);
    close(fd);
    unlink(PAGEOUT_FILE_PATH);
    munmap(named, ns_len);
}

void test_named_swap_flush_anon(void)
{
    const size_t len = PAGEOUT_TEST_SIZE;
    unsigned char *addr, *file_map = MAP_FAILED;
    int file_fd = -1;
    int saved_flush = read_named_swap_flush();
    int saved_wb = read_sysctl_int("/proc/sys/vm/dirty_writeback_centisecs");
    int saved_expire = read_sysctl_int("/proc/sys/vm/dirty_expire_centisecs");
    unsigned int dirty, file_dirty, after_wait, after_reclaim;

    /* Mode 0: kupdate stays on; only named-swap is skipped. */
    ASSERT_EQ(write_named_swap_flush(0), 0);
    ASSERT_EQ(write_sysctl_int("/proc/sys/vm/dirty_writeback_centisecs", 1), 0);
    ASSERT_EQ(write_sysctl_int("/proc/sys/vm/dirty_expire_centisecs", 1), 0);
    drop_caches();

    addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        goto restore;
    dirty_pages(addr, len);
    ASSERT_EQ(sample_lru_gen_type(addr), TEST_LRU_GEN_NAMED);
    dirty = count_folio_dirty(addr, len);
    ASSERT_ABOVE(dirty, len / PAGE_SIZE / 2);

    file_map = map_dirty_file(FLUSH_CTRL_FILE, FLUSH_CTRL_SIZE, &file_fd);
    ASSERT(file_map != MAP_FAILED);
    if (file_map != MAP_FAILED) {
        ASSERT_EQ(sample_lru_gen_type(file_map), TEST_LRU_GEN_FILE);
        file_dirty = count_folio_dirty(file_map, FLUSH_CTRL_SIZE);
        ASSERT_ABOVE(file_dirty, FLUSH_CTRL_SIZE / PAGE_SIZE / 2);
        file_dirty = (unsigned int)wait_dirty_drop(file_map, FLUSH_CTRL_SIZE,
                                                   file_dirty, 8000);
        ASSERT_ABOVE(FLUSH_CTRL_SIZE / PAGE_SIZE / 2, file_dirty);
    }

    after_wait = count_folio_dirty(addr, len);
    ASSERT_ABOVE(after_wait, len / PAGE_SIZE / 2);

    /*
     * Mode 0 skips kupdate/background flushers. Reclaim (MADV_PAGEOUT)
     * and data-integrity sync are the remaining writeback paths.
     */
    ASSERT_EQ(madvise(addr, len, MADV_PAGEOUT), 0);
    {
        struct swap_file_info info = get_swap_file_info(addr);
        int ns_fd;

        ASSERT(info.path[0] != '\0');
        ns_fd = open(info.path, O_RDWR | O_CLOEXEC);
        ASSERT_NEQ(ns_fd, -1);
        if (ns_fd >= 0) {
            ASSERT_EQ(fsync(ns_fd), 0);
            close(ns_fd);
        }
    }
    after_reclaim = (unsigned int)wait_dirty_drop(addr, len, after_wait, 8000);
    ASSERT_ABOVE(after_wait, after_reclaim);

    if (file_map != MAP_FAILED)
        munmap(file_map, FLUSH_CTRL_SIZE);
    if (file_fd >= 0)
        close(file_fd);
    unlink(FLUSH_CTRL_FILE);
    munmap(addr, len);
restore:
    write_named_swap_flush(saved_flush >= 0 ? saved_flush : 2);
    if (saved_wb >= 0)
        write_sysctl_int("/proc/sys/vm/dirty_writeback_centisecs", saved_wb);
    if (saved_expire >= 0)
        write_sysctl_int("/proc/sys/vm/dirty_expire_centisecs", saved_expire);
}

void test_named_swap_flush_background(void)
{
    const size_t len = PAGEOUT_TEST_SIZE;
    unsigned char *addr;
    int saved_flush = read_named_swap_flush();
    int saved_wb = read_sysctl_int("/proc/sys/vm/dirty_writeback_centisecs");
    int saved_expire = read_sysctl_int("/proc/sys/vm/dirty_expire_centisecs");
    unsigned int dirty, after;

    ASSERT_EQ(write_named_swap_flush(1), 0);
    /* Phase 1: kupdate off, default dirty limit — writer must not clean. */
    ASSERT_EQ(write_sysctl_int("/proc/sys/vm/dirty_writeback_centisecs", 0), 0);
    ASSERT_EQ(write_sysctl_int("/proc/sys/vm/dirty_expire_centisecs", 6000), 0);
    drop_caches();

    addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        goto restore;
    dirty_pages(addr, len);
    assert_named_swap_file_for_addr(addr);
    ASSERT_EQ(sample_lru_gen_type(addr), TEST_LRU_GEN_NAMED);
    dirty = count_folio_dirty(addr, len);
    ASSERT_ABOVE(dirty, len / PAGE_SIZE / 2);

    /* Phase 2: kupdate on — background writeback must clean them. */
    ASSERT_EQ(write_sysctl_int("/proc/sys/vm/dirty_writeback_centisecs", 1), 0);
    ASSERT_EQ(write_sysctl_int("/proc/sys/vm/dirty_expire_centisecs", 1), 0);
    after = (unsigned int)wait_dirty_drop(addr, len, dirty, 8000);
    ASSERT_ABOVE(dirty, after);

    munmap(addr, len);
restore:
    write_named_swap_flush(saved_flush >= 0 ? saved_flush : 2);
    if (saved_wb >= 0)
        write_sysctl_int("/proc/sys/vm/dirty_writeback_centisecs", saved_wb);
    if (saved_expire >= 0)
        write_sysctl_int("/proc/sys/vm/dirty_expire_centisecs", saved_expire);
}

void test_named_swap_flush_file(void)
{
    const size_t len = PAGEOUT_TEST_SIZE;
    unsigned char *addr;
    int saved_flush = read_named_swap_flush();
    int saved_wb = read_sysctl_int("/proc/sys/vm/dirty_writeback_centisecs");
    int saved_expire = read_sysctl_int("/proc/sys/vm/dirty_expire_centisecs");
    int saved_dirty_bytes = read_sysctl_int("/proc/sys/vm/dirty_bytes");
    int saved_bg_bytes = read_sysctl_int("/proc/sys/vm/dirty_background_bytes");
    int saved_ratio = read_sysctl_int("/proc/sys/vm/dirty_ratio");
    int saved_bg_ratio = read_sysctl_int("/proc/sys/vm/dirty_background_ratio");
    unsigned int dirty, after;

    ASSERT_EQ(write_named_swap_flush(2), 0);
    /* Kupdate off, dirty limit below the mapping: writer throttle writes. */
    ASSERT_EQ(write_sysctl_int("/proc/sys/vm/dirty_writeback_centisecs", 0), 0);
    ASSERT_EQ(write_sysctl_int("/proc/sys/vm/dirty_expire_centisecs", 6000), 0);
    drop_caches();
    ASSERT_EQ(write_sysctl_int("/proc/sys/vm/dirty_background_bytes",
                               FLUSH_DIRTY_BG_BYTES), 0);
    ASSERT_EQ(write_sysctl_int("/proc/sys/vm/dirty_bytes", FLUSH_DIRTY_BYTES), 0);

    addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        goto restore;
    dirty_pages(addr, len);
    ASSERT_EQ(sample_lru_gen_type(addr), TEST_LRU_GEN_NAMED);
    dirty = count_folio_dirty(addr, len);
    after = dirty;
    if (after * 2 >= len / PAGE_SIZE)
        after = (unsigned int)wait_dirty_drop(addr, len, after, 8000);
    ASSERT_ABOVE(len / PAGE_SIZE / 2, after);

    munmap(addr, len);
restore:
    write_named_swap_flush(saved_flush >= 0 ? saved_flush : 2);
    if (saved_wb >= 0)
        write_sysctl_int("/proc/sys/vm/dirty_writeback_centisecs", saved_wb);
    if (saved_expire >= 0)
        write_sysctl_int("/proc/sys/vm/dirty_expire_centisecs", saved_expire);
    restore_dirty_limits(saved_bg_bytes, saved_dirty_bytes,
                         saved_bg_ratio, saved_ratio);
}

void test_named_swap_root_config(void) {
    char root[PATH_MAX];
    char expected[PATH_MAX];
    char cmdline_root[PATH_MAX];
    char mode[32];
    unsigned char *addr;
    unsigned long index;
    struct swap_file_info info;
    FILE *fp;

    ASSERT_EQ(named_swap_get_root(root, sizeof(root)), 0);
    mode[0] = '\0';
    fp = fopen(NAMED_SWAP_MODE_SYSCTL, "r");
    if (fp) {
        if (fgets(mode, sizeof(mode), fp)) {
            size_t n = strlen(mode);
            while (n > 0 && (mode[n - 1] == '\n' || mode[n - 1] == '\r'))
                mode[--n] = '\0';
        }
        fclose(fp);
    }
    if (!strcmp(mode, "swap") || !strcmp(mode, "hybrid")) {
        if (cmdline_named_swap_root(cmdline_root, sizeof(cmdline_root)))
            ASSERT_EQ(strcmp(root, cmdline_root), 0);
    } else {
        char fs_root[PATH_MAX];

        /*
         * named_swap_root stays /nswap even in fs mode. Files land under
         * named_swap_fs_root (default /.named_swap).
         */
        ASSERT_EQ(named_swap_get_fs_root(fs_root, sizeof(fs_root)), 0);
        ASSERT_EQ(strcmp(fs_root, NAMED_SWAP_DEFAULT_FS_ROOT), 0);
    }

    /* Boot-time anonymous mappings already enabled named-swap. */
    errno = 0;
    ASSERT_EQ(named_swap_set_root(NAMED_SWAP_DEFAULT_ROOT), -1);
    ASSERT_EQ(errno, EBUSY);

    errno = 0;
    ASSERT_EQ(named_swap_set_root("/tmp/named_swap_other"), -1);
    ASSERT_EQ(errno, EBUSY);

    /* Anonymous mmap without MAP_NAMED_SWAP is still named-swap. */
    addr = mmap(NULL, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    addr[0] = 1;
    index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(expected, sizeof(expected), index);
    ASSERT(parse_named_swap_index(expected, &index));
    info = get_swap_file_info(addr);
    ASSERT_EQ(strcmp(info.path, expected), 0);
    ASSERT_EQ(access(expected, F_OK), 0);
    munmap(addr, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP);

    /* Every anonymous VMA is named-swap, including a single page. */
    addr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    addr[0] = 1;
    index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(expected, sizeof(expected), index);
    ASSERT(parse_named_swap_index(expected, &index));
    info = get_swap_file_info(addr);
    ASSERT_EQ(strcmp(info.path, expected), 0);
    ASSERT_EQ(access(expected, F_OK), 0);
    munmap(addr, PAGE_SIZE);
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
    assert_named_swap_file_for_addr(addr);
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
    parent_index = assert_named_swap_pte_file_for_addr(addr);
    ASSERT_EQ(assert_named_swap_pte_file_for_addr(addr + half * PAGE_SIZE),
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
        child_index = assert_named_swap_pte_file_for_addr(addr + half * PAGE_SIZE);
        ASSERT_NEQ(child_index, parent_index);
        ASSERT_EQ(assert_named_swap_pte_file_for_addr(addr), parent_index);

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
            ASSERT_EQ(assert_named_swap_pte_file_for_addr(p), expect_idx);
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
        ASSERT_EQ(assert_named_swap_pte_file_for_addr(p), parent_index);
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

static char *map_fixed_named_swap(void *at, size_t len, int extra_flags)
{
    char *addr = mmap(at, len, PROT_READ | PROT_WRITE,
                      MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS |
                      MAP_NAMED_SWAP | extra_flags, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return MAP_FAILED;
    ASSERT_EQ(addr, at);
    return addr;
}

static char *reserve_hole(size_t len)
{
    char *hole = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    ASSERT(hole != MAP_FAILED);
    if (hole == MAP_FAILED)
        return MAP_FAILED;
    ASSERT_EQ(munmap(hole, len), 0);
    return hole;
}

void test_named_swap_growsdown_stack(void)
{
    const size_t guard = 512 * PAGE_SIZE;
    const size_t init_len = PAGE_SIZE;
    const int grow_pages = 4;
    char *hole;
    char *addr;
    struct vma_info_args vma;
    struct stat st;
    char path[PATH_MAX];
    unsigned long index;
    int i;

    hole = mmap(NULL, guard + init_len, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(hole != MAP_FAILED);
    if (hole == MAP_FAILED)
        return;
    ASSERT_EQ(munmap(hole, guard + init_len), 0);

    addr = mmap(hole + guard, init_len, PROT_READ | PROT_WRITE,
                MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS |
                MAP_GROWSDOWN | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;
    ASSERT_EQ(addr, hole + guard);

    addr[0] = (char)0xA1;
    index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(path, sizeof(path), index);
    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_EQ(st.st_size, (off_t)init_len);

    vma = get_vma_info(addr);
    ASSERT_EQ(vma.vma_start, (unsigned long)addr);
    ASSERT_EQ(vma.vma_end, (unsigned long)addr + init_len);

    for (i = 1; i <= grow_pages; i++)
        addr[-i * (int)PAGE_SIZE] = (char)(0xB0 + i);

    ASSERT_EQ((unsigned char)addr[0], 0xA1);
    for (i = 1; i <= grow_pages; i++)
        ASSERT_EQ((unsigned char)addr[-i * (int)PAGE_SIZE], 0xB0 + i);

    vma = get_vma_info(addr);
    ASSERT_EQ(vma.vma_start,
              (unsigned long)addr - (unsigned long)grow_pages * PAGE_SIZE);
    ASSERT_EQ(vma.vma_end, (unsigned long)addr + init_len);

    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_EQ(st.st_size, (off_t)(init_len + (size_t)grow_pages * PAGE_SIZE));
    ASSERT_EQ(assert_named_swap_file_for_addr(
                  addr - grow_pages * (int)PAGE_SIZE),
              index);

    ASSERT_EQ(munmap((void *)vma.vma_start, vma.vma_end - vma.vma_start), 0);
    ASSERT_EQ(access(path, F_OK), -1);
}

void test_named_swap_growsdown_shrink_back(void)
{
    const size_t guard = 512 * PAGE_SIZE;
    const size_t init_len = PAGE_SIZE;
    const int grow_pages = 4;
    const int shrink_pages = 2;
    char *hole;
    char *addr;
    char *grown_start;
    struct vma_info_args vma;
    struct stat st;
    char path[PATH_MAX];
    unsigned long index;
    int i;

    hole = reserve_hole(guard + init_len);
    if (hole == MAP_FAILED)
        return;

    addr = map_fixed_named_swap(hole + guard, init_len, MAP_GROWSDOWN);
    if (addr == MAP_FAILED)
        return;

    addr[0] = (char)0xA1;
    index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(path, sizeof(path), index);

    for (i = 1; i <= grow_pages; i++)
        addr[-i * (int)PAGE_SIZE] = (char)(0xB0 + i);

    grown_start = addr - grow_pages * (int)PAGE_SIZE;
    ASSERT_EQ(munmap(grown_start, (size_t)shrink_pages * PAGE_SIZE), 0);

    vma = get_vma_info(addr);
    ASSERT_EQ(vma.vma_start,
              (unsigned long)addr -
                  (unsigned long)(grow_pages - shrink_pages) * PAGE_SIZE);
    ASSERT_EQ(vma.vma_end, (unsigned long)addr + init_len);
    ASSERT_EQ((unsigned char)addr[0], 0xA1);
    for (i = 1; i <= grow_pages - shrink_pages; i++)
        ASSERT_EQ((unsigned char)addr[-i * (int)PAGE_SIZE], 0xB0 + i);

    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_EQ(st.st_size,
              (off_t)(init_len +
                      (size_t)(grow_pages - shrink_pages) * PAGE_SIZE));

    addr[-(grow_pages) * (int)PAGE_SIZE] = (char)0xC1;
    vma = get_vma_info(addr);
    ASSERT_EQ(vma.vma_start, (unsigned long)addr - (unsigned long)grow_pages * PAGE_SIZE);
    ASSERT_EQ((unsigned char)addr[0], 0xA1);
    ASSERT_EQ((unsigned char)addr[-(grow_pages) * (int)PAGE_SIZE], 0xC1);

    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_EQ(st.st_size, (off_t)(init_len + (size_t)grow_pages * PAGE_SIZE));

    ASSERT_EQ(munmap((void *)vma.vma_start, vma.vma_end - vma.vma_start), 0);
    ASSERT_EQ(access(path, F_OK), -1);
}

void test_named_swap_enlarge_right_shrink(void)
{
    const size_t init_pages = 4;
    const size_t grow_pages = 4;
    const size_t init_len = init_pages * PAGE_SIZE;
    const size_t grow_len = grow_pages * PAGE_SIZE;
    char *hole;
    char *addr;
    char *extra;
    struct vma_info_args vma;
    struct stat st;
    char path[PATH_MAX];
    unsigned long index;
    size_t i;

    hole = reserve_hole(init_len + grow_len);
    if (hole == MAP_FAILED)
        return;

    addr = map_fixed_named_swap(hole, init_len, 0);
    if (addr == MAP_FAILED)
        return;

    for (i = 0; i < init_pages; i++)
        addr[i * PAGE_SIZE] = (char)(0x40 + i);
    index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(path, sizeof(path), index);
    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_EQ(st.st_size, (off_t)init_len);

    extra = map_fixed_named_swap(addr + init_len, grow_len, 0);
    if (extra == MAP_FAILED) {
        munmap(addr, init_len);
        return;
    }
    for (i = 0; i < grow_pages; i++)
        extra[i * PAGE_SIZE] = (char)(0x80 + i);

    vma = get_vma_info(addr);
    ASSERT_EQ(vma.vma_start, (unsigned long)addr);
    ASSERT_EQ(vma.vma_end, (unsigned long)addr + init_len + grow_len);
    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_EQ(st.st_size, (off_t)(init_len + grow_len));
    for (i = 0; i < init_pages; i++)
        ASSERT_EQ((unsigned char)addr[i * PAGE_SIZE], 0x40 + i);
    for (i = 0; i < grow_pages; i++)
        ASSERT_EQ((unsigned char)extra[i * PAGE_SIZE], 0x80 + i);

    ASSERT_EQ(munmap(extra, grow_len), 0);
    vma = get_vma_info(addr);
    ASSERT_EQ(vma.vma_start, (unsigned long)addr);
    ASSERT_EQ(vma.vma_end, (unsigned long)addr + init_len);
    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_EQ(st.st_size, (off_t)init_len);
    for (i = 0; i < init_pages; i++)
        ASSERT_EQ((unsigned char)addr[i * PAGE_SIZE], 0x40 + i);

    ASSERT_EQ(munmap(addr, init_len), 0);
    ASSERT_EQ(access(path, F_OK), -1);
}

/*
 * glibc ls/malloc and node grow heaps with mremap(MREMAP_MAYMOVE).
 * That is copy_vma + munmap source, not the in-place vma_merge_extend
 * path covered by test_named_swap_enlarge_right_shrink.
 */
void test_named_swap_mremap_maymove_grow(void)
{
    const size_t old_len = 323584; /* glibc ls buffer from the host SIGBUS */
    const size_t new_len = 643072;
    char *p, *q;
    struct stat st;
    char path[PATH_MAX];
    unsigned long index;
    size_t i;

    p = mmap(NULL, old_len, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(p != MAP_FAILED);
    if (p == MAP_FAILED)
        return;

    memset(p, 0xab, old_len);
    index = assert_named_swap_file_for_addr(p);
    named_swap_path_for_index(path, sizeof(path), index);
    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_EQ(st.st_size, (off_t)old_len);

    q = mremap(p, old_len, new_len, MREMAP_MAYMOVE);
    ASSERT(q != MAP_FAILED);
    if (q == MAP_FAILED) {
        munmap(p, old_len);
        return;
    }

    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_EQ(st.st_size, (off_t)new_len);
    for (i = 0; i < old_len; i += 4096)
        ASSERT_EQ((unsigned char)q[i], 0xab);
    q[old_len] = 0xcd;
    q[new_len - 1] = 0xef;
    ASSERT_EQ((unsigned char)q[old_len], 0xcd);
    ASSERT_EQ((unsigned char)q[new_len - 1], 0xef);

    ASSERT_EQ(munmap(q, new_len), 0);
    ASSERT_EQ(access(path, F_OK), -1);
}

static void assert_fork_window_file_size(void *addr, off_t need)
{
    struct swap_file_info info;
    struct stat st;
    char path[PATH_MAX];
    unsigned long index;

    index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(path, sizeof(path), index);
    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_EQ(st.st_size, need);
    info = get_swap_file_info(addr);
    ASSERT_EQ(info.file_size, (unsigned long)need);
}

/*
 * pthread stacks: mmap(stack+guard) PROT_NONE, mprotect the stack at +4K.
 * That VMA has vm_pgoff=1. Fork must size the new files to stack+guard,
 * not stack, or the last page SIGBUS (index == max_idx).
 */
void test_named_swap_fork_pgoff_guard(void)
{
    const size_t guard = PAGE_SIZE;
    const size_t stack = 32 * PAGE_SIZE;
    const size_t total = guard + stack;
    char *p;
    pid_t pid;
    int status = 0;
    unsigned long before_index;
    unsigned long after_index;

    p = mmap(NULL, total, PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(p != MAP_FAILED);
    if (p == MAP_FAILED)
        return;

    ASSERT_EQ(mprotect(p + guard, stack, PROT_READ | PROT_WRITE), 0);
    p[guard] = 0xaa;
    p[total - 1] = 0xbb;
    before_index = assert_named_swap_file_for_addr(p + guard);
    assert_fork_window_file_size(p + guard, (off_t)total);

    pid = fork();
    ASSERT_NEQ(pid, -1);
    if (pid < 0) {
        munmap(p, total);
        return;
    }

    if (pid == 0) {
        ASSERT_EQ((unsigned char)p[guard], 0xaa);
        ASSERT_EQ((unsigned char)p[total - 1], 0xbb);
        p[guard] = 0xcc;
        p[total - 1] = 0xdd;
        after_index = assert_named_swap_file_for_addr(p + guard);
        ASSERT_NEQ(after_index, before_index);
        assert_fork_window_file_size(p + guard, (off_t)total);
        _exit(current_test_failed ? EXIT_FAILURE : EXIT_SUCCESS);
    }

    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT(WIFEXITED(status));
    if (WIFEXITED(status))
        ASSERT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);

    ASSERT_EQ((unsigned char)p[guard], 0xaa);
    ASSERT_EQ((unsigned char)p[total - 1], 0xbb);
    p[guard] = 0xee;
    p[total - 1] = 0xff;
    after_index = assert_named_swap_file_for_addr(p + guard);
    ASSERT_NEQ(after_index, before_index);
    assert_fork_window_file_size(p + guard, (off_t)total);

    ASSERT_EQ(munmap(p, total), 0);
}

/*
 * V8/node isolate: mmap a large PROT_NONE cage, mprotect a window at a
 * high offset, then grow that VMA with adjacent mprotect. Fork must
 * keep i_size at (vm_pgoff << PAGE_SHIFT) + VMA length so the first
 * page (file offset = original window) does not SIGBUS.
 */
void test_named_swap_fork_pgoff_middle(void)
{
    const size_t total_pages = 256;
    const size_t off_pages = 64;
    const size_t first_win_pages = 8;
    const size_t grow_pages = 8;
    const size_t total = total_pages * PAGE_SIZE;
    const size_t offset = off_pages * PAGE_SIZE;
    const size_t win = (first_win_pages + grow_pages) * PAGE_SIZE;
    const off_t need = (off_t)offset + (off_t)win;
    char *p;
    pid_t pid;
    int status = 0;
    unsigned long before_index;
    unsigned long after_index;
    size_t i;

    p = mmap(NULL, total, PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(p != MAP_FAILED);
    if (p == MAP_FAILED)
        return;

    ASSERT_EQ(mprotect(p + offset, first_win_pages * PAGE_SIZE,
                       PROT_READ | PROT_WRITE), 0);
    for (i = 0; i < grow_pages; i++)
        ASSERT_EQ(mprotect(p + offset + (first_win_pages + i) * PAGE_SIZE,
                           PAGE_SIZE, PROT_READ | PROT_WRITE), 0);

    p[offset] = 0x11;
    p[offset + win - 1] = 0x22;
    before_index = assert_named_swap_file_for_addr(p + offset);
    assert_fork_window_file_size(p + offset, (off_t)total);

    pid = fork();
    ASSERT_NEQ(pid, -1);
    if (pid < 0) {
        munmap(p, total);
        return;
    }

    if (pid == 0) {
        ASSERT_EQ((unsigned char)p[offset], 0x11);
        ASSERT_EQ((unsigned char)p[offset + win - 1], 0x22);
        p[offset] = 0x33;
        p[offset + win - 1] = 0x44;
        p[offset + PAGE_SIZE] = 0x55;
        after_index = assert_named_swap_file_for_addr(p + offset);
        ASSERT_NEQ(after_index, before_index);
        assert_fork_window_file_size(p + offset, need);
        _exit(current_test_failed ? EXIT_FAILURE : EXIT_SUCCESS);
    }

    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT(WIFEXITED(status));
    if (WIFEXITED(status))
        ASSERT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);

    ASSERT_EQ((unsigned char)p[offset], 0x11);
    ASSERT_EQ((unsigned char)p[offset + win - 1], 0x22);
    p[offset] = 0x66;
    p[offset + win - 1] = 0x77;
    p[offset + PAGE_SIZE] = 0x88;
    after_index = assert_named_swap_file_for_addr(p + offset);
    ASSERT_NEQ(after_index, before_index);
    assert_fork_window_file_size(p + offset, need);

    ASSERT_EQ(munmap(p, total), 0);
}

static off_t backing_alloc_bytes(const char *path);
static void assert_backing_size(const char *path, off_t size);
static void assert_backing_sparse(const char *path, off_t size);
static void assert_backing_allocated(const char *path, off_t size);

static pid_t fork_and_wait_child(void)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    ASSERT_NEQ(pid, -1);
    if (pid < 0)
        return -1;
    if (pid == 0)
        _exit(0);
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT(WIFEXITED(status));
    if (WIFEXITED(status))
        ASSERT_EQ(WEXITSTATUS(status), 0);
    return pid;
}

/*
 * After fork the orig backing file is an artifact: i_size stays the VMA
 * size, but unused ZERO_RANGE reservations are punched.
 */
void test_named_swap_fork_artifact_sparse(void)
{
    const size_t pages = MIN_PAGE_NAMED_SWAP_MMAP;
    const size_t len = pages * PAGE_SIZE;
    unsigned char *addr;
    char orig_path[PATH_MAX];
    char new_path[PATH_MAX];
    unsigned long orig_index;
    unsigned long new_index;
    size_t i;

    addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;

    addr[0] = 0xa1;
    addr[PAGE_SIZE] = 0xa2;
    orig_index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(orig_path, sizeof(orig_path), orig_index);
    assert_backing_allocated(orig_path, (off_t)len);

    if (fork_and_wait_child() < 0) {
        munmap(addr, len);
        return;
    }

    ASSERT_EQ(addr[0], 0xa1);
    ASSERT_EQ(addr[PAGE_SIZE], 0xa2);
    assert_backing_size(orig_path, (off_t)len);
    assert_backing_sparse(orig_path, (off_t)len);

    addr[len - PAGE_SIZE] = 0xa3;
    new_index = assert_named_swap_file_for_addr(addr + len - PAGE_SIZE);
    ASSERT_NEQ(new_index, orig_index);
    named_swap_path_for_index(new_path, sizeof(new_path), new_index);
    assert_backing_allocated(new_path, (off_t)len);
    for (i = 0; i < 2; i++)
        ASSERT_EQ(addr[i * PAGE_SIZE], (unsigned char)(0xa1 + i));
    ASSERT_EQ(addr[len - PAGE_SIZE], 0xa3);

    munmap(addr, len);
}

/*
 * Pages reclaimed to SWP_NAMED_SWAP before fork must not be punched.
 */
void test_named_swap_fork_artifact_reclaim(void)
{
    const size_t pages = MIN_PAGE_NAMED_SWAP_MMAP;
    const size_t touch = 4;
    const size_t len = pages * PAGE_SIZE;
    unsigned char *addr;
    char orig_path[PATH_MAX];
    unsigned long orig_index;
    off_t orig_alloc;
    size_t i;

    addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;

    for (i = 0; i < touch; i++)
        addr[i * PAGE_SIZE] = (unsigned char)(0xb0 + i);
    orig_index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(orig_path, sizeof(orig_path), orig_index);
    ASSERT_EQ(madvise(addr, touch * PAGE_SIZE, MADV_PAGEOUT), 0);

    if (fork_and_wait_child() < 0) {
        munmap(addr, len);
        return;
    }

    for (i = 0; i < touch; i++)
        ASSERT_EQ(addr[i * PAGE_SIZE], (unsigned char)(0xb0 + i));
    assert_backing_size(orig_path, (off_t)len);
    orig_alloc = backing_alloc_bytes(orig_path);
    ASSERT_ABOVE(orig_alloc + 1, (off_t)touch * PAGE_SIZE / 2);
    assert_backing_sparse(orig_path, (off_t)len);

    munmap(addr, len);
}

void test_named_swap_fork_artifact_holes(void)
{
    const size_t pages = MIN_PAGE_NAMED_SWAP_MMAP;
    const size_t len = pages * PAGE_SIZE;
    unsigned char *addr;
    char orig_path[PATH_MAX];
    unsigned long orig_index;

    addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;

    addr[0] = 0xc1;
    addr[len - PAGE_SIZE] = 0xc2;
    orig_index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(orig_path, sizeof(orig_path), orig_index);

    if (fork_and_wait_child() < 0) {
        munmap(addr, len);
        return;
    }

    ASSERT_EQ(addr[0], 0xc1);
    ASSERT_EQ(addr[len - PAGE_SIZE], 0xc2);
    assert_backing_size(orig_path, (off_t)len);
    assert_backing_sparse(orig_path, (off_t)len);

    munmap(addr, len);
}

void test_named_swap_fork_artifact_no_alloc(void)
{
    const size_t pages = MIN_PAGE_NAMED_SWAP_MMAP;
    const size_t len = pages * PAGE_SIZE;
    unsigned char *addr;
    char orig_path[PATH_MAX];
    unsigned long orig_index;
    unsigned long new_index;
    off_t before;
    off_t after;
    size_t i;

    addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;

    addr[0] = 0xd1;
    orig_index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(orig_path, sizeof(orig_path), orig_index);

    if (fork_and_wait_child() < 0) {
        munmap(addr, len);
        return;
    }

    assert_backing_sparse(orig_path, (off_t)len);
    before = backing_alloc_bytes(orig_path);

    for (i = 1; i < 8; i++)
        addr[i * PAGE_SIZE] = (unsigned char)(0xd1 + i);
    new_index = assert_named_swap_file_for_addr(addr + PAGE_SIZE);
    ASSERT_NEQ(new_index, orig_index);
    after = backing_alloc_bytes(orig_path);
    ASSERT_EQ(after, before);
    ASSERT_EQ(addr[0], 0xd1);

    munmap(addr, len);
}

/*
 * Concurrent mmap/munmap while forking used to abort dup_mmap after
 * map_count++ and trip exit_mmap's BUG_ON. Fork may still return
 * EAGAIN if the prepared VMA list stays stale; the parent must survive.
 */
#define FORK_VMA_RACE_NMAPS 8

static void *fork_vma_race_maps[FORK_VMA_RACE_NMAPS];
static volatile int fork_vma_race_stop;

static void *fork_vma_race_mutator(void *arg)
{
    unsigned n = 0;

    (void)arg;
    while (!fork_vma_race_stop) {
        int j = (int)(n++ % FORK_VMA_RACE_NMAPS);

        if (fork_vma_race_maps[j])
            munmap(fork_vma_race_maps[j], PAGE_SIZE);
        fork_vma_race_maps[j] = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (fork_vma_race_maps[j] != MAP_FAILED)
            *(char *)fork_vma_race_maps[j] = 1;
        else
            fork_vma_race_maps[j] = NULL;
    }
    return NULL;
}

void test_named_swap_fork_vma_race(void)
{
    const int niters = 80;
    pthread_t th;
    int i, forks_ok = 0, forks_fail = 0;
    int status = 0;

    fork_vma_race_stop = 0;
    for (i = 0; i < FORK_VMA_RACE_NMAPS; i++) {
        fork_vma_race_maps[i] = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ASSERT(fork_vma_race_maps[i] != MAP_FAILED);
        if (fork_vma_race_maps[i] == MAP_FAILED)
            return;
        *(char *)fork_vma_race_maps[i] = 1;
    }

    ASSERT_EQ(pthread_create(&th, NULL, fork_vma_race_mutator, NULL), 0);

    for (i = 0; i < niters; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            forks_fail++;
            continue;
        }
        if (pid == 0)
            _exit(0);
        if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0)
            forks_fail++;
        else
            forks_ok++;
    }

    fork_vma_race_stop = 1;
    pthread_join(th, NULL);
    for (i = 0; i < FORK_VMA_RACE_NMAPS; i++) {
        if (fork_vma_race_maps[i])
            munmap(fork_vma_race_maps[i], PAGE_SIZE);
        fork_vma_race_maps[i] = NULL;
    }

    ASSERT(forks_ok > 0);
    (void)forks_fail;
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

static off_t backing_alloc_bytes(const char *path)
{
    struct stat st;

    ASSERT_EQ(stat(path, &st), 0);
    if (stat(path, &st) != 0)
        return -1;
    return (off_t)st.st_blocks * 512;
}

static void assert_backing_size(const char *path, off_t size)
{
    struct stat st;

    ASSERT_EQ(stat(path, &st), 0);
    if (stat(path, &st) != 0)
        return;
    ASSERT_EQ(st.st_size, size);
}

static void assert_backing_sparse(const char *path, off_t size)
{
    off_t alloc;

    assert_backing_size(path, size);
    alloc = backing_alloc_bytes(path);
    ASSERT_ABOVE(size / 4, alloc);
}

static void assert_backing_allocated(const char *path, off_t size)
{
    off_t alloc;

    assert_backing_size(path, size);
    alloc = backing_alloc_bytes(path);
    ASSERT_ABOVE(alloc + 1, size);
}

static sigjmp_buf prot_none_jmp;

static void prot_none_segv_handler(int sig)
{
    siglongjmp(prot_none_jmp, sig);
}

static void assert_addr_sigsegv(volatile char *p)
{
    struct sigaction sa = {0}, old;

    sa.sa_handler = prot_none_segv_handler;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(sigaction(SIGSEGV, &sa, &old), 0);
    if (sigsetjmp(prot_none_jmp, 1) == 0) {
        volatile char c = *p;
        (void)c;
        sigaction(SIGSEGV, &old, NULL);
        ASSERT(0 && "PROT_NONE access did not SIGSEGV");
        return;
    }
    sigaction(SIGSEGV, &old, NULL);
}

void test_named_swap_prot_none_mprotect(void)
{
    const size_t pages = 8;
    const size_t len = pages * PAGE_SIZE;
    char *addr;
    struct swap_file_info info;
    char path[PATH_MAX];
    unsigned long index;
    size_t i;

    addr = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;

    assert_addr_sigsegv(addr);

    index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(path, sizeof(path), index);
    info = get_swap_file_info(addr);
    ASSERT_EQ(info.file_size, (unsigned long)len);
    assert_backing_sparse(path, (off_t)len);

    ASSERT_EQ(mprotect(addr, len, PROT_READ | PROT_WRITE), 0);
    assert_backing_allocated(path, (off_t)len);
    for (i = 0; i < pages; i++)
        addr[i * PAGE_SIZE] = (char)(0x10 + i);
    for (i = 0; i < pages; i++)
        ASSERT_EQ((unsigned char)addr[i * PAGE_SIZE], 0x10 + i);

    munmap(addr, len);
    ASSERT_EQ(access(path, F_OK), -1);
}

/*
 * glibc arenas: mmap PROT_NONE, then mprotect 4K at a time to RW and
 * fault. vma_modify_flags merges with the previous RW VMA. Allocating
 * the whole merged VMA ZERO_RANGEs still-mapped folios and used to
 * oops in unmap_mapping_folio (i_mmap is anon_vma on named-swap).
 */
void test_named_swap_prot_none_incremental_mprotect(void)
{
    const size_t pages = 8;
    const size_t len = pages * PAGE_SIZE;
    char *addr;
    char path[PATH_MAX];
    unsigned long index;
    size_t i;

    addr = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;

    index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(path, sizeof(path), index);
    assert_backing_sparse(path, (off_t)len);

    for (i = 0; i < pages; i++) {
        ASSERT_EQ(mprotect(addr + i * PAGE_SIZE, PAGE_SIZE,
                           PROT_READ | PROT_WRITE), 0);
        addr[i * PAGE_SIZE] = (char)(0x50 + i);
        ASSERT_EQ((unsigned char)addr[0], 0x50);
        ASSERT_EQ((unsigned char)addr[i * PAGE_SIZE], 0x50 + i);
    }

    assert_backing_allocated(path, (off_t)len);
    for (i = 0; i < pages; i++)
        ASSERT_EQ((unsigned char)addr[i * PAGE_SIZE], 0x50 + i);

    munmap(addr, len);
    ASSERT_EQ(access(path, F_OK), -1);
}

void test_named_swap_prot_none_mmap_over(void)
{
    const size_t pages = 8;
    const size_t len = pages * PAGE_SIZE;
    char *addr;
    char *overlay;
    char old_path[PATH_MAX];
    char new_path[PATH_MAX];
    unsigned long old_index;
    unsigned long new_index;
    size_t i;

    addr = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;

    old_index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(old_path, sizeof(old_path), old_index);
    assert_backing_sparse(old_path, (off_t)len);

    overlay = mmap(addr, len, PROT_READ | PROT_WRITE,
                   MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(overlay != MAP_FAILED);
    if (overlay == MAP_FAILED) {
        munmap(addr, len);
        return;
    }
    ASSERT_EQ(overlay, addr);

    for (i = 0; i < pages; i++)
        overlay[i * PAGE_SIZE] = (char)(0x20 + i);
    new_index = assert_named_swap_file_for_addr(overlay);
    named_swap_path_for_index(new_path, sizeof(new_path), new_index);
    ASSERT_NEQ(new_index, old_index);
    assert_backing_allocated(new_path, (off_t)len);
    for (i = 0; i < pages; i++)
        ASSERT_EQ((unsigned char)overlay[i * PAGE_SIZE], 0x20 + i);

    munmap(overlay, len);
    ASSERT_EQ(access(new_path, F_OK), -1);
}

void test_named_swap_mprotect_middle_punch(void)
{
    const size_t pages = 8;
    const size_t hole_pages = 4;
    const size_t hole_off = 2 * PAGE_SIZE;
    const size_t hole_len = hole_pages * PAGE_SIZE;
    const size_t len = pages * PAGE_SIZE;
    char *addr;
    char path[PATH_MAX];
    unsigned long index;
    off_t before;
    off_t after;
    struct vma_info_args left;
    struct vma_info_args hole;
    struct vma_info_args right;
    size_t i;

    addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;

    for (i = 0; i < pages; i++)
        addr[i * PAGE_SIZE] = (char)(0x30 + i);
    index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(path, sizeof(path), index);
    assert_backing_allocated(path, (off_t)len);
    before = backing_alloc_bytes(path);

    ASSERT_EQ(mprotect(addr + hole_off, hole_len, PROT_NONE), 0);

    left = get_vma_info(addr);
    hole = get_vma_info(addr + hole_off);
    right = get_vma_info(addr + hole_off + hole_len);
    ASSERT_EQ(left.vma_start, (unsigned long)addr);
    ASSERT_EQ(left.vma_end, (unsigned long)addr + hole_off);
    ASSERT_EQ(hole.vma_start, (unsigned long)addr + hole_off);
    ASSERT_EQ(hole.vma_end, (unsigned long)addr + hole_off + hole_len);
    ASSERT_EQ(right.vma_start, (unsigned long)addr + hole_off + hole_len);
    ASSERT_EQ(right.vma_end, (unsigned long)addr + len);

    assert_backing_size(path, (off_t)len);
    after = backing_alloc_bytes(path);
    ASSERT_ABOVE(before, after);
    ASSERT_ABOVE(before - after + 1, (off_t)hole_len / 2);

    ASSERT_EQ((unsigned char)addr[0], 0x30);
    ASSERT_EQ((unsigned char)addr[PAGE_SIZE], 0x31);
    ASSERT_EQ((unsigned char)addr[6 * PAGE_SIZE], 0x36);
    ASSERT_EQ((unsigned char)addr[7 * PAGE_SIZE], 0x37);
    assert_addr_sigsegv(addr + hole_off);
    assert_addr_sigsegv(addr + hole_off + PAGE_SIZE);

    munmap(addr, len);
    ASSERT_EQ(access(path, F_OK), -1);
}

void test_named_swap_mprotect_end_truncate(void)
{
    const size_t pages = 8;
    const size_t keep_pages = 4;
    const size_t len = pages * PAGE_SIZE;
    const size_t keep = keep_pages * PAGE_SIZE;
    char *addr;
    char path[PATH_MAX];
    unsigned long index;
    struct vma_info_args prefix;
    struct vma_info_args tail;
    size_t i;

    addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;

    for (i = 0; i < pages; i++)
        addr[i * PAGE_SIZE] = (char)(0x40 + i);
    index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(path, sizeof(path), index);
    assert_backing_allocated(path, (off_t)len);

    ASSERT_EQ(mprotect(addr + keep, len - keep, PROT_NONE), 0);

    prefix = get_vma_info(addr);
    tail = get_vma_info(addr + keep);
    ASSERT_EQ(prefix.vma_start, (unsigned long)addr);
    ASSERT_EQ(prefix.vma_end, (unsigned long)addr + keep);
    ASSERT_EQ(tail.vma_start, (unsigned long)addr + keep);
    ASSERT_EQ(tail.vma_end, (unsigned long)addr + len);
    assert_backing_size(path, (off_t)keep);
    ASSERT_ABOVE((off_t)keep + PAGE_SIZE, backing_alloc_bytes(path));

    for (i = 0; i < keep_pages; i++)
        ASSERT_EQ((unsigned char)addr[i * PAGE_SIZE], 0x40 + i);
    assert_addr_sigsegv(addr + keep);

    munmap(addr, len);
    ASSERT_EQ(access(path, F_OK), -1);
}

static void convert_fill_pattern(unsigned char *addr, size_t pages)
{
    size_t i;

    for (i = 0; i < pages; i++) {
        if (i % 2 == 0)
            memset(addr + i * PAGE_SIZE, (int)(0xA0 + i), PAGE_SIZE);
        else
            ASSERT_EQ(addr[i * PAGE_SIZE], 0);
    }
}

static void convert_check_bytes(unsigned char *addr, const unsigned char *shadow,
                                size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (addr[i] != shadow[i]) {
            ASSERT_EQ(addr[i], shadow[i]);
            return;
        }
    }
}

void test_named_swap_convert_roundtrip(void)
{
    const size_t pages = 8;
    const size_t len = pages * PAGE_SIZE;
    unsigned char *addr;
    unsigned char *shadow;
    struct swap_file_info info;
    char path[PATH_MAX];
    unsigned long index;
    struct folio_info_args folio_info;
    size_t i;

    addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;

    shadow = malloc(len);
    ASSERT(shadow != NULL);
    if (!shadow) {
        munmap(addr, len);
        return;
    }

    convert_fill_pattern(addr, pages);
    memcpy(shadow, addr, len);
    index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(path, sizeof(path), index);
    folio_info = get_folio_info(addr);
    ASSERT_EQ(folio_info.is_file, 1);
    convert_check_bytes(addr, shadow, len);

    errno = 0;
    ASSERT_EQ(madvise(addr, len, MADV_NAMED_SWAP), 0);
    ASSERT_EQ(errno, 0);
    info = get_swap_file_info(addr);
    ASSERT_EQ(strcmp(info.path, path), 0);

    ASSERT_EQ(madvise(addr, len, MADV_NO_NAMED_SWAP), 0);
    info = get_swap_file_info(addr);
    ASSERT_EQ(info.path[0], 0);
    ASSERT_EQ(access(path, F_OK), -1);
    folio_info = get_folio_info(addr);
    ASSERT_EQ(folio_info.is_anon, 1);
    ASSERT_EQ(folio_info.is_file, 0);
    convert_check_bytes(addr, shadow, len);

    addr[PAGE_SIZE + 7] = 0x55;
    shadow[PAGE_SIZE + 7] = 0x55;
    convert_check_bytes(addr, shadow, len);

    ASSERT_EQ(madvise(addr, len, MADV_NAMED_SWAP), 0);
    info = get_swap_file_info(addr);
    ASSERT_NEQ(info.path[0], 0);
    ASSERT_EQ(access(info.path, F_OK), 0);
    folio_info = get_folio_info(addr);
    ASSERT_EQ(folio_info.is_file, 1);
    convert_check_bytes(addr, shadow, len);

    addr[2 * PAGE_SIZE + 3] = 0x66;
    shadow[2 * PAGE_SIZE + 3] = 0x66;
    convert_check_bytes(addr, shadow, len);

    ASSERT_EQ(madvise(addr, len, MADV_NO_NAMED_SWAP), 0);
    info = get_swap_file_info(addr);
    ASSERT_EQ(info.path[0], 0);
    folio_info = get_folio_info(addr);
    ASSERT_EQ(folio_info.is_anon, 1);
    convert_check_bytes(addr, shadow, len);

    ASSERT_EQ(madvise(addr, len, MADV_NAMED_SWAP), 0);
    info = get_swap_file_info(addr);
    ASSERT_NEQ(info.path[0], 0);
    convert_check_bytes(addr, shadow, len);

    ASSERT_EQ(madvise(addr, len, MADV_NO_NAMED_SWAP), 0);
    convert_check_bytes(addr, shadow, len);
    ASSERT_EQ(madvise(addr, len, MADV_NAMED_SWAP), 0);
    convert_check_bytes(addr, shadow, len);

    ASSERT_EQ(madvise(addr, len, MADV_PAGEOUT), 0);
    convert_check_bytes(addr, shadow, len);

    for (i = 0; i < pages; i++)
        ASSERT_EQ(addr[i * PAGE_SIZE], shadow[i * PAGE_SIZE]);

    munmap(addr, len);
    free(shadow);
}

void test_named_swap_convert_einval(void)
{
    const size_t len = PAGE_SIZE;
    unsigned char *addr;
    int fd;

    fd = open(PAGEOUT_FILE_PATH, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    ASSERT_NEQ(fd, -1);
    if (fd < 0)
        return;
    ASSERT_EQ(ftruncate(fd, (off_t)len), 0);

    addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED) {
        close(fd);
        unlink(PAGEOUT_FILE_PATH);
        return;
    }
    addr[0] = 0x11;
    errno = 0;
    ASSERT_EQ(madvise(addr, len, MADV_NAMED_SWAP), -1);
    ASSERT_EQ(errno, EINVAL);
    errno = 0;
    ASSERT_EQ(madvise(addr, len, MADV_NO_NAMED_SWAP), -1);
    ASSERT_EQ(errno, EINVAL);
    munmap(addr, len);

    addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr != MAP_FAILED) {
        addr[0] = 0x22;
        errno = 0;
        ASSERT_EQ(madvise(addr, len, MADV_NAMED_SWAP), -1);
        ASSERT_EQ(errno, EINVAL);
        munmap(addr, len);
    }

    close(fd);
    unlink(PAGEOUT_FILE_PATH);
}

void test_named_swap_convert_subrange(void)
{
    const size_t pages = 8;
    const size_t len = pages * PAGE_SIZE;
    const size_t mid_off = 2 * PAGE_SIZE;
    const size_t mid_len = 2 * PAGE_SIZE;
    unsigned char *addr;
    unsigned char *shadow;
    char left_path[PATH_MAX];
    char right_path[PATH_MAX];
    unsigned long left_index;
    struct swap_file_info mid_info;
    struct vma_info_args left;
    struct vma_info_args mid;
    struct vma_info_args right;
    struct folio_info_args folio_info;
    struct anon_vma_info_args avma_before;
    struct anon_vma_info_args avma_after;

    addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        return;

    shadow = malloc(len);
    ASSERT(shadow != NULL);
    if (!shadow) {
        munmap(addr, len);
        return;
    }

    convert_fill_pattern(addr, pages);
    memcpy(shadow, addr, len);
    left_index = assert_named_swap_file_for_addr(addr);
    named_swap_path_for_index(left_path, sizeof(left_path), left_index);
    avma_before = get_anon_vma_info_from_vma(addr);

    ASSERT_EQ(madvise(addr + mid_off, mid_len, MADV_NO_NAMED_SWAP), 0);

    left = get_vma_info(addr);
    mid = get_vma_info(addr + mid_off);
    right = get_vma_info(addr + mid_off + mid_len);
    ASSERT_EQ(left.vma_start, (unsigned long)addr);
    ASSERT_EQ(left.vma_end, (unsigned long)addr + mid_off);
    ASSERT_EQ(mid.vma_start, (unsigned long)addr + mid_off);
    ASSERT_EQ(mid.vma_end, (unsigned long)addr + mid_off + mid_len);
    ASSERT_EQ(right.vma_start, (unsigned long)addr + mid_off + mid_len);
    ASSERT_EQ(right.vma_end, (unsigned long)addr + len);
    ASSERT_NEQ(left.vma_ptr, mid.vma_ptr);
    ASSERT_NEQ(mid.vma_ptr, right.vma_ptr);

    assert_named_swap_file_for_addr(addr);
    assert_named_swap_file_for_addr(addr + mid_off + mid_len);
    mid_info = get_swap_file_info(addr + mid_off);
    ASSERT_EQ(mid_info.path[0], 0);
    ASSERT_EQ(access(left_path, F_OK), 0);

    folio_info = get_folio_info(addr);
    ASSERT_EQ(folio_info.is_file, 1);
    folio_info = get_folio_info(addr + mid_off);
    ASSERT_EQ(folio_info.is_anon, 1);
    ASSERT_EQ(folio_info.is_file, 0);
    folio_info = get_folio_info(addr + mid_off + mid_len);
    ASSERT_EQ(folio_info.is_file, 1);

    avma_after = get_anon_vma_info_from_vma(addr);
    ASSERT_EQ(avma_before.anon_vma, avma_after.anon_vma);
    avma_after = get_anon_vma_info_from_vma(addr + mid_off);
    ASSERT_EQ(avma_before.anon_vma, avma_after.anon_vma);

    convert_check_bytes(addr, shadow, len);

    ASSERT_EQ(madvise(addr + mid_off, mid_len, MADV_NAMED_SWAP), 0);
    assert_named_swap_file_for_addr(addr + mid_off);
    convert_check_bytes(addr, shadow, len);

    named_swap_path_for_index(right_path, sizeof(right_path),
                              assert_named_swap_file_for_addr(addr + mid_off + mid_len));
    ASSERT_EQ(access(right_path, F_OK), 0);

    munmap(addr, len);
    free(shadow);
}

static unsigned long read_sysctl_ulong(const char *path)
{
    FILE *fp;
    unsigned long value = 0;

    fp = fopen(path, "r");
    if (!fp)
        return 0;
    if (fscanf(fp, "%lu", &value) != 1)
        value = 0;
    fclose(fp);
    return value;
}

void test_named_swap_storage(void)
{
    char mode[32];
    char storage[512];
    unsigned char *addr;
    struct swap_file_info info;
    struct statvfs vfs;
    unsigned long usage, total, hard, avail;
    unsigned long usage_before, usage_after;
    int saved_fs_free, saved_freerun;
    int new_fs_free;
    const size_t big_len = PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP * 8;
    FILE *fp;
    size_t n;

    mode[0] = '\0';
    fp = fopen(NAMED_SWAP_MODE_SYSCTL, "r");
    ASSERT(fp != NULL);
    if (fp) {
        if (fgets(mode, sizeof(mode), fp)) {
            n = strlen(mode);
            while (n > 0 && (mode[n - 1] == '\n' || mode[n - 1] == '\r'))
                mode[--n] = '\0';
        }
        fclose(fp);
    }
    ASSERT(mode[0] != '\0');

    fp = fopen("/proc/sys/vm/named_swap_storage", "r");
    ASSERT(fp != NULL);
    if (fp) {
        n = fread(storage, 1, sizeof(storage) - 1, fp);
        storage[n] = '\0';
        fclose(fp);
        ASSERT(strstr(storage, "mode=") != NULL);
    }

    saved_fs_free = read_sysctl_int("/proc/sys/vm/named_swap_fs_free");
    saved_freerun = read_sysctl_int("/proc/sys/vm/named_swap_freerun");
    ASSERT(saved_fs_free >= 0);
    ASSERT(saved_freerun >= 0);

    if (!strcmp(mode, "fs") || !strcmp(mode, "hybrid")) {
        usage = read_sysctl_ulong("/proc/sys/vm/named_swap_fs_usage");
        total = read_sysctl_ulong("/proc/sys/vm/named_swap_fs_total");
        hard = read_sysctl_ulong("/proc/sys/vm/named_swap_fs_hard");
        avail = read_sysctl_ulong("/proc/sys/vm/named_swap_fs_avail");
        ASSERT(total > 0);
        ASSERT_EQ(hard, total * (100 - (unsigned long)saved_fs_free) / 100);
        ASSERT(avail <= total);
        if (!statvfs("/.named_swap", &vfs) || !statvfs("/", &vfs)) {
            unsigned long df_pages = (unsigned long)
                ((vfs.f_bavail * (unsigned long long)vfs.f_frsize) / PAGE_SIZE);
            /* Live VFS free should be in the same ballpark as the sysctl. */
            ASSERT(df_pages + 4096 >= avail && avail + 4096 >= df_pages);
        }
    }

    if (!strcmp(mode, "swap") || !strcmp(mode, "hybrid")) {
        total = read_sysctl_ulong("/proc/sys/vm/named_swap_swap_total");
        hard = read_sysctl_ulong("/proc/sys/vm/named_swap_swap_hard");
        usage = read_sysctl_ulong("/proc/sys/vm/named_swap_swap_usage");
        ASSERT(total > 0);
        ASSERT_EQ(hard, total);
        ASSERT(usage <= total);
    }

    /* Tighten the FS (or only) hard limit so the next large create is denied. */
    if (!strcmp(mode, "swap")) {
        usage = read_sysctl_ulong("/proc/sys/vm/named_swap_swap_usage");
        hard = read_sysctl_ulong("/proc/sys/vm/named_swap_swap_hard");
    } else {
        usage = read_sysctl_ulong("/proc/sys/vm/named_swap_fs_usage");
        total = read_sysctl_ulong("/proc/sys/vm/named_swap_fs_total");
        if (total > 0) {
            unsigned long target = usage + MIN_PAGE_NAMED_SWAP_MMAP;
            if (target >= total)
                target = total;
            new_fs_free = (int)(100 - (target * 100 / total));
            if (new_fs_free < 0)
                new_fs_free = 0;
            if (new_fs_free > 99)
                new_fs_free = 99;
            ASSERT_EQ(write_sysctl_int("/proc/sys/vm/named_swap_fs_free",
                                       new_fs_free), 0);
        }
        usage = read_sysctl_ulong("/proc/sys/vm/named_swap_fs_usage");
        hard = read_sysctl_ulong("/proc/sys/vm/named_swap_fs_hard");
    }

    ASSERT_EQ(write_sysctl_int("/proc/sys/vm/named_swap_freerun", 0), 0);
    usage_before = !strcmp(mode, "swap") ?
        read_sysctl_ulong("/proc/sys/vm/named_swap_swap_usage") :
        read_sysctl_ulong("/proc/sys/vm/named_swap_fs_usage");

    addr = mmap(NULL, big_len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr == MAP_FAILED)
        goto restore;
    addr[0] = 1;
    info = get_swap_file_info(addr);
    /*
     * Hybrid allocates from the swap pool first. Tightening the FS hard
     * limit must not deny the mapping; overflow-to-FS is checked below.
     */
    if (strcmp(mode, "hybrid") &&
        usage_before + (big_len / PAGE_SIZE) > hard) {
        ASSERT_EQ(info.path[0], 0);
        usage_after = !strcmp(mode, "swap") ?
            read_sysctl_ulong("/proc/sys/vm/named_swap_swap_usage") :
            read_sysctl_ulong("/proc/sys/vm/named_swap_fs_usage");
        ASSERT(usage_after < usage_before + (big_len / PAGE_SIZE));
    }
    munmap(addr, big_len);

    ASSERT_EQ(write_sysctl_int("/proc/sys/vm/named_swap_freerun", 1), 0);
    addr = mmap(NULL, big_len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(addr != MAP_FAILED);
    if (addr != MAP_FAILED) {
        addr[0] = 1;
        assert_named_swap_file_for_addr(addr);
        info = get_swap_file_info(addr);
        if (!strcmp(mode, "swap"))
            ASSERT_EQ(strncmp(info.path, "/nswap/", 7), 0);
        else if (!strcmp(mode, "fs"))
            ASSERT_EQ(strncmp(info.path, NAMED_SWAP_DEFAULT_FS_ROOT,
                              strlen(NAMED_SWAP_DEFAULT_FS_ROOT)), 0);
        munmap(addr, big_len);
    }

    if (!strcmp(mode, "hybrid")) {
        unsigned long swap_hard = read_sysctl_ulong("/proc/sys/vm/named_swap_swap_hard");
        unsigned long swap_usage = read_sysctl_ulong("/proc/sys/vm/named_swap_swap_usage");
        unsigned long remain = swap_hard > swap_usage ? swap_hard - swap_usage : 0;

        write_sysctl_int("/proc/sys/vm/named_swap_freerun", 0);
        write_sysctl_int("/proc/sys/vm/named_swap_fs_free", saved_fs_free);
        if (remain && remain <= 4096) {
            size_t fill = remain * PAGE_SIZE;
            unsigned char *fill_addr = mmap(NULL, fill, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (fill_addr != MAP_FAILED)
                fill_addr[0] = 1;
            addr = mmap(NULL, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP,
                        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                        -1, 0);
            ASSERT(addr != MAP_FAILED);
            if (addr != MAP_FAILED) {
                addr[0] = 1;
                info = get_swap_file_info(addr);
                ASSERT(info.path[0] != 0);
                ASSERT_EQ(strncmp(info.path, NAMED_SWAP_DEFAULT_FS_ROOT,
                                  strlen(NAMED_SWAP_DEFAULT_FS_ROOT)), 0);
                munmap(addr, PAGE_SIZE * MIN_PAGE_NAMED_SWAP_MMAP);
            }
            if (fill_addr != MAP_FAILED)
                munmap(fill_addr, fill);
        }
    }

restore:
    write_sysctl_int("/proc/sys/vm/named_swap_freerun", saved_freerun);
    write_sysctl_int("/proc/sys/vm/named_swap_fs_free", saved_fs_free);
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
