#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/swap.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/plist.h>
#include <linux/slab.h>
#include <linux/swapops.h>
#include <linux/mm_inline.h>
#include <linux/pgtable.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/xarray.h>

#define DEVICE_NAME "swapctl"
#define RMAP_WALK_MAX_VMAS 64
#define IOCTL_GET_SWAP_OFFSET_FROM_PAGE _IOR('s', 0x01, unsigned long)
#define IOCTL_VMA_INFO _IOR('s', 0x02, struct vma_info_args)
#define IOCTL_FOLIO_LRU_INFO _IOR('s', 0x03, struct folio_info_args)
#define IOCTL_GET_CURRENT_CGROUP _IOR('s', 0x04, unsigned short)
#define IOCTL_ANON_VMA_INFO _IOR('s', 0x05, struct anon_vma_info_args)
#define IOCTL_COUNT_RMAP_VMAS _IOWR('s', 0x06, struct rmap_walk_args)
#define IOCTL_GET_SWAP_FILE_PATH _IOWR('s', 0x07, struct swap_file_info)
#define IOCTL_ANON_VMA_INFO_FROM_VMA _IOR('s', 0x08, struct anon_vma_info_args)
#define IOCTL_GET_PT_PAGE_FROM_ADDRESS _IOWR('s', 0x09, unsigned long)
#define IOCTL_FOLIO_GET_MAPCOUNT _IOWR('s', 0x10, struct folio_get_mapcount_args)
#define IOCTL_NAMED_SWAP_ALIAS_COUNT _IOWR('s', 0x11, struct named_swap_alias_count_args)
#define IOCTL_GET_PAGE_PROT _IOR('s', 0x12, struct page_prot_args)

struct folio_get_mapcount_args {
    void *virtual_address;
    unsigned long mapcount;
};

struct named_swap_alias_count_args {
    void *virtual_address;
    unsigned int count;
};

struct swap_file_info {
    void *virtual_address;
    char path[PATH_MAX];
    unsigned long offset;
    unsigned long size;
    unsigned long file_size;   // NEW: Entire backing file size
    unsigned long allocated_blocks; /* NEW: Extracted from i_blocks */
};
struct page_prot_args {
    void *virtual_address;
    unsigned int is_readable;
    unsigned int is_writable;
    unsigned int is_executable;
};
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

enum swapctl_pte_state {
    SWAPCTL_PTE_NONE = 0,
    SWAPCTL_PTE_PRESENT,
    SWAPCTL_PTE_SWAP,
    SWAPCTL_PTE_NAMED_SWAP,
    SWAPCTL_PTE_NON_SWAP,
};

struct swapctl_pte_info {
    unsigned long pte_value;
    enum swapctl_pte_state state;
    unsigned long pfn;
    unsigned int swp_type;
    unsigned long swp_offset;
    unsigned long named_swap_index;
};

static bool swapctl_rmap_one(struct folio *folio, struct vm_area_struct *vma,
                             unsigned long addr, void *arg)
{
    unsigned int *nr_vmas = arg;
    (*nr_vmas)++;
    return true;
}

static int swapctl_named_swap_mapcount(struct address_space *mapping,
                                       pgoff_t index)
{
    void *entry;
    int mapcount = 0;

    if (!mapping || !mapping_named_swap(mapping))
        return 0;

    xa_lock_irq(&mapping->i_pages);
    entry = xa_load(&mapping->i_pages, index);
    if (!entry)
        goto out;

    if (!xa_is_value(entry))
        mapcount = folio_mapcount(entry);

out:
    xa_unlock_irq(&mapping->i_pages);
    return mapcount;
}

static int swapctl_get_pte_info(struct mm_struct *mm, unsigned long address,
                                struct swapctl_pte_info *info)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;
    pte_t pteval;
    spinlock_t *ptl;

    memset(info, 0, sizeof(*info));

    pgd = pgd_offset(mm, address);
    if (pgd_none(*pgd))
        return 0;
    if (pgd_bad(*pgd))
        return -EINVAL;

    p4d = p4d_offset(pgd, address);
    if (p4d_none(*p4d))
        return 0;
    if (p4d_bad(*p4d))
        return -EINVAL;

    pud = pud_offset(p4d, address);
    if (pud_none(*pud))
        return 0;
    if (pud_bad(*pud))
        return -EINVAL;

    pmd = pmd_offset(pud, address);
    if (pmd_none(*pmd))
        return 0;
    if (pmd_bad(*pmd))
        return -EINVAL;
    if (pmd_trans_huge(*pmd) || pmd_devmap(*pmd))
        return -EOPNOTSUPP;

    ptl = pte_lockptr(mm, pmd);
    spin_lock(ptl);
    pte = pte_offset_kernel(pmd, address);
    pteval = ptep_get(pte);
    info->pte_value = pte_val(pteval);

    if (pte_none(pteval)) {
        info->state = SWAPCTL_PTE_NONE;
    } else if (pte_present(pteval)) {
        info->state = SWAPCTL_PTE_PRESENT;
        info->pfn = pte_pfn(pteval);
    } else if (is_swap_pte(pteval)) {
        swp_entry_t entry = pte_to_swp_entry(pteval);

        info->swp_type = swp_type(entry);
        info->swp_offset = swp_offset(entry);

        if (is_named_swap_entry(entry)) {
            info->state = SWAPCTL_PTE_NAMED_SWAP;
            info->named_swap_index = named_swap_entry_index(entry);
        } else if (non_swap_entry(entry)) {
            info->state = SWAPCTL_PTE_NON_SWAP;
        } else {
            info->state = SWAPCTL_PTE_SWAP;
        }
    }
    spin_unlock(ptl);

    return 0;
}

