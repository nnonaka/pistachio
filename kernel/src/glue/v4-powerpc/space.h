/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     glue/v4-powerpc/space.h
 * Description:   
 *                
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *                
 * $Id$
 *                
 ********************************************************************/

#ifndef __GLUE__V4_POWERPC__SPACE_H__
#define __GLUE__V4_POWERPC__SPACE_H__

#include <debug.h>
#include <asid.h>

#include INC_API(types.h)
#include INC_API(fpage.h)
#include INC_API(thread.h)

#if defined(CONFIG_PPC_MMU_SEGMENTS)
#include INC_ARCH(pgent-pghash.h)
#else
#include INC_ARCH(pgent-swtlb.h)
#include INC_ARCH(swtlb.h)
#define HAVE_ARCH_FREE_SPACE
#include INC_ARCH(softhvm.h)
#endif
#include INC_GLUE(hwspace.h)
#include INC_ARCH(atomic.h)

// Even if new MDB is not used we need the mdb_t::ctrl_t
#include <mdb.h>

//translation table (actual declaration in space.cc)
#define TRANSLATION_TABLE_ENTRIES 32
extern struct transtable_t {
	word_t s0addr;
	paddr_t physaddr;
	word_t size;
} transtable[TRANSLATION_TABLE_ENTRIES];

struct utcb_t; typedef struct utcb_t utcb_t;
struct tcb_t;  typedef struct tcb_t tcb_t;

/* space_t's access_e becomes the SPACE_ACCESS_* constants the shared
   api/v4/space.c already uses; it passes them as a plain int. */
#define SPACE_ACCESS_READ	0
#define SPACE_ACCESS_WRITE	1
#define SPACE_ACCESS_READWRITE	-1
#define SPACE_ACCESS_EXECUTE	2

struct space_t
{
    pgent_t pdir[1024];
    union {
	word_t raw[1024];
	struct {
	    word_t linknode[USER_AREA_SIZE >> PPC_PAGEDIR_BITS];
	    fpage_t kip_area;
	    fpage_t utcb_area;
	    atomic_t thread_count;
#ifdef CONFIG_X_PPC_SOFTHVM
	    bool hvm_mode;
#endif
	    struct {
		atomic_t thread_count;
#ifdef CONFIG_PPC_MMU_TLB
		asid_t asid;
		pgent_t cpulocal;
#endif
	    } cpu[CONFIG_SMP_MAX_CPUS];
	};
    };

};
typedef struct space_t space_t;

/* was the static member space_t::pinned_mapping */
extern word_t space_pinned_mapping;

BEGIN_DECLS
/* Declarations only; the definitions live in space.c / space-swtlb.c.  The
   names are fixed by the shared C callers in api/v4 and generic. */
void      space_init (space_t *self, fpage_t utcb_area, fpage_t kip_area);
void      space_free (space_t *self);
void      space_arch_free (space_t *self);
bool      space_sync_kernel_space (space_t *self, addr_t addr);
void      space_switch_to_kernel_space (cpuid_t cpu);
void      space_handle_pagefault (space_t *self, addr_t addr, addr_t ip, int access, bool kernel);
bool      space_is_initialized (space_t *self);
void      space_map_sigma0 (space_t *self, addr_t addr);
void      space_map_fpage (space_t *self, fpage_t snd_fp, word_t base, space_t *t_space, fpage_t rcv_fp, bool grant);
fpage_t   space_unmap_fpage (space_t *self, fpage_t fpage, bool flush, bool unmap_all);
fpage_t   space_mapctrl (space_t *self, fpage_t fpage, mdb_ctrl_t ctrl, word_t attribute, bool unmap_all);
void      space_allocate_tcb (space_t *self, addr_t addr);
void      space_map_dummy_tcb (space_t *self, addr_t addr);
utcb_t *  space_allocate_utcb (space_t *self, tcb_t *tcb);
bool      space_is_user_area_addr (addr_t addr);
/* generic/linear_ptab.h calls the unsuffixed name; glue/v4-x86 declares both. */
bool      space_is_user_area (addr_t addr);
bool      space_is_user_area_fpage (fpage_t fpage);
bool      space_is_kernel_area (addr_t addr);
bool      space_is_tcb_area (addr_t addr);
bool      space_is_copy_area (addr_t addr);
bool      space_is_mappable_addr (space_t *self, addr_t addr);
bool      space_is_mappable_fpage (space_t *self, fpage_t fpage);
word_t    space_space_control (space_t *self, word_t ctrl, fpage_t kip_area, fpage_t utcb_area, threadid_t redirector_tid);
/* flush_tlb's start/end defaulted to the whole address space; the shared
   callers use the two-argument form, so that is the C name, with the range
   variant spelled out separately. */
