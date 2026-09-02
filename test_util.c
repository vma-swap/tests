#include "test_util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int signal_fd(int fd) {
    char byte = 1;
    ssize_t ret;

    do {
        ret = write(fd, &byte, sizeof(byte));
    } while (ret < 0 && errno == EINTR);

    return ret == sizeof(byte) ? 0 : -1;
}

int wait_fd(int fd) {
    char byte;
    ssize_t ret;

    do {
        ret = read(fd, &byte, sizeof(byte));
    } while (ret < 0 && errno == EINTR);

    return ret == sizeof(byte) ? 0 : -1;
}

static int open_swapctl(void) {
    int fd = open(DEVICE, O_RDONLY);
    if (fd < 0)
        perror("open " DEVICE);
    return fd;
}

struct vma_info_args get_vma_info(void *addr) {
    struct vma_info_args args = {0};
    args.virtual_address = addr;

    int fd = open_swapctl();
    if (fd < 0)
        return args;

    if (ioctl(fd, IOCTL_VMA_INFO, &args) < 0)
        perror("Failed to get VMA info");
    close(fd);
    return args;
}

struct anon_vma_info_args get_anon_vma_info(void *addr) {
    struct anon_vma_info_args args = {0};
    args.virtual_address = addr;

    int fd = open_swapctl();
    if (fd < 0)
        return args;

    if (ioctl(fd, IOCTL_ANON_VMA_INFO, &args) < 0)
        perror("Failed to get anon_vma info");
    close(fd);
    return args;
}

struct anon_vma_info_args get_anon_vma_info_from_vma(void *addr) {
    struct anon_vma_info_args args = {0};
    args.virtual_address = addr;

    int fd = open_swapctl();
    if (fd < 0)
        return args;

    if (ioctl(fd, IOCTL_ANON_VMA_INFO_FROM_VMA, &args) < 0)
        perror("Failed to get anon_vma info");
    close(fd);
    return args;
}

struct folio_info_args get_folio_info(void *addr) {
    struct folio_info_args args = {0};
    args.virtual_address = addr;

    int fd = open_swapctl();
    if (fd < 0)
        return args;

    if (ioctl(fd, IOCTL_FOLIO_LRU_INFO, &args) < 0)
        perror("Failed to get folio info");
    close(fd);
    return args;
}

unsigned int count_rmap_vmas(void *addr) {
    int fd = open_swapctl();
    if (fd < 0)
        return 0;

    struct rmap_walk_args args = {0};
    args.virtual_address = addr;
    if (ioctl(fd, IOCTL_COUNT_RMAP_VMAS, &args) < 0)
        perror("Failed to count rmap vmas");
    close(fd);
    return args.nr_vmas;
}

struct swap_file_info get_swap_file_info(void *addr) {
    struct swap_file_info args = {0};
    args.virtual_address = addr;

    int fd = open_swapctl();
    if (fd < 0)
        return args;

    if (ioctl(fd, IOCTL_GET_SWAP_FILE_PATH, &args) < 0)
        perror("Failed to get swap file info");
    close(fd);
    return args;
}

unsigned long get_pte_value(void *addr) {
    unsigned long value = (unsigned long)addr;
    int fd = open_swapctl();

    if (fd < 0)
        return 0;

    if (ioctl(fd, IOCTL_GET_PT_PAGE_FROM_ADDRESS, &value) < 0)
        perror("Failed to get PTE value");
    close(fd);
    return value;
}

unsigned long get_folio_mapcount(void *addr) {
    struct folio_get_mapcount_args args = {0};
    args.virtual_address = addr;
    int fd = open_swapctl();
    if (fd < 0)
        return 0;
    if (ioctl(fd, IOCTL_FOLIO_GET_MAPCOUNT, &args) < 0)
        perror("Failed to get folio mapcount");
    close(fd);
    return args.mapcount;
}

unsigned int get_named_swap_alias_count(void *addr) {
    struct named_swap_alias_count_args args = {0};
    args.virtual_address = addr;
    int fd = open_swapctl();
    if (fd < 0)
        return 0;
    if (ioctl(fd, IOCTL_NAMED_SWAP_ALIAS_COUNT, &args) < 0)
        perror("Failed to get named-swap alias count");
    close(fd);
    return args.count;
}

