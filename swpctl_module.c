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
#define RMAP_WALK_MAX_VMAS 64
#define IOCTL_GET_SWAPFILE_COUNT _IOR('s', 0x01, int)
#define IOCTL_GET_SWAP_OFFSET_FROM_PAGE _IOR('s', 0x02, unsigned long)
#define IOCTL_VMA_HAS_SWAP_INFO _IOR('s', 0x03, int)
#define IOCTL_VMA_INFO _IOR('s', 0x04, struct vma_info_args)
#define IOCTL_IS_FOLIO_SEQ _IOR('s', 0x05, struct folio_info_args)
#define ICOTL_FOLIO_LRU_INFO _IOR('s', 0x06, struct folio_info_args)
#define ICOTL_GET_CURRENT_CGROUP _IOR('s', 0x07, unsigned short)
#define IOCTL_ANON_VMA_INFO _IOR('s', 0x08, struct anon_vma_info_args)
#define ICOTL_RMAP_WALK _IOR('s', 0x09, struct rmap_walk_args)
struct swap_info_args {
    void *virtual_address;     // Input: User-space virtual address
    unsigned long offset;      // Output: Swap offset
    int has_swap_info;         // Output: Swap info presence
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
struct anon_vma_info_args {
    void *virtual_address;
    unsigned long vma_start;
    unsigned long vma_end;
    void *vma_ptr;
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
    void *folio_ptr;
    unsigned int nr_vmas;
    unsigned int total_vmas;
    unsigned int overflow;
    struct rmap_vma_info vmas[RMAP_WALK_MAX_VMAS];
};
struct folio_info_args {
    unsigned int is_seq;
    void *virtual_address;     // Input: User-space virtual address
    unsigned int is_anon;
    unsigned int is_file;
    unsigned int has_mapping;
    unsigned short memory_cgroup;
};

static bool swapctl_rmap_one(struct folio *folio, struct vm_area_struct *vma,
                             unsigned long addr, void *arg)
{
    struct rmap_walk_args *args = arg;
    unsigned int idx = args->nr_vmas;

    args->total_vmas++;
    if (idx >= RMAP_WALK_MAX_VMAS) {
        args->overflow = 1;
        return true;
    }

    args->vmas[idx].vma_ptr = vma;
    args->vmas[idx].vma_start = vma->vm_start;
    args->vmas[idx].vma_end = vma->vm_end;
    args->vmas[idx].address = addr;
    args->vmas[idx].vm_flags = vma->vm_flags;
    args->vmas[idx].anon_vma = vma->anon_vma;
    args->nr_vmas++;

    return true;
}

static long swapctl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    
    switch (cmd) {
    case IOCTL_VMA_INFO: {
        struct vma_info_args args;

        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;

        // lock current->mm for reading
        mmap_read_lock(current->mm);
        struct vm_area_struct *vma = find_vma(current->mm, (unsigned long)args.virtual_address);
        if (!vma)
            return -EINVAL;

        args.vma_start = vma->vm_start;
        args.vma_end = vma->vm_end;
        args.vma_ptr = vma;
        args.vm_flags = vma->vm_flags;
        unsigned long flags;
        spin_lock_irqsave(&vma->swap_lock, flags);
        args.swap_info = vma->si; // Pointer to swap info
        spin_unlock_irqrestore(&vma->swap_lock, flags);
        spin_lock_irqsave(&vma->reclaim_lock, flags);
        args.last_fault_offset = vma->last_fault_offset;
        args.window_start = vma->window_start;
        args.window_end = vma->window_end;
        args.swap_ahead_size = vma->swap_ahead_size;
        spin_unlock_irqrestore(&vma->reclaim_lock, flags);
        mmap_read_unlock(current->mm);


        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;

        return 0;
    }
    case IOCTL_ANON_VMA_INFO: {
        struct anon_vma_info_args args;
        struct vm_area_struct *vma;
        struct anon_vma *anon_vma;
        unsigned long addr;

        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;

        addr = (unsigned long)args.virtual_address;
        mmap_read_lock(current->mm);
        vma = find_vma(current->mm, addr);
        if (!vma || addr < vma->vm_start) {
            mmap_read_unlock(current->mm);
            return -EINVAL;
        }

        args.vma_start = vma->vm_start;
        args.vma_end = vma->vm_end;
        args.vma_ptr = vma;
        anon_vma = vma->anon_vma;
        args.anon_vma = anon_vma;
        args.root = NULL;
        args.parent = NULL;
        args.refcount = 0;
        args.num_children = 0;
        args.num_active_vmas = 0;

        if (anon_vma) {
            anon_vma_lock_read(anon_vma);
            args.root = anon_vma->root;
            args.parent = anon_vma->parent;
            args.refcount = atomic_read(&anon_vma->refcount);
            args.num_children = anon_vma->num_children;
            args.num_active_vmas = anon_vma->num_active_vmas;
            anon_vma_unlock_read(anon_vma);
        }
        mmap_read_unlock(current->mm);

        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;

        return 0;
    }
    case IOCTL_GET_SWAPFILE_COUNT: {
        int count = get_avail_swap_info_count();
        if (copy_to_user((int __user *)arg, &count, sizeof(count)))
            return -EFAULT;
        return 0;
        }
    case IOCTL_GET_SWAP_OFFSET_FROM_PAGE: {
        struct swap_info_args args;

        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;
        printk(KERN_INFO "swapctl: Getting swap offset for address 0x%lx\n", (unsigned long)args.virtual_address);
         // Pin the user page

        struct page *page = NULL;
        int ret = get_user_pages_fast((unsigned long)args.virtual_address, 1, 0, &page);
        if (ret != 1) {
            pr_err("swapctl: Failed to get page for user address %px (ret=%d)\n", 
                args.virtual_address, ret);
            return -EFAULT;
        }
        struct folio* folio = page_folio(page);
        if(!folio) {
            pr_err("swapctl: Invalid folio for address %px\n", args.virtual_address);
            return -EINVAL;
        }
        printk(KERN_INFO "swapctl: Folio %px has swap %lu\n", folio, swp_offset(folio->swap));
        args.offset = swp_offset(folio->swap);
        
        put_page(page); // ADD THIS LINE before return

        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;

        return 0;
    }
    case ICOTL_RMAP_WALK: {
        struct rmap_walk_args args;
        struct page *page = NULL;
        struct folio *folio;
        struct rmap_walk_control rwc = {
            .arg = &args,
            .rmap_one = swapctl_rmap_one,
        };
        int ret;

        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;

        memset(args.vmas, 0, sizeof(args.vmas));
        args.folio_ptr = NULL;
        args.nr_vmas = 0;
        args.total_vmas = 0;
        args.overflow = 0;

        ret = get_user_pages_fast((unsigned long)args.virtual_address, 1, 0, &page);
        if (ret != 1) {
            pr_err("swapctl: Failed to get page for user address %px (ret=%d)\n",
                   args.virtual_address, ret);
            return -EFAULT;
        }

        folio = page_folio(page);
        if (!folio) {
            put_page(page);
            return -EINVAL;
        }

        args.folio_ptr = folio;
        rmap_walk(folio, &rwc);
        put_page(page);

        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;

        return 0;
    }
    case IOCTL_VMA_HAS_SWAP_INFO: {
        struct swap_info_args args;

        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;

        struct vm_area_struct *vma = find_vma(current->mm, (unsigned long)args.virtual_address);
        if (!vma)
            return -EINVAL;
        unsigned long flags;
        spin_lock_irqsave(&vma->swap_lock, flags);
        args.has_swap_info = vma->si != NULL;
        spin_unlock_irqrestore(&vma->swap_lock, flags);

        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;

        return 0;
    }
    case IOCTL_IS_FOLIO_SEQ: {
        struct folio_info_args args;
        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;
        printk(KERN_INFO "swapctl: Getting folio info for address 0x%lx\n", (unsigned long)args.virtual_address);
         // Pin the user page

        struct page *page = NULL;
        int ret = get_user_pages_fast((unsigned long)args.virtual_address, 1, 0, &page);
        if (ret != 1) {
            pr_err("swapctl: Failed to get page for user address %px (ret=%d)\n", 
                args.virtual_address, ret);
            return -EFAULT;
        }
        struct folio* folio = page_folio(page);
        if(!folio) {
            pr_err("swapctl: Invalid folio for address %px\n", args.virtual_address);
            return -EINVAL;
        }
        args.is_seq = folio_test_seq(folio);
        put_page(page); // ADD THIS LINE before return
        printk(KERN_INFO "swapctl: Folio %px is_seq %u\n", folio, args.is_seq);
        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;

        return 0;
    }
    case ICOTL_FOLIO_LRU_INFO: {
        struct folio_info_args args;
        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;
        printk(KERN_INFO "swapctl: Getting folio info for address 0x%lx\n", (unsigned long)args.virtual_address);
         // Pin the user page

        struct page *page = NULL;
        int ret = get_user_pages_fast((unsigned long)args.virtual_address, 1, 0, &page);
        if (ret != 1) {
            pr_err("swapctl: Failed to get page for user address %px (ret=%d)\n", 
                args.virtual_address, ret);
            return -EFAULT;
        }
        struct folio* folio = page_folio(page);
        if(!folio) {
            pr_err("swapctl: Invalid folio for address %px\n", args.virtual_address);
            return -EINVAL;
        }
        args.is_anon = folio_test_anon(folio);
        args.is_file = folio_is_file_lru(folio);
        args.has_mapping = folio->mapping != NULL;
        struct mem_cgroup *memcg = folio_memcg(folio);
        if (memcg) {
            args.memory_cgroup = mem_cgroup_id(memcg);
        }
        put_page(page); // ADD THIS LINE before return
        printk(KERN_INFO "swapctl: Folio %px is_anon %u is_file %u has_mapping %u\n", folio, args.is_anon, args.is_file, args.has_mapping);
        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;

        return 0;
    }
    case ICOTL_GET_CURRENT_CGROUP: {
        unsigned short memcg_id = -1;
        struct mem_cgroup *memcg = mem_cgroup_from_task(current);
        if (memcg) {
            memcg_id = mem_cgroup_id(memcg);
        }
        if (copy_to_user((unsigned short __user *)arg, &memcg_id, sizeof(memcg_id)))
            return -EFAULT;
        return 0;
    }
    default:
        return -EINVAL;
}
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
