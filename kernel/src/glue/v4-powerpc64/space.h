/****************************************************************************
 *
 * Copyright (C) 2003,  National ICT Australia (NICTA)
 *
 * File path:	glue/v4-powerpc64/space.h
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
 * $Id: space.h,v 1.15 2006/11/14 18:44:56 skoglund Exp $
 *
 ***************************************************************************/

#ifndef __GLUE__V4_POWERPC64__SPACE_H__
#define __GLUE__V4_POWERPC64__SPACE_H__

#include <debug.h>

#include INC_ARCH(vsid_asid.h)
#include INC_ARCH(pghash.h)

#include INC_API(types.h)	/* for cpuid_t */
#include INC_API(fpage.h)
#include INC_API(thread.h)

#include INC_GLUE(pgent.h)
#include INC_GLUE(hwspace.h)

#if CONFIG_POWERPC64_STAB
#include INC_ARCH(stab.h)

#define HAVE_ARCH_FREE_SPACE
#endif

// Even if new MDB is not used we need the mdb_t::ctrl_t
#include <mdb.h>

//translation table (actual declaration in space.cc)
#define TRANSLATION_TABLE_ENTRIES 32
extern struct transTable_t {
	word_t s0addr;
	paddr_t physaddr;
	word_t size;
} transTable[TRANSLATION_TABLE_ENTRIES];

struct utcb_t;
typedef struct utcb_t utcb_t;
struct tcb_t;
typedef struct tcb_t tcb_t;

/* space_t::access_e */
#define SPACE_ACCESS_READ	0
#define SPACE_ACCESS_WRITE	1
#define SPACE_ACCESS_READWRITE	(-1)
#define SPACE_ACCESS_EXECUTE	2

struct space_t
{
    /* In order to provide an almost full 64-bit user address space,
     * space_t is 1024 bytes (minimum kalloc size). */
    union {
	pgent_t pdir[64];	/* 6-bits of top level page table */
	struct {
	    pgent_t user[64];
	} map;
    };
    union {
	word_t general[64];	/* 64 general purpose locations */
	struct {
	    /* If you add variables to this area, subtract corresponding
	     * space from the resv[] region.
	     */
	    fpage_t kip_area;
	    fpage_t utcb_area;
	    word_t  thread_count;
	    vsid_asid_t vsid_asid;  /* 9-bit ASID - shifted to fit VSID */

#if CONFIG_POWERPC64_SLB
	    /* XXX - we don't use this yet, we just do random replacement */
	    u64_t   slb_bitmap;	    /* Segment lookaside buffer usage bitmap */
	    word_t  segments[48];
	    word_t  resv[64-48-5];
#elif CONFIG_POWERPC64_STAB

	    ppc64_stab_t segment_table;
	    word_t  resv[64-5];
#endif
	} x;
    };
};
typedef struct space_t space_t;


BEGIN_DECLS

/* glue -- out of line in glue/v4-powerpc64/space.c */
void	 space_init (space_t *self, fpage_t utcb_area, fpage_t kip_area);
void	 space_free (space_t *self);
/* access is space_t::access_e, a signed int enum, so it is passed as int --
   api/v4/space.c defines this one and spells it that way. */
void	 space_handle_pagefault (space_t *self, addr_t addr, addr_t ip,
				 int access, bool kernel);
bool	 space_is_initialized (space_t *self);
void	 space_map_sigma0 (space_t *self, addr_t addr);
void	 space_map_fpage (space_t *self, fpage_t snd_fp, word_t base,
			  space_t *t_space, fpage_t rcv_fp, bool grant);
fpage_t	 space_unmap_fpage (space_t *self, fpage_t fpage, bool flush,
			    bool unmap_all);
fpage_t	 space_mapctrl (space_t *self, fpage_t fpage, mdb_ctrl_t ctrl,
			word_t attribute, bool unmap_all);
void	 space_allocate_tcb (space_t *self, addr_t addr);
void	 space_map_dummy_tcb (space_t *self, addr_t addr);
utcb_t * space_allocate_utcb (space_t *self, tcb_t *tcb);

