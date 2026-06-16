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
int parse_named_swap_index(const char *path, unsigned long *index) {
    const char *digits;
    char *end;

    if (strncmp(path, NAMED_SWAP_PREFIX, strlen(NAMED_SWAP_PREFIX)) != 0)
        return 0;

    digits = path + strlen(NAMED_SWAP_PREFIX);
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

void named_swap_path_for_index(char *path, size_t size, unsigned long index) {
    snprintf(path, size, NAMED_SWAP_PREFIX "%lu", index);
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
            "mmap:*",
            "-e",
            "kmem:*",
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
