#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#define DEVICE "/dev/swapctl"
#define RMAP_WALK_MAX_VMAS 64

struct vma_info_args {
    void *virtual_address;
    unsigned long vma_start;
    unsigned long vma_end;
    void *vma_ptr;
    unsigned long vm_flags;
};

struct anon_vma_info_args {
    void *virtual_address;
    void *anon_vma;
    void *root;
    void *parent;
    unsigned long refcount;
    unsigned long num_children;
    unsigned long num_active_vmas;
};

struct rmap_vma_info {
    void *vma_ptr;
    unsigned long vma_start;
    unsigned long vma_end;
    unsigned long address;
    unsigned long vm_flags;
    void *anon_vma;
};

struct rmap_walk_args {
    void *virtual_address;
    unsigned int nr_vmas;
};

struct swap_file_info {
    void *virtual_address;
    char path[PATH_MAX];
    unsigned long offset;
    unsigned long size;
};

#define IOCTL_VMA_INFO _IOR('s', 0x02, struct vma_info_args)
#define IOCTL_ANON_VMA_INFO _IOR('s', 0x05, struct anon_vma_info_args)
#define ICOTL_COUNT_RMAP_VMAS _IOWR('s', 0x06, struct rmap_walk_args)
#define IOCTL_GET_SWAP_FILE_PATH _IOWR('s', 0x07, struct swap_file_info)

struct vma_info_args get_vma_info(void *addr);
struct anon_vma_info_args get_anon_vma_info(void *addr);
unsigned int count_rmap_vmas(void *addr);
struct swap_file_info get_swap_file_info(void *addr);

pid_t start_ftrace(void);
void stop_ftrace(char *test_name, pid_t pid);

#endif
