#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#define DEVICE "/dev/swapctl"
#define RMAP_WALK_MAX_VMAS 64
#define NAMED_SWAP_PREFIX "/.named_swap/"

struct vma_info_args {
    void *virtual_address;
    unsigned long vma_start;
    unsigned long vma_end;
    void *vma_ptr;
    unsigned long vm_flags;
};

struct anon_vma_info_args {
    void *virtual_address;
    void *named_swap_file;
    void *anon_vma;
    void *root;
    void *parent;
    unsigned long refcount;
    unsigned long num_children;
    unsigned long num_active_vmas;
};

enum anon_vma_info_source {
    ANON_VMA_FOLIO,
    ANON_VMA_VMA,
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

struct folio_info_args {
    unsigned int is_seq;
    void *virtual_address;     // Input: User-space virtual address
    unsigned int is_anon;
    unsigned int is_file;
    unsigned int has_mapping;
    unsigned short memory_cgroup;
};
struct swap_file_info {
    void *virtual_address;
    char path[PATH_MAX];
    unsigned long offset;
    unsigned long size;
};
struct swap_file_info_mremap{
    void *virtual_address;
    char path[PATH_MAX];
    unsigned long offset;
    unsigned long size; //original folio size
    unsigned long file_size;   // NEW: Entire backing file size
}

#define IOCTL_VMA_INFO _IOR('s', 0x02, struct vma_info_args)
#define IOCTL_ANON_VMA_INFO _IOR('s', 0x05, struct anon_vma_info_args)
#define IOCTL_COUNT_RMAP_VMAS _IOWR('s', 0x06, struct rmap_walk_args)
#define IOCTL_GET_SWAP_FILE_PATH _IOWR('s', 0x07, struct swap_file_info)
#define IOCTL_FOLIO_LRU_INFO _IOR('s', 0x03, struct folio_info_args)
#define IOCTL_GET_CURRENT_CGROUP _IOR('s', 0x04, unsigned short)
#define IOCTL_ANON_VMA_INFO_FROM_VMA _IOR('s', 0x08, struct anon_vma_info_args)

struct vma_info_args get_vma_info(void *addr);
struct anon_vma_info_args get_anon_vma_info(void *addr);
struct anon_vma_info_args get_anon_vma_info_from_vma(void *addr);
unsigned int count_rmap_vmas(void *addr);
struct swap_file_info get_swap_file_info(void *addr);
struct folio_info_args get_folio_info(void *addr);
unsigned short get_current_cgroup(void);
int parse_named_swap_index(const char *path, unsigned long *index);
void named_swap_path_for_index(char *path, size_t size, unsigned long index);

pid_t start_ftrace(void);
void stop_ftrace(char *test_name, pid_t pid);

#endif
