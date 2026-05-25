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

#define DEVICE_NAME "swapctl"
#define IOCTL_GET_SWAPFILE_COUNT _IOR('s', 0x01, int)
#define IOCTL_GET_SWAP_OFFSET_FROM_PAGE _IOR('s', 0x02, unsigned long)
#define IOCTL_VMA_HAS_SWAP_INFO _IOR('s', 0x03, int)
#define IOCTL_VMA_INFO _IOR('s', 0x04, struct vma_info_args)
#define IOCTL_IS_FOLIO_SEQ _IOR('s', 0x05, struct folio_info_args)
#define ICOTL_FOLIO_LRU_INFO _IOR('s', 0x06, struct folio_info_args)
#define ICOTL_GET_CURRENT_CGROUP _IOR('s', 0x07, unsigned short)
//a
#define IOCTL_GET_SWAPFILE_PATH _IOWR('s', 0x08, struct swap_path_args)

struct swap_path_args {
    void *virtual_address;
    char path[256];
};
//a
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
    case IOCTL_VMA_INFO: {
        struct vma_info_args args;

        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;

        // lock current->mm for reading
        // current => macro that returns task_struct
        mmap_read_lock(current->mm);
        struct vm_area_struct *vma = find_vma(current->mm, (unsigned long)args.virtual_address);
        if (!vma)
            return -EINVAL;

        args.vma_start = vma->vm_start;
        args.vma_end = vma->vm_end;
        args.vma_ptr = vma;
        args.vm_flags = vma->vm_flags;
        unsigned long flags;
        struct sequential_swap_context *sqwap = vma_get_sqwap(vma);
        if (sqwap) {
            spin_lock_irqsave(&sqwap->lock, flags);
            args.swap_info = sqwap->si;
            args.last_fault_offset = sqwap->last_fault_offset[0]; // Adapt based on index logic
            args.window_start = sqwap->window_start;
            args.window_end = sqwap->window_end;
            args.swap_ahead_size = sqwap->swap_ahead_size;
            spin_unlock_irqrestore(&sqwap->lock, flags);
        }
        mmap_read_unlock(current->mm);


        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;

        return 0;
    }
    /*
    case IOCTL_GET_SWAPFILE_COUNT: {
        int count = get_avail_swap_info_count();
        if (copy_to_user((int __user *)arg, &count, sizeof(count)))
            return -EFAULT;
        return 0;
        }
    */
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
    case IOCTL_VMA_HAS_SWAP_INFO: {
        struct swap_info_args args;

        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;

        struct vm_area_struct *vma = find_vma(current->mm, (unsigned long)args.virtual_address);
        if (!vma)
            return -EINVAL;
        struct sequential_swap_context *sqwap = vma_get_sqwap(vma);
        if (!sqwap)
            return -EINVAL;
        unsigned long flags;
        spin_lock_irqsave(&sqwap->lock, flags);
        args.has_swap_info = sqwap->si != NULL;
        spin_unlock_irqrestore(&sqwap->lock, flags);

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
    case IOCTL_GET_SWAPFILE_PATH: {
        struct swap_path_args args;
        struct vm_area_struct *vma;
        struct swap_info_struct *si;
        unsigned long flags;
        char *tmp_path;
        char *path_str;

        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;

        mmap_read_lock(current->mm);
        vma = find_vma(current->mm, (unsigned long)args.virtual_address);
        if (!vma) {
            mmap_read_unlock(current->mm);
            return -EINVAL;
        }
        struct sequential_swap_context *sqwap = vma_get_sqwap(vma);
        if (!sqwap) {
            mmap_read_unlock(current->mm);
            return -EINVAL;
        }

        spin_lock_irqsave(&sqwap->lock, flags);
        si = (struct swap_info_struct *)sqwap->si;
        spin_unlock_irqrestore(&sqwap->lock, flags);
        mmap_read_unlock(current->mm);

        if (!si || !si->swap_file)
            return -EINVAL;

        tmp_path = kmalloc(256, GFP_KERNEL);
        if (!tmp_path)
            return -ENOMEM;

        path_str = d_path(&si->swap_file->f_path, tmp_path, 256);
        if (IS_ERR(path_str)) {
            kfree(tmp_path);
            return PTR_ERR(path_str);
        }

        strncpy(args.path, path_str, sizeof(args.path) - 1);
        args.path[sizeof(args.path) - 1] = '\0';
        kfree(tmp_path);

        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;

        return 0;
    }
    case IOCTL_GET_ANON_VMAS: {
        struct anon_vmas_cow_args args;
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
            args.page_anon_vma = folio_get_anon_vma(folio);

            vma = find_vma(current->mm, (unsigned long)args.virtual_address);
            if (!vma) {
                mmap_read_unlock(current->mm);
                return -EINVAL;
            }
            args.vma_anon_vma = vma_get_anon_vma(vma);
            //unpin
            put_page(page);
        }
        mmap_read_unlock(current->mm);

        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;

        return 0;
    }

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