bool	 space_is_user_area (addr_t addr);
bool	 space_is_user_area_fpage (fpage_t fpage);
bool	 space_is_kernel_area (addr_t addr);
bool	 space_is_tcb_area (addr_t addr);
bool	 space_is_copy_area (addr_t addr);
bool	 space_is_mappable (space_t *self, addr_t addr);
bool	 space_is_mappable_fpage (space_t *self, fpage_t fpage);

/* powerpc64 specific */
void	 space_init_kernel_mappings (space_t *self);
bool	 space_handle_hash_miss (space_t *self, addr_t vaddr);
bool	 space_handle_protection_fault (space_t *self, addr_t vaddr, bool dsi);
bool	 space_handle_segment_miss (space_t *self, addr_t vaddr);
bool	 space_lookup_mapping (space_t *self, addr_t vaddr, pgent_t **r_pg,
			       pgsize_e *r_size, cpuid_t cpu);
/* cpu defaulted to 0; the _c form is what the callers that took the default
   use, as in glue/v4-powerpc/space.h. */
bool	 space_lookup_mapping_c (space_t *self, addr_t vaddr, pgent_t **r_pg,
				 pgsize_e *r_size);
bool	 space_readmem (space_t *self, addr_t vaddr, word_t *contents);
void	 space_release_kernel_mapping (space_t *self, addr_t vaddr,
				       addr_t paddr, word_t log2size);
void	 space_add_mapping (space_t *self, addr_t vaddr, addr_t paddr,
			    bool writable, bool executable, bool kernel,
			    pgsize_e size);

extern void init_kernel_space (void);

END_DECLS

/* Bodies that were inline in the class and depend on nothing but their
   arguments. */
INLINE bool   space_sync_kernel_space (space_t *self, addr_t addr)	{ return false; }
INLINE bool   space_is_arch_mappable (addr_t addr, size_t size)		{ return true; }
INLINE addr_t space_sign_extend (addr_t addr)				{ return addr; }
INLINE bool   space_does_tlbflush_pay (word_t log2size)
{ return log2size != POWERPC64_PAGE_BITS; }
INLINE void   space_begin_update (void)					{ }
INLINE void   space_end_update (void)					{ }
INLINE word_t space_space_control (space_t *self, word_t ctrl, fpage_t kip_area,
				   fpage_t utcb_area, threadid_t redirector_tid)
{ return 0; }
INLINE paddr_t space_sigma0_translate (addr_t addr, pgsize_e size)
{ return (paddr_t) (word_t) addr; }
INLINE word_t space_sigma0_attributes (pgent_t *pg, addr_t addr, pgsize_e size)
{ return 0; }
INLINE word_t space_readmem_phys (addr_t paddr)
{ return *phys_to_virt((word_t*)paddr); }

INLINE space_t *get_kernel_space(void)
{
    extern space_t *kernel_space;
    return kernel_space;
}

INLINE space_t *space_lookup_space (word_t vsid)
{
    return vsid_asid_cache_lookup (get_vsid_asid_cache(), vsid);
}

/**********************************************************************
 *
 *                 inline functions
 *
 ***********************************************************************/

INLINE pgent_t * space_get_pdir (space_t *self) { return self->pdir; }

INLINE pgent_t * space_pgent_cpu (space_t *self, word_t num, word_t cpu)
{
    /* Was get_pdir()->next(this, size_max, num).  pgent_next lives in
       pgent_inline.h, which includes this header, so it is not declared yet
       at this point -- and for a page directory it is just pointer
       arithmetic, which is all pgent_next does.  Same resolution as
       glue/v4-powerpc/space.h. */
    return space_get_pdir (self) + num;
}

INLINE pgent_t * space_pgent (space_t *self, word_t num)
{
    return space_pgent_cpu (self, num, 0);
}

INLINE bool space_is_cpu_area (addr_t addr)
{
    return (addr >= (addr_t)CPU_AREA_START &&
	    addr < (addr_t)CPU_AREA_END);
}

