/*********************************************************************
 *                
 * Copyright (C) 2007-2010,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/space.h
 * Description:   
 *                
 * @LICENSE@
 *                
 * $Id:$
 *                
 ********************************************************************/
#ifndef __GLUE__V4_X86__SPACE_H__
#define __GLUE__V4_X86__SPACE_H__

#include INC_GLUE_SA(space.h)
#if defined(CONFIG_X_EVT_LOGGING)
#include INC_GLUE(logging.h)
#endif

extern cpuid_t current_cpu;

struct space_t {
    /* space_t adds no data of its own; single inheritance from x86_space_t
       becomes base-as-first-member in C (identical layout, no vtable). */
    x86_space_t base;
};
typedef struct space_t space_t;

#if defined(CONFIG_X86_COMPATIBILITY_MODE)
INLINE bool space_is_compatibility_mode (space_t *self)
{
    return x86_space_is_compatibility_mode (&self->base);
}
#endif


/* C entry points for the copy-area / per-CPU pdir methods that resources.c
   needs (space_t stays a C++ class; these wrap the methods, defined in
   space.cc). get_top_pdir_phys returns word_t so C need not know x86_pgent_t. */
BEGIN_DECLS
void   space_populate_copy_area (space_t *self, word_t n, tcb_t *tcb, space_t *partner, cpuid_t cpu);
void   space_delete_copy_area (space_t *self, word_t n, cpuid_t cpu);
word_t space_get_top_pdir_phys (space_t *self, cpuid_t cpu);
struct x86_top_pdir_t * space_get_top_pdir (space_t *self, cpuid_t cpu);
void   space_alloc_cpu_top_pdir (space_t *self, cpuid_t cpu);
bool   space_has_cpu_top_pdir (space_t *self, cpuid_t cpu);
END_DECLS

/* C wrappers for the space_t methods that generic/linear_ptab_walker.c drives
   (space_t stays a C++ class); defined in space.cc. pgsize args are word_t
   holding an X86_PGSIZE_* value. */
BEGIN_DECLS
pgent_t * space_pgent (space_t *self, word_t num);
pgent_t * space_pgent_cpu (space_t *self, word_t num, word_t cpu);
void      space_begin_update (void);
void      space_end_update (void);
bool      space_is_mappable_addr (space_t *self, addr_t addr);
bool      space_is_mappable_fpage (space_t *self, fpage_t fp);
bool      space_does_tlbflush_pay (word_t log2size);
fpage_t   space_get_kip_page_area (space_t *self);
fpage_t   space_get_utcb_page_area (space_t *self);
paddr_t   space_sigma0_translate (addr_t addr, word_t size);
word_t    space_sigma0_attributes (pgent_t *pg, addr_t addr, word_t size);
word_t    space_readmem_phys (addr_t paddr);
void      space_release_kernel_mapping (space_t *self, addr_t vaddr, addr_t paddr, word_t log2size);
void      space_flush_tlb (space_t *self, space_t *curspace);
void      space_flush_tlbent (space_t *self, space_t *curspace, addr_t vaddr, word_t log2size);
space_t * get_current_space_c (void);
/* space_t methods driven by api/v4/space.c (handle_pagefault/free/syscalls). */
void      space_map_sigma0 (space_t *self, addr_t addr);
bool      space_sync_kernel_space (space_t *self, addr_t addr);
bool      space_is_initialized (space_t *self);
void      space_allocate_tcb (space_t *self, addr_t addr);
void      space_map_dummy_tcb (space_t *self, addr_t addr);
word_t    space_space_control (space_t *self, word_t ctrl, fpage_t kip_area, fpage_t utcb_area, threadid_t redir);
void      space_init (space_t *self, fpage_t utcb_area, fpage_t kip_area);
void      space_arch_free (space_t *self);
fpage_t   space_unmap_fpage (space_t *self, fpage_t fpage, bool flush, bool unmap_all);
/* space_map_fpage is the asm-name of space_t::map_fpage (no wrapper needed). */
void      space_map_fpage (space_t *self, fpage_t snd_fp, word_t base, space_t *t_space, fpage_t rcv_fp, bool grant);
word_t    space_get_copy_limit (space_t *self, addr_t addr, word_t limit);
u8_t      space_get_from_user (space_t *self, addr_t addr);
bool      space_is_tcb_area (addr_t addr);
bool      space_is_user_area (addr_t addr);
void      reload_user_segregs_c (void);
space_t * get_kernel_space_c (void);
void      space_init_kernel_space (void);
void      space_init_cpu_mappings (space_t *self, cpuid_t cpu);
void      space_remap_area (space_t *self, addr_t vaddr, addr_t paddr, word_t pgsize, word_t len, bool writable, bool kernel, bool global);
void      space_add_mapping (space_t *self, addr_t vaddr, addr_t paddr, word_t size, bool writable, bool kernel, bool global, bool cacheable);
/* space_readmem is the asm-name of space_t::readmem (no wrapper needed). */
bool      space_readmem (space_t *self, addr_t vaddr, word_t *contents);
bool      space_is_copy_area (addr_t addr);
/* tcb reference-counting / utcb allocation for api/v4/thread.c. */
/*
 * IO permission bitmap and IO space.  These were members of space_t, removed
 * from this header by 312b160 along with the rest of the __cplusplus block;
 * their definitions went from space.cc in 49fab2d.  Both passes verified
 * against a config with CONFIG_X86_IO_FLEXPAGES off, which compiles none of
 * this.  Restored in C -- see notes §116.
 *
 * get_io_bitmap's `cpuid_t cpu = current_cpu' default is dropped, as it was
 * for space_add_tcb/space_remove_tcb above; callers pass current_cpu.
 */
#if defined(CONFIG_X86_IO_FLEXPAGES)
addr_t      space_install_io_bitmap (space_t *self, bool create);
void        space_free_io_bitmap (space_t *self);
bool        space_sync_io_bitmap (space_t *self);
addr_t      space_get_io_bitmap (space_t *self, cpuid_t cpu);
void        space_set_io_space (space_t *self, io_space_t *n);
io_space_t * space_get_io_space (space_t *self);
void        init_io_space (void);
#endif

void      space_add_tcb (space_t *self, tcb_t *tcb, cpuid_t cpu);
bool      space_remove_tcb (space_t *self, tcb_t *tcb, cpuid_t cpu);
void      space_move_tcb (space_t *self, tcb_t *tcb, cpuid_t src_cpu, cpuid_t dst_cpu);
utcb_t *  space_allocate_utcb (space_t *self, tcb_t *tcb);
void      space_switch_to_kernel_space (cpuid_t cpu);
space_t * space_allocate_space (void);
void      space_free_space (space_t *space);
void      align_memregion (struct mem_region_t *region, word_t size);
/* lookup_mapping stays C++ (its out-param is a 4-byte pgsize_e; a word_t-writing
   C symbol would corrupt the many external callers). This wrapper bridges it for
   linear_ptab_walker.c's readmem, writing the page size as a word_t. */
bool      space_lookup_mapping_c (space_t *self, addr_t vaddr, pgent_t **r_pg, word_t *r_size);
/* r_size is a 4-byte pgsize_e on the C++ side -- hence int*, not word_t*. */
bool      space_lookup_mapping (space_t *self, addr_t vaddr, pgent_t **r_pg, int *r_size, cpuid_t cpu);
END_DECLS


#endif /* !__GLUE__V4_X86__SPACE_H__ */
