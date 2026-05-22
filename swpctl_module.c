#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/swap.h>
#include <linux/mutex.h>
#include <linux/plist.h>
#include <linux/slab.h>
#include <linux/swapops.h>
#include <linux/mm_inline.h>
#include <linux/rmap.h>

#define DEVICE_NAME "swapctl"
#define RMAP_WALK_MAX_VMAS 64
#define IOCTL_GET_SWAP_OFFSET_FROM_PAGE _IOR('s', 0x01, unsigned long)
#define IOCTL_VMA_INFO _IOR('s', 0x02, struct vma_info_args)
#define ICOTL_FOLIO_LRU_INFO _IOR('s', 0x03, struct folio_info_args)
#define ICOTL_GET_CURRENT_CGROUP _IOR('s', 0x04, unsigned short)
#define IOCTL_ANON_VMA_INFO _IOR('s', 0x05, struct anon_vma_info_args)
#define ICOTL_COUNT_RMAP_VMAS _IOWR('s', 0x06, struct rmap_walk_args)

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
    unsigned int *nr_vmas = arg;
    (*nr_vmas)++;
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
        if (!vma) {
            mmap_read_unlock(current->mm);
            return -EINVAL;
        }

        args.vma_start = vma->vm_start;
        args.vma_end = vma->vm_end;
        args.vma_ptr = vma;
        args.vm_flags = vma->vm_flags;
        mmap_read_unlock(current->mm);


        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;

        return 0;
    }
    case IOCTL_ANON_VMA_INFO: {
        struct anon_vma_info_args args;
        struct anon_vma *anon_vma;
        unsigned long addr;

        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;

        addr = (unsigned long)args.virtual_address;
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
        anon_vma = folio_get_anon_vma(folio);
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
            if (atomic_dec_and_test(&anon_vma->refcount))
		        __put_anon_vma(anon_vma);
        }

        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;

        return 0;
    }
    case ICOTL_COUNT_RMAP_VMAS: {
        struct rmap_walk_args args;
        unsigned int nr_vmas = 0;
        struct page *page = NULL;
        struct folio *folio;
        struct rmap_walk_control rwc = {
            .rmap_one = swapctl_rmap_one,
        };
        int ret;

        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;

        rwc.arg = &nr_vmas;

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

        rmap_walk(folio, &rwc);
        put_page(page);

        args.nr_vmas = nr_vmas;
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