INLINE word_t space_get_copy_limit (space_t *self, addr_t addr, word_t len)
{
    word_t end = (word_t)addr + len;

    if( space_is_user_area (addr) )
    {
	if( end >= USER_AREA_END )
	    return (USER_AREA_END - (word_t)addr);
    }
    else
    {
	ASSERT( space_is_copy_area (addr) );
	{
	    word_t max = COPY_AREA_SIZE - ((word_t)addr - COPY_AREA_START);
	    if( len > max )
		return max;
	}
    }

    return len;
}

INLINE fpage_t space_get_kip_page_area (space_t *self)
{
    return self->x.kip_area;
}

INLINE fpage_t space_get_utcb_page_area (space_t *self)
{
    return self->x.utcb_area;
}

INLINE word_t space_get_from_user (space_t *self, addr_t addr)
{
    return *(word_t *)(addr);
}

INLINE word_t space_get_vsid_asid (space_t *self)
{
    return vsid_asid_get (&self->x.vsid_asid, self);
}

INLINE word_t space_get_vsid (space_t *self, addr_t addr)
{
    word_t vsid;

    /* Kernel VSID = 0 */
    if( (word_t)addr >= USER_AREA_END)
	vsid = 0;
    else
	vsid = vsid_asid_get (&self->x.vsid_asid, self) >> 12;

    /* Add VSID and ASID */
    return vsid | (((word_t)addr >> POWERPC64_SEGMENT_BITS) & ((1ul << CONFIG_POWERPC64_ESID_BITS)-1));
}

/**
 * adds a thread to the space
 * @param tcb pointer to thread control block
 */
INLINE void space_add_tcb (space_t *self, tcb_t * tcb, cpuid_t cpu)
{
    self->x.thread_count ++;
}

/**
 * removes a thread from a space
 * @param tcb_t thread control block
 * @return true if it was the last thread
 */
INLINE bool space_remove_tcb (space_t *self, tcb_t * tcb, cpuid_t cpu)
{
    ASSERT(self->x.thread_count != 0);
    self->x.thread_count --;
    return (self->x.thread_count == 0);
}

INLINE void space_flush_tlb (space_t *self, space_t *curspace)
{
    // TODO: flush the tlb for a given address space.
    ppc64_invalidate_tlb();
}

INLINE void space_flush_tlbent (space_t *self, space_t *curspace, addr_t addr,
				word_t log2size)
{
    ppc64_invalidate_tlbe( addr, (log2size == POWERPC64_PAGE_BITS) ? 0 : 1 );
}

INLINE void space_add_4k_mapping (space_t *self, addr_t vaddr, addr_t paddr,
				  bool writable, bool kernel)
{
    space_add_mapping( self, vaddr, paddr, writable, true, kernel, size_4k );
}

INLINE void space_add_4k_mapping_noexecute (space_t *self, addr_t vaddr,
					    addr_t paddr, bool writable,
					    bool kernel)
{
    space_add_mapping( self, vaddr, paddr, writable, false, kernel, size_4k );
}

INLINE void space_add_large_mapping (space_t *self, addr_t vaddr, addr_t paddr,
				     bool writable, bool kernel)
{
#ifdef CONFIG_POWERPC64_LARGE_PAGES
    space_add_mapping( self, vaddr, paddr, writable, true, kernel, size_16m );
#else
    ASSERT(!"No large page support");
#endif
}

#if CONFIG_POWERPC64_STAB
INLINE ppc64_stab_t *space_get_seg_table (space_t *self)
{ return &self->x.segment_table; }
#endif
#if CONFIG_POWERPC64_SLB
INLINE word_t space_get_segment (space_t *self, int i) { return self->x.segments[i]; }
INLINE void space_set_segment (space_t *self, int i, word_t val) { self->x.segments[i] = val; }
#endif

#if defined(HAVE_ARCH_FREE_SPACE)

INLINE void space_arch_free (space_t *self)
{
#if CONFIG_POWERPC64_STAB
    ppc64_stab_free (&self->x.segment_table);
#endif
}

#endif

#endif /* __GLUE__V4_POWERPC64__SPACE_H__ */