static int swapctl_get_pte_value(struct mm_struct *mm, unsigned long address,
                                 unsigned long *pte_value)
{
    struct swapctl_pte_info info;
    int ret;

    ret = swapctl_get_pte_info(mm, address, &info);
    if (ret)
        return ret;

    *pte_value = info.pte_value;
    return 0;
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
        args.named_swap_file = NULL;

        if (anon_vma) {
            anon_vma_lock_read(anon_vma);
            args.root = anon_vma->root;
            args.parent = anon_vma->parent;
            args.refcount = atomic_read(&anon_vma->refcount) - 1;
            args.num_children = anon_vma->num_children;
            args.num_active_vmas = anon_vma->num_active_vmas;
            args.named_swap_file = anon_vma->named_swap_file;
            anon_vma_unlock_read(anon_vma);
            put_anon_vma(anon_vma);
        }

        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;

        return 0;
    }

    case IOCTL_ANON_VMA_INFO_FROM_VMA: {
        struct anon_vma_info_args args;
        struct vm_area_struct *vma;
        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;

        mmap_read_lock(current->mm);
        vma = find_vma(current->mm, (unsigned long)args.virtual_address);
        if (!vma) {
            mmap_read_unlock(current->mm);
            return -EINVAL;
        }
        args.anon_vma = NULL;
        args.root = NULL;
        args.parent = NULL;
        args.refcount = 0;
        args.num_children = 0;
        args.num_active_vmas = 0;
        args.named_swap_file = NULL;
        if(vma->anon_vma) {
            anon_vma_lock_read(vma->anon_vma);
            args.named_swap_file = vma->anon_vma->named_swap_file;
            args.anon_vma = vma->anon_vma;
            args.root = vma->anon_vma->root;
            args.parent = vma->anon_vma->parent;
            args.refcount = atomic_read(&vma->anon_vma->refcount);
            args.num_children = vma->anon_vma->num_children;
            args.num_active_vmas = vma->anon_vma->num_active_vmas;
            anon_vma_unlock_read(vma->anon_vma);
        }
        mmap_read_unlock(current->mm);
        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;
        return 0;
    }

    case IOCTL_COUNT_RMAP_VMAS: {
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
    case IOCTL_FOLIO_LRU_INFO: {
        struct folio_info_args args;
        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;
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
        args.virtual_address = (void*)folio;
        struct mem_cgroup *memcg = folio_memcg(folio);
        if (memcg) {
            args.memory_cgroup = mem_cgroup_id(memcg);
        }
        put_page(page); // ADD THIS LINE before return
        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;

        return 0;
    }
    case IOCTL_GET_CURRENT_CGROUP: {
        unsigned short memcg_id = -1;
        struct mem_cgroup *memcg = mem_cgroup_from_task(current);
        if (memcg) {
            memcg_id = mem_cgroup_id(memcg);
        }
        if (copy_to_user((unsigned short __user *)arg, &memcg_id, sizeof(memcg_id)))
            return -EFAULT;
        return 0;
    }
case IOCTL_GET_SWAP_FILE_PATH: {
        struct swap_file_info *args;
        char *path_buf;
        char *path;
        struct page *page = NULL;
        struct folio *folio;
        struct anon_vma *anon_vma;
        struct file *named_swap_file;
        struct mm_struct *mm = current->mm;
        struct vm_area_struct* vma;
        int ret;

        args = kzalloc(sizeof(*args), GFP_KERNEL);
        if (!args)
            return -ENOMEM;

        path_buf = kmalloc(PATH_MAX, GFP_KERNEL);
        if (!path_buf) {
            kfree(args);
            return -ENOMEM;
        }

        if (copy_from_user(args, (void __user *)arg, sizeof(*args))) {
            kfree(path_buf);
            kfree(args);
            return -EFAULT;
        }

        args->path[0] = '\0';
        args->offset = 0;
        args->size = 0;
        /*
        ret = get_user_pages_fast((unsigned long)args->virtual_address, 1, 0, &page);
        if (ret != 1) {
            pr_err("swapctl: Failed to get page for user address %px (ret=%d)\n",
                args->virtual_address, ret);
            kfree(path_buf);
            kfree(args);
            return -EFAULT;
        }
        folio = page_folio(page);
        if(!folio) {
            pr_err("swapctl: Invalid folio for address %px\n", args->virtual_address);
            put_page(page);
            kfree(path_buf);
            kfree(args);
            return -EINVAL;
        }
        anon_vma = folio_get_anon_vma(folio);
        if(!anon_vma) {
            pr_err("swapctl: Invalid anon_vma for address %px\n", args->virtual_address);
            put_page(page);
            kfree(path_buf);
            kfree(args);
            return -EINVAL;
        }
        named_swap_file = anon_vma->named_swap_file;
        */

        /* Safely lock the mm and look up the VMA directly */
        mmap_read_lock(mm);
        vma = vma_lookup(mm, (unsigned long)args->virtual_address);
        
        if (!vma || !vma->anon_vma || !vma->anon_vma->named_swap_file) {
            mmap_read_unlock(mm);
            kfree(path_buf);
            kfree(args);
            return -EINVAL; /* Not a valid named_swap VMA */
        }

        named_swap_file = vma->anon_vma->named_swap_file;
        if(named_swap_file) {
            path = named_swap_file_path(named_swap_file, path_buf, PATH_MAX);
            if (IS_ERR(path)) {
                /*
                put_anon_vma(anon_vma);
                put_page(page);
                */
                mmap_read_unlock(mm);
                kfree(path_buf);
                kfree(args);
                return PTR_ERR(path);
            }
            strscpy(args->path, path, sizeof(args->path));

            /* Calculate offset logically from the VMA instead of the folio index */

            args->offset = ((unsigned long)args->virtual_address - vma->vm_start) + 
                       (vma->vm_pgoff << PAGE_SHIFT);
            args->size = PAGE_SIZE;

            /*
            //addition
            vma = vma_lookup(mm, args->virtual_address);
            if (!vma) {
                return -EINVAL;
            }
            */

            args->file_size = named_swap_file_size(named_swap_file); 

            args->allocated_blocks = named_swap_file_blocks(named_swap_file);
        }

        mmap_read_unlock(mm);

        /*
        put_anon_vma(anon_vma);
        put_page(page);
        */

        if (copy_to_user((void __user *)arg, args, sizeof(*args))) {
            kfree(path_buf);
            kfree(args);
            return -EFAULT;
        }
        kfree(path_buf);
        kfree(args);
        return 0;
    }
    case IOCTL_GET_PT_PAGE_FROM_ADDRESS: {
        unsigned long address;
        unsigned long pte_value;
        int ret;

        if (copy_from_user(&address, (void __user *)arg, sizeof(address)))
            return -EFAULT;

        mmap_read_lock(current->mm);
        ret = swapctl_get_pte_value(current->mm, address, &pte_value);
        mmap_read_unlock(current->mm);
        if (ret)
            return ret;

        if (copy_to_user((void __user *)arg, &pte_value, sizeof(pte_value)))
            return -EFAULT;

        return 0;
    }
    case IOCTL_FOLIO_GET_MAPCOUNT: {
        //only works if there were no forks.
        struct folio_get_mapcount_args args;
        struct vm_area_struct *vma;
        pgoff_t index;

        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;

        args.mapcount = 0;

        mmap_read_lock(current->mm);
        vma = find_vma(current->mm, (unsigned long)args.virtual_address);
        if (!vma || (unsigned long)args.virtual_address < vma->vm_start) {
            mmap_read_unlock(current->mm);
            return -EINVAL;
        }

        if (vma->vm_file) {
            index = linear_page_index(vma, (unsigned long)args.virtual_address);
            args.mapcount = swapctl_named_swap_mapcount(vma->vm_file->f_mapping,
                                                        index);
        }
        mmap_read_unlock(current->mm);

        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;

        return 0;
    }
    case IOCTL_NAMED_SWAP_ALIAS_COUNT: {
        struct named_swap_alias_count_args args;
        struct vm_area_struct *vma;
        unsigned long address;

        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;

        address = (unsigned long)args.virtual_address;
        args.count = 0;

        mmap_read_lock(current->mm);
        vma = find_vma(current->mm, address);
        if (!vma || address < vma->vm_start) {
            mmap_read_unlock(current->mm);
            return -EINVAL;
        }

        if (vma_is_named_swap(vma))
            args.count = named_swap_same_file_pte_count(vma, address);
        mmap_read_unlock(current->mm);

        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;

        return 0;
    }

    case IOCTL_GET_PAGE_PROT: {
        struct page_prot_args args;
        struct vm_area_struct *vma;

        if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
            return -EFAULT;

        mmap_read_lock(current->mm);
        vma = vma_lookup(current->mm, (unsigned long)args.virtual_address);
        if (!vma) {
            mmap_read_unlock(current->mm);
            return -EINVAL;
        }

        /* Extract the actual kernel permissions */
        args.is_readable = !!(vma->vm_flags & VM_READ);
        args.is_writable = !!(vma->vm_flags & VM_WRITE);
        args.is_executable = !!(vma->vm_flags & VM_EXEC);
        
        mmap_read_unlock(current->mm);

        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
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