void      space_flush_tlb (space_t *self, space_t *curspace);
void      space_flush_tlb_range (space_t *self, space_t *curspace, addr_t start, addr_t end);
void      space_flush_tlbent (space_t *self, space_t *curspace, addr_t addr, word_t log2size);
paddr_t   space_sigma0_translate (addr_t addr, word_t size);
word_t    space_sigma0_attributes (pgent_t *pg, paddr_t addr, word_t size);
void      space_init_kernel_space (void);
void      space_init_kernel_mappings (space_t *self);
void      space_init_cpu_mappings (space_t *self, cpuid_t cpu);
addr_t    space_map_device (space_t *self, paddr_t paddr, word_t size, bool kernel, word_t attrib);
pgent_t * space_page_lookup (space_t *self, addr_t vaddr);
bool      space_lookup_mapping (space_t *self, addr_t vaddr, pgent_t **r_pg, word_t *r_size, cpuid_t cpu);
bool      space_readmem (space_t *self, addr_t vaddr, word_t *contents);
void      space_release_kernel_mapping (space_t *self, addr_t vaddr, paddr_t paddr, word_t log2size);
void      space_add_mapping (space_t *self, addr_t vaddr, paddr_t paddr, word_t size, bool writable, bool kernel, word_t attrib);
void      space_flush_mapping (space_t *self, addr_t vaddr, word_t size, pgent_t *pgent);
space_t * space_allocate_space (void);
void      space_free_space (space_t *space);
#ifdef CONFIG_PPC_MMU_TLB
addr_t    space_map_device_pinned (space_t *self, paddr_t paddr, word_t size, bool kernel, word_t attrib);
bool      space_handle_tlb_miss (space_t *self, addr_t lookup_vaddr, addr_t install_vaddr, bool user, bool global);
void      space_allocate_asid (space_t *self);
#endif
END_DECLS

/* Trivial statics, formerly inline in the class body. */
INLINE bool   space_is_arch_mappable (addr_t addr, size_t size)	{ return true; }
INLINE addr_t space_sign_extend (addr_t addr)			{ return addr; }
INLINE bool   space_does_tlbflush_pay (word_t log2size)		{ return log2size != POWERPC_PAGE_BITS; }
INLINE void   space_begin_update (void)				{ }
INLINE void   space_end_update (void)				{ }
INLINE word_t space_readmem_phys (paddr_t paddr)		{ return *(word_t *) phys_to_virt((void *)(word_t)paddr); }

INLINE space_t * space_vsid_to_space (word_t vsid)
{
    return (space_t *)( (vsid & 0xfffffff0) << (POWERPC_PAGE_BITS - 4) );
}


/**********************************************************************
 *
 *                 global declarations
 *
 ***********************************************************************/

extern void init_kernel_space();

INLINE space_t *get_kernel_space()
{
    extern space_t *kernel_space;
    return kernel_space;
}

/**********************************************************************
 *
 *                 inline functions
 *
 ***********************************************************************/

INLINE pgent_t * space_get_pdir (space_t *self)
{
    return self->pdir;
}

