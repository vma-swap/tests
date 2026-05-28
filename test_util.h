// test_util.h
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stddef.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

struct vma_info_args {
    void *virtual_address;
    unsigned long vma_start;
    unsigned long vma_end;
    void *vma_ptr;
    unsigned long vm_flags;
    void *swap_info;
    unsigned long last_fault_offset;
	unsigned long window_start;
	unsigned long window_end;
	size_t swap_ahead_size; 
};

#define DEVICE "/dev/swapctl"
#define IOCTL_GET_SWAPFILE_COUNT _IOR('s', 0x01, int)
#define IOCTL_GET_SWAP_OFFSET_FROM_PAGE _IOR('s', 0x02, unsigned long)
#define IOCTL_VMA_HAS_SWAP_INFO _IOR('s', 0x03, int)
#define IOCTL_VMA_INFO _IOR('s', 0x04, struct vma_info_args)
#define IOCTL_IS_FOLIO_SEQ _IOR('s', 0x05, struct folio_info_args)
#define ICOTL_FOLIO_LRU_INFO _IOR('s', 0x06, struct folio_info_args)
#define ICOTL_GET_CURRENT_CGROUP _IOR('s', 0x07, unsigned short)
#define IOCTL_GET_SWAPFILE_PATH _IOWR('s', 0x08, struct swap_path_args)
#define IOCTL_GET_ANON_VMA_FOLIO _IOR('s', 0x09, struct anon_vma_cow_folio_args)
#define IOCTL_GET_ANON_VMA_VMA _IOR('s', 0x0A, struct anon_vma_cow_vma_args)
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))


unsigned int is_folio_seq(void *addr);
unsigned int is_folio_anon(void *addr);
unsigned int is_folio_file(void *addr);
unsigned int folio_has_mapping(void *addr);
unsigned short get_folio_memcg_id(void *addr);
unsigned short get_current_memcg_id(void);
int get_current_memcg_id_fs(void);
int evict_mem(int pages);
int swapout_page(void *addr);
int swapout_pages(void *addr, unsigned long long pages);
int get_swapfile_count();
int get_swap_offset_from_page(void *addr);
void make_swaps(int num_swapfiles, int swap_flags);
int vma_has_swap_info(void *addr);
int disable_swaps();
void* map_anon_region(size_t size);
void* map_large_anon_region(unsigned long long size);
pid_t start_ftrace(void);
void stop_ftrace(char* test_name, pid_t pid);
struct vma_info_args get_vma_info(void *addr);
void set_minimal_swapfile_num(int num);
void start_measurement(void);
//return time in micro
double stop_measurement(void);
int create_tempfile(size_t size);
void drop_caches();

#endif // TEST_UTIL_H
