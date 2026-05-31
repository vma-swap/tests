#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/swap.h>
#include <linux/mutex.h>
#include <linux/plist.h>
#include <linux/swapops.h>
#include <linux/mm_inline.h>
#include <linux/rmap.h>

#define DEVICE_NAME "swapctl"
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

struct swap_path_args {
    void *virtual_address;
    char path[256];
};
struct swap_info_args {
    void *virtual_address;     // Input: User-space virtual address
    unsigned long offset;      // Output: Swap offset
    int has_swap_info;         // Output: Swap info presence
};
#define IOCTL_GET_RMAP_COUNT _IOWR('s', 0x0B, struct rmap_count_args)
struct rmap_count_args {
    void *virtual_address;
    int rmap_count;
};
static bool count_rmap_one(struct folio *folio, struct vm_area_struct *vma, unsigned long address, void *arg) {
    int *count = (int *)arg;
    (*count)++;
    return true; // return true to continue walking to the next VMA
}



struct anon_vma_cow_folio_args {
    void *virtual_address;
    unsigned long page_anon_vma;
};

struct anon_vma_cow_vma_args {
    void *virtual_address;
    unsigned long vma_anon_vma;
};

struct vma_info_args {
    void *virtual_address;
    unsigned long vma_start;
    unsigned long vma_end;
    void *vma_ptr;
    unsigned long vm_flags;
    void *swap_info;
    pgoff_t last_fault_offset;
	pgoff_t window_start;
	pgoff_t window_end;
	size_t swap_ahead_size; 
};
struct folio_info_args {
    unsigned int is_seq;
    void *virtual_address;     // Input: User-space virtual address
    unsigned int is_anon;
    unsigned int is_file;
    unsigned int has_mapping;
    unsigned short memory_cgroup;
};

static long swapctl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    
    switch (cmd) {
    case IOCTL_GET_ANON_VMA_FOLIO: {
        struct anon_vma_cow_folio_args args;
        struct page *page = NULL;
        struct folio *folio = NULL;

        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;

        mmap_read_lock(current->mm);
        //gotta pin the addr from userspace so it doesnt slip
        //use this to fetch page from addr and then folio from page
        int retu = get_user_pages_fast((unsigned long)args.virtual_address, 1, 0, &page);
        if (retu == 1){
            folio = page_folio(page);
            args.page_anon_vma = (unsigned long)folio_get_anon_vma(folio);
            //get more metadata from anon_vma
            //unpin
            put_page(page);
        }
        mmap_read_unlock(current->mm);

        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;

        return 0;
    }

    case IOCTL_GET_ANON_VMA_VMA: {
        struct anon_vma_cow_vma_args args;
        struct vm_area_struct *vma;

        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;

        mmap_read_lock(current->mm);
        vma = find_vma(current->mm, (unsigned long)args.virtual_address);
        if (!vma) {
            mmap_read_unlock(current->mm);
            return -EINVAL;
        }
        args.vma_anon_vma = (unsigned long)vma_get_anon_vma(vma);
        mmap_read_unlock(current->mm);

        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;
    

    return 0;
    }
    case IOCTL_GET_RMAP_COUNT: {
    struct rmap_count_args args;
    struct page *page = NULL;
    struct folio *folio = NULL;

    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    mmap_read_lock(current->mm);
    
    // Pin the page and get the underlying folio
    if (get_user_pages_fast((unsigned long)args.virtual_address, 1, 0, &page) == 1) {
        folio = page_folio(page);
        args.rmap_count = 0; 
        
        struct rmap_walk_control rwc = {
            .rmap_one = count_rmap_one,
            .arg = &args.rmap_count,
            .anon_lock = folio_lock_anon_vma_read,
        };
        
        // This walks all VMAs mapped to this folio and runs 'count_rmap_one'
        rmap_walk(folio, &rwc);
        
        put_page(page);
    } else {
        args.rmap_count = -1; // If the page is not resident/invalid
    }
    
    mmap_read_unlock(current->mm);

    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;

    return 0;
}
    default:
        return -ENOTTY;
    }
    return 0;
}
static const struct file_operations swapctl_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = swapctl_ioctl,
    .compat_ioctl = swapctl_ioctl,
};

static struct miscdevice swapctl_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = DEVICE_NAME,
    .fops = &swapctl_fops,
    .mode = 0666,
};

static int __init swapctl_init(void)
{
    int ret = misc_register(&swapctl_dev);
    if (ret)
        pr_err("swapctl: failed to register misc device\n");
    else
        pr_info("swapctl: device registered as /dev/%s\n", DEVICE_NAME);
    return ret;
}

static void __exit swapctl_exit(void)
{
    misc_deregister(&swapctl_dev);
    pr_info("swapctl: module unloaded\n");
}

module_init(swapctl_init);
module_exit(swapctl_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Daniel");
MODULE_DESCRIPTION("Expose swapfile stats");