static int named_swap_read_sysctl_str(const char *sysctl, char *buf, size_t size)
{
    FILE *fp;
    size_t len;

    if (!buf || size == 0)
        return -1;

    fp = fopen(sysctl, "r");
    if (!fp)
        return -1;
    if (!fgets(buf, size, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    return len ? 0 : -1;
}

int named_swap_get_root(char *buf, size_t size) {
    if (named_swap_read_sysctl_str(NAMED_SWAP_SYSCTL, buf, size) == 0)
        return 0;
    snprintf(buf, size, "%s", NAMED_SWAP_DEFAULT_ROOT);
    return 0;
}

static int named_swap_get_fs_root(char *buf, size_t size)
{
    if (named_swap_read_sysctl_str(NAMED_SWAP_FS_ROOT_SYSCTL, buf, size) == 0)
        return 0;
    snprintf(buf, size, "%s", NAMED_SWAP_DEFAULT_ROOT);
    return 0;
}

static int named_swap_index_under_root(const char *path, const char *root,
                                       unsigned long *index)
{
    size_t root_len;
    const char *digits;
    char *end;

    if (!path || !root || !index)
        return 0;
    root_len = strlen(root);
    if (!root_len || strncmp(path, root, root_len) != 0 || path[root_len] != '/')
        return 0;

    digits = path + root_len + 1;
    if (*digits == '\0')
        return 0;
    for (const char *p = digits; *p; p++) {
        if (!isdigit((unsigned char)*p))
            return 0;
    }

    errno = 0;
    *index = strtoul(digits, &end, 10);
    return errno == 0 && *end == '\0';
}

unsigned long named_swap_read_sysctl_ulong(const char *path)
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

unsigned long named_swap_swap_usage(void)
{
    return named_swap_read_sysctl_ulong("/proc/sys/vm/named_swap_swap_usage");
}

unsigned long named_swap_fs_usage(void)
{
    return named_swap_read_sysctl_ulong("/proc/sys/vm/named_swap_fs_usage");
}

unsigned long named_swap_pool_usage(void)
{
    return named_swap_swap_usage() + named_swap_fs_usage();
}

int named_swap_set_root(const char *path) {
    int fd;
    ssize_t n;
    int err;

    fd = open(NAMED_SWAP_SYSCTL, O_WRONLY | O_TRUNC);
    if (fd < 0)
        return -1;
    n = write(fd, path, strlen(path));
    err = errno;
    if (close(fd) != 0 && n >= 0) {
        return -1;
    }
    if (n < 0) {
        errno = err;
        return -1;
    }
    return 0;
}
struct page_prot_args get_page_prot(void *addr) {
    struct page_prot_args args = {0};
    args.virtual_address = addr;

    int fd = open_swapctl(); 
    if (fd < 0)
        return args;

    if (ioctl(fd, IOCTL_GET_PAGE_PROT, &args) < 0)
        perror("Failed to get page protections");
        
    close(fd);
    return args;
}


int parse_named_swap_index(const char *path, unsigned long *index) {
    char swap_root[PATH_MAX];
    char fs_root[PATH_MAX];

    named_swap_get_root(swap_root, sizeof(swap_root));
    named_swap_get_fs_root(fs_root, sizeof(fs_root));
    if (named_swap_index_under_root(path, swap_root, index))
        return 1;
    if (named_swap_index_under_root(path, fs_root, index))
        return 1;
    return 0;
}

void named_swap_path_for_index(char *path, size_t size, unsigned long index) {
    char swap_root[PATH_MAX];
    char fs_root[PATH_MAX];
    char swap_path[PATH_MAX];
    char fs_path[PATH_MAX];

    named_swap_get_root(swap_root, sizeof(swap_root));
    named_swap_get_fs_root(fs_root, sizeof(fs_root));
    snprintf(swap_path, sizeof(swap_path), "%s/%lu", swap_root, index);
    snprintf(fs_path, sizeof(fs_path), "%s/%lu", fs_root, index);
    if (access(swap_path, F_OK) == 0)
        snprintf(path, size, "%s", swap_path);
    else if (access(fs_path, F_OK) == 0)
        snprintf(path, size, "%s", fs_path);
    else
        snprintf(path, size, "%s/%lu", swap_root, index);
}

pid_t start_ftrace(void) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork failed");
        return -1;
    }

    if (pid == 0) {
        char *args[] = {
            "trace-cmd",
            "record",
            "-e",
            "named_swap:*",
            NULL,
        };

        execvp("trace-cmd", args);
        perror("execvp failed");
        _exit(EXIT_FAILURE);
    }

    sleep(1);
    return pid;
}

void stop_ftrace(char *test_name, pid_t pid) {
    if (pid <= 0)
        return;

    kill(pid, SIGINT);
    waitpid(pid, NULL, 0);

    char command[256];
    snprintf(command, sizeof(command), "trace-cmd report > %s.trace", test_name);
    system(command);
}

int check_vma_in_maps(unsigned char *expected_start, unsigned char *expected_end) {
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) {
        perror("Failed to open /proc/self/maps");
        return 0;
    }

    char line[512];
    unsigned long start, end;
    int found = 0;

    /* Read the maps file line by line */
    while (fgets(line, sizeof(line), fp)) {
        /* Parse the start and end addresses from the standard format: "start-end perms offset..." */
        if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
            /* Check if this VMA matches our expected boundaries exactly */
            if (start == (unsigned long)expected_start && end == (unsigned long)expected_end) {
                found = 1;
                break;
            }
        }
    }
    
    fclose(fp);
    return found;
}