INLINE pgent_t * space_pgent_cpu (space_t *self, word_t num, word_t cpu)
{
    /* Was get_pdir()->next(this, size_4m, num).  pgent_next lives in
       pgent-swtlb_functions.h, which includes this header, so it is not
       declared yet at this point -- and for a page directory it is just
       pointer arithmetic, which is what pgent_next does. */
    return space_get_pdir (self) + num;
}

/* cpu defaulted to 0. */
INLINE pgent_t * space_pgent (space_t *self, word_t num)
{ return space_pgent_cpu (self, num, 0); }

INLINE bool space_is_kernel_paged_area (addr_t addr)
{
    return (addr > (addr_t)DEVICE_AREA_START &&
	    addr < (addr_t)DEVICE_AREA_END);
}

INLINE word_t space_get_copy_limit (space_t *self, addr_t addr, word_t len)
{
    word_t end = (word_t)addr + len;

    if( space_is_user_area_addr(addr) )
    {
	if( end >= USER_AREA_END )
	    return (USER_AREA_END - (word_t)addr);
    }
    else
    {
	ASSERT( space_is_copy_area(addr) );
	word_t max;
	max = COPY_AREA_SIZE - ((word_t)addr - COPY_AREA_START);
	if( len > max )
	    return max;
    }

    return len;
}

INLINE fpage_t space_get_kip_page_area (space_t *self)
{
    return self->kip_area;
}

INLINE fpage_t space_get_utcb_page_area (space_t *self)
{
    return self->utcb_area;
}

INLINE word_t space_get_from_user (space_t *self, addr_t addr)
{
    return *(word_t *)(addr);
}

#ifdef CONFIG_PPC_MMU_SEGMENTS
INLINE word_t space_get_vsid (space_t *self, addr_t addr)
{
    // TODO: get_vsid() needs optimisation
    ppc_segment_t seg;
    if( (word_t)addr > KERNEL_OFFSET )
	seg = space_get_segment_id (get_kernel_space());
    else
	seg = space_get_segment_id (self);
    return seg.raw | ((word_t)addr >> 28);
}

INLINE ppc_segment_t space_get_segment_id (space_t *self)
{
    ppc_segment_t seg;
    seg.raw = ((word_t)self >> POWERPC_PAGE_BITS) << 4;
    return seg;
}
#elif defined(CONFIG_PPC_MMU_TLB)
INLINE asid_manager_t *get_asid_manager (void)
{
    extern asid_manager_t asid_manager;
    return &asid_manager;
}

INLINE asid_t *space_get_asid_cpu (space_t *self, cpuid_t cpu)
{
#if defined(CONFIG_X_PPC_SOFTHVM)
    return self->hvm_mode ? NULL : &self->cpu[cpu].asid;
#else
    return &self->cpu[cpu].asid;
#endif
}
#endif

/**
 * add a thread to the space
 * @param tcb	pointer to thread control block
 */
INLINE void space_add_tcb (space_t *self, tcb_t * tcb, cpuid_t cpu)
{
    atomic_inc (&self->thread_count);
#ifdef CONFIG_SMP
    atomic_inc (&self->cpu[cpu].thread_count);
#endif
}

/**
 * remove a thread from a space
 * @param tcb	thread control block
 * @return	true if it was the last thread
 */
INLINE bool space_remove_tcb (space_t *self, tcb_t * tcb, cpuid_t cpu)
{
    ASSERT (atomic_read (&self->thread_count) != 0);
#ifdef CONFIG_SMP
    atomic_dec (&self->cpu[cpu].thread_count);
#endif
    atomic_dec (&self->thread_count);
    return atomic_read (&self->thread_count) == 0;
}

INLINE void space_move_tcb (space_t *self, tcb_t * tcb, cpuid_t src_cpu, cpuid_t dst_cpu)
{
    atomic_inc (&self->cpu[dst_cpu].thread_count);
    atomic_dec (&self->cpu[src_cpu].thread_count);
}

#if defined(CONFIG_PPC_MMU_TLB)
void setup_kernel_mappings( void );
void install_exception_handlers( cpuid_t cpu );
#endif

#endif /* __GLUE__V4_POWERPC__SPACE_H__ */
