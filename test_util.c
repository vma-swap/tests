#include "test_util.h"

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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

struct rmap_walk_args get_rmap_walk_info(void *addr) {
    struct rmap_walk_args args = {0};
    args.virtual_address = addr;

    int fd = open_swapctl();
    if (fd < 0)
        return args;

    if (ioctl(fd, ICOTL_RMAP_WALK, &args) < 0)
        perror("Failed to walk folio rmap");
    close(fd);
    return args;
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
