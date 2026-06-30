#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x49127e8a, "misc_deregister" },
	{ 0x13c49cc2, "_copy_from_user" },
	{ 0xf835edfa, "const_pcpu_hot" },
	{ 0x6b547574, "__tracepoint_mmap_lock_start_locking" },
	{ 0x668b19a1, "down_read" },
	{ 0x6be1a070, "__tracepoint_mmap_lock_acquire_returned" },
	{ 0x55c92c84, "find_vma" },
	{ 0x8427cc7b, "_raw_spin_lock_irq" },
	{ 0xa85a3e6d, "xa_load" },
	{ 0x4b750f53, "_raw_spin_unlock_irq" },
	{ 0x62728e5e, "__tracepoint_mmap_lock_released" },
	{ 0x53b954a2, "up_read" },
	{ 0x8239dd01, "get_user_pages_fast" },
	{ 0xbcb36fe4, "hugetlb_optimize_vmemmap_key" },
	{ 0xb59b264e, "rmap_walk" },
	{ 0x587f22d7, "devmap_managed_key" },
	{ 0x6b10bee1, "_copy_to_user" },
	{ 0xe3906bbe, "folio_get_anon_vma" },
	{ 0xc372c45a, "mem_cgroup_from_task" },
	{ 0xf352023f, "memory_cgrp_subsys_enabled_key" },
	{ 0x4c03a563, "random_kmalloc_seed" },
	{ 0x1b1f880a, "kmalloc_caches" },
	{ 0x210e101f, "__kmalloc_cache_noprof" },
	{ 0x037a0cba, "kfree" },
	{ 0x72d79d83, "pgdir_shift" },
	{ 0xef45223d, "boot_cpu_data" },
	{ 0x1992a7d5, "named_swap_same_file_pte_count" },
	{ 0xb21dcc7d, "named_swap_file_path" },
	{ 0xa916b694, "strnlen" },
	{ 0x476b165a, "sized_strscpy" },
	{ 0x331cae51, "__mmap_lock_do_trace_released" },
	{ 0x57b05cf6, "__mmap_lock_do_trace_acquire_returned" },
	{ 0xeca12fc3, "__mmap_lock_do_trace_start_locking" },
	{ 0x1d19f77b, "physical_mask" },
	{ 0x8a35b432, "sme_me_mask" },
	{ 0xdad13544, "ptrs_per_p4d" },
	{ 0xfb02104a, "pv_ops" },
	{ 0xd4ec10e6, "BUG_func" },
	{ 0x7cd8d75e, "page_offset_base" },
	{ 0x97651e6c, "vmemmap_base" },
	{ 0xba8fbd64, "_raw_spin_lock" },
	{ 0xb5b54b34, "_raw_spin_unlock" },
	{ 0x59bd8145, "__put_devmap_managed_folio_refs" },
	{ 0xe6817947, "__put_anon_vma" },
	{ 0x7f72e4ba, "__folio_put" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x19dee613, "__fortify_panic" },
	{ 0xbdfb6dbb, "__fentry__" },
	{ 0x5b8239ca, "__x86_return_thunk" },
	{ 0xb2523578, "misc_register" },
	{ 0x122c3a7e, "_printk" },
	{ 0xe94fd8fd, "module_layout" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "7F2B6125777892F1626327F");
