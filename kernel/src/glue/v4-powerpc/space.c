/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     glue/v4-powerpc/space.cc
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

#include INC_API(fpage.h)	/* fpage_is_addr_in_fpage */
#include <debug.h>
#include <kmemory.h>
#include <generic/lib.h>
#include <linear_ptab.h>
#include <kdb/tracepoints.h>

#include INC_API(tcb.h)
#include INC_API(kernelinterface.h)

#include INC_GLUE(space.h)
#include INC_GLUE(pghash.h)
/* swtlb.h reaches SPR_PID and SPR_MMUCR, which ppc_registers.h defines only
   inside CONFIG_PPC_BOOKE; upstream includes it unconditionally, so a classic
   PowerPC build fails in it before reaching anything in this file.  Guarded to
   the MMU variant that has a software TLB.  Notes §144. */
#ifdef CONFIG_PPC_MMU_TLB
#include INC_ARCH(swtlb.h)
#endif

DECLARE_TRACEPOINT(hash_miss_cnt);
DECLARE_TRACEPOINT(hash_insert_cnt);
DECLARE_TRACEPOINT(ptab_4k_map_cnt);

DECLARE_KMEM_GROUP(kmem_utcb);
EXTERN_KMEM_GROUP(kmem_pgtab);
EXTERN_KMEM_GROUP(kmem_space);
/* Only space_allocate_tcb below uses it, and only when tcbs are dynamic. */
EXTERN_KMEM_GROUP(kmem_tcb);

#define TRACE_SPACE(x...)
//#define TRACE_SPACE(x...)	TRACEF(x)

space_t *kernel_space = NULL;

//translation table
struct transtable_t transtable[TRANSLATION_TABLE_ENTRIES];

void space_allocate_tcb (space_t *self, addr_t addr)
{
#if !defined(CONFIG_STATIC_TCBS)
    /* Remove the dummy tcb mapping. We could have a dummy tcb
     * mapping due to someone sending invalid parameters to system
     * calls
     */
    pgent_t *pgent = space_page_lookup (kernel_space,  addr );
    if( pgent && pgent_is_valid (pgent, self, size_4k) )
	space_flush_mapping (kernel_space,  addr, size_4k, pgent );

    addr_t page = kmem_alloc(&kmem,  kmem_tcb, POWERPC_PAGE_SIZE );
    ASSERT(page);

    TRACE_SPACE( "new tcb, kmem virt %p, phys %p, tcb virt %p\n", page, 
	         virt_to_phys(page), addr);
    space_add_mapping (kernel_space,  addr, virt_to_phys((paddr_t) page), size_4k,
		       true, true, cache_standard);

    /* was the implicit-this sync_kernel_space(addr). */
    space_sync_kernel_space (self, addr);
#endif
}

void space_map_dummy_tcb (space_t *self, addr_t addr)
{
#if !defined(CONFIG_STATIC_TCBS)
    space_add_mapping (self, addr, virt_to_phys((paddr_t) get_dummy_tcb_c ()),
		       size_4k, false, true, cache_standard);
#endif
}

void space_add_mapping (space_t *self, addr_t vaddr, paddr_t paddr, word_t size, bool writable, bool kernel, word_t attrib)
{
    pgent_t * pg = space_pgent (self, page_table_index (size_max, vaddr));
    word_t pgsize = size_max;

    ASSERT(is_page_size_valid(size));

    /* Lookup mapping */
    while (pgsize > size)
    {
	if (!pgent_is_valid (pg, self, pgsize))
	    pgent_make_subtree (pg, self, pgsize, kernel);
	else
	{
	    ASSERT(pgent_is_subtree (pg, self, pgsize) );
	}

	pg = pgent_next (pgent_subtree (pg, self, pgsize), self,
			 pgsize-1, page_table_index (pgsize-1, vaddr));
	pgsize--;
    }

    /* Modify page table */
    pgent_set_entry (pg, self, pgsize, paddr, writable ? 7 : 5, attrib, kernel);

#ifdef CONFIG_PPC_MMU_SEGMENTS
    ASSERT(pgsize == size_4k);
    /* Upstream passes `pgent', which this function has no such name for --
       the leaf entry the walk above ends on is `pg', and it is what
       pgent_set_entry has just written.  `this' is `self'. */
    pghash_insert_4k_mapping (get_pghash(), self, vaddr, pg);
#endif
}

#if 0
void space_add_4k_mapping (space_t *self, addr_t vaddr, paddr_t paddr, bool writable, bool kernel, word_t attrib)
{
    pgent_t *pgent = space_pgent (self,  page_table_index(size_4m, vaddr) );
    if( !pgent_is_valid (pgent, self, size_4m))
	pgent_make_subtree (pgent, self, size_4m, kernel );

    pgent = pgent_next (pgent_subtree (pgent, self, size_4m), self,
	    size_4k, page_table_index(size_4k, vaddr) );

    pgent_set_entry (pgent, self, size_4k, paddr, writable ? 7 : 5, 
		      attrib, kernel);

#ifdef CONFIG_PPC_MMU_SEGMENTS
    pghash_insert_4k_mapping (get_pghash(), self, vaddr, pgent);
#endif
}
#endif

void space_flush_mapping (space_t *self, addr_t vaddr, word_t pgsize, pgent_t *pgent)
{
#ifdef CONFIG_PPC_MMU_SEGMENTS
    ASSERT(pgsize == size_4k);
    pghash_flush_4k_mapping (get_pghash(), self, vaddr, pgent);
#endif
    ppc_invalidate_tlbe( vaddr );
}

pgent_t * space_page_lookup (space_t *self, addr_t vaddr)
{
    pgent_t *pgent = space_pgent (self, page_table_index(size_4m, vaddr));
    if( !pgent_is_valid (pgent, self, size_4m) )
	return NULL;

    pgent = pgent_subtree (pgent, self, size_4m );
    pgent = pgent_next (pgent, self, size_4k, 
	    page_table_index(size_4k, vaddr) );
    return pgent;
}

/**********************************************************************
 *                               Allocation
 **********************************************************************/

space_t * space_allocate_space (void)
{
    space_t * space = (space_t*)kmem_alloc(&kmem, kmem_space, sizeof(space_t));
    ASSERT(space);
    return space;
}

void space_free_space (space_t *space)
{
    kmem_free(&kmem, kmem_space, (addr_t)space, sizeof(space_t));
}

/**********************************************************************
 *
 *                         System initialization 
 *
 **********************************************************************/

#if (KERNEL_PAGE_SIZE != POWERPC_PAGE_SIZE)
# error invalid kernel page size - please adapt
#endif

void SECTION(".init.memory") space_init_kernel_space (void)
{
    kernel_space = space_allocate_space();
    space_init_kernel_mappings (kernel_space);
}

addr_t space_map_device (space_t *self, paddr_t paddr, word_t size, bool kernel, word_t attrib)
{
    addr_t start_addr = (addr_t)(DEVICE_AREA_START + DEVICE_AREA_BAT_SIZE);

    /* Ensure that the size is a multiple of the page size. */
    if( size % POWERPC_PAGE_SIZE )
	size = (size + POWERPC_PAGE_SIZE) & POWERPC_PAGE_MASK;

    /* Search for the first available page.
     */
    while( 1 )
    {
	// Look for a 2nd level page table.
	word_t pdir_idx = page_table_index( size_4m, start_addr );
	pgent_t *pgent = space_pgent (self,  pdir_idx );
	if( !pgent_is_valid (pgent, self, size_4m) )
	    goto found;

	// Move to the starting position in the 2nd level page table.
	pgent = pgent_subtree (pgent, self, size_4m );
	word_t ptab_idx = page_table_index( size_4k, start_addr );
	pgent = pgent_next (pgent, self, size_4k, ptab_idx );

	// Search the page table for an unused entry.
	do {
	    if( !pgent_is_valid (pgent, self, size_4k) )
		goto found;

	    // Increment the address to the next page.
	    start_addr = addr_offset( start_addr, POWERPC_PAGE_SIZE );
	    if( ((word_t)start_addr + size) > DEVICE_AREA_END )
		return NULL;

	    // Move to the next page table entry.
	    pgent = pgent_next (pgent, self, size_4k, 1 );
	    ptab_idx++;
	} while( ptab_idx < POWERPC_PAGE_SIZE/sizeof(pgent_t) );
    }

found:

    /* TODO: flush the cache for the mapped region (to avoid cache paradoxes)!
     */

    // Add 4k device mappings for the entire region.
    TRACE_SPACE( "device mapping 0x%x --> 0x%x, size 0x%x\n", 
	          paddr, start_addr, size );

    for( word_t page = 0; page < size; page += POWERPC_PAGE_SIZE ) 
    {
	space_add_mapping (self,  addr_offset(start_addr, page),
		paddr + page, size_4k, true, kernel, attrib );
    }

    return start_addr;
}

/**********************************************************************
 *
 *                    space_t implementation
 *
 **********************************************************************/


void space_release_kernel_mapping (space_t *self, addr_t vaddr, paddr_t paddr, word_t log2size)
{
    // Free up memory used for UTCBs
    fpage_t utcb_page_area = space_get_utcb_page_area (self);

    if (fpage_is_addr_in_fpage (&utcb_page_area, vaddr))
	kmem_free(&kmem, kmem_utcb, (addr_t) phys_to_virt (paddr), 1UL << log2size);
}

utcb_t *space_allocate_utcb (space_t *self, tcb_t *tcb)
{
    ASSERT(tcb);
    addr_t utcb = (addr_t)tcb_get_utcb_location (tcb);
    addr_t page;

    pgent_t *pgent = space_page_lookup (self,  utcb );
    if( pgent && pgent_is_valid (pgent, self, size_4k) )
	// Already a valid page mapped at the UTCB address.
	page = (addr_t) phys_to_virt( pgent_address (pgent, self, size_4k) );
    else
    {
	// Allocate a new UTCB page.
	page = kmem_alloc(&kmem,  kmem_utcb, POWERPC_PAGE_SIZE );
	if( page == NULL )
	{
	    WARNING( "out of memory!\n" );
	    return NULL;
	}
	space_add_mapping (self, utcb, (paddr_t)virt_to_phys(page), size_4k, true, false, cache_standard);
    }

    return (utcb_t *)addr_offset( page, (word_t)utcb & ~POWERPC_PAGE_MASK );
}

void space_map_sigma0 (space_t *self, addr_t addr)
{
    ASSERT( 
	    ((addr < get_kip()->reserved_mem0.low) 
	     || (addr >= get_kip()->reserved_mem0.high))
    	    && 
	    ((addr < get_kip()->reserved_mem1.low) 
	     || (addr >= get_kip()->reserved_mem1.high)) 
	    );

    space_add_mapping (self, addr, (paddr_t)addr, size_4k, true, false, cache_standard);
}

word_t space_space_control (space_t *self, word_t ctrl, fpage_t kip_area, fpage_t utcb_area, threadid_t redirector_tid)
{
    word_t oldctrl = 0;
    paddr_t physaddr;
    int i;
#ifdef CONFIG_X_PPC_SOFTHVM
    oldctrl |= self->hvm_mode ? 1 : 0;

    /* XXX: HVM mode can only be changed when space is uninitialized */
    if ((ctrl & 1) && !self->hvm_mode) 
    {
	TRACEF("Enabling HVM mode\n");
	self->hvm_mode = true;
    }
#endif
    if (ctrl & (1 << 29)) {
    	for (i = 0; i < TRANSLATION_TABLE_ENTRIES; ++i) {
    		if (transtable[i].size > 0)
    			continue;
        	oldctrl |= 1 << 29;
        	physaddr = threadid_get_raw (&redirector_tid);
        	physaddr <<= 32;
        	physaddr |= utcb_area.raw;
        	transtable[i].physaddr = physaddr;
        	transtable[i].s0addr = kip_area.mem.x.base << 10;
        	transtable[i].size = 1UL << kip_area.mem.x.size;
        	break;
    	}
    }
    return oldctrl;
}


/**********************************************************************
 *
 *   C entry points that api/v4 and generic code call.  The C++ class
 *   declared most of these and defined none of them -- space_t::is_user_area
 *   and friends have no definition anywhere in the tree's history, which is
 *   one reason this port has never linked.  They are written here against the
 *   address-space constants in glue/v4-powerpc/config.h, matching what
 *   glue/v4-x86/space.c does for x86.
 *
 **********************************************************************/

bool space_is_user_area (addr_t addr)
{ return (word_t) addr >= USER_AREA_START && (word_t) addr < USER_AREA_END; }

/* space_is_user_area_addr / _fpage are architecture-neutral wrappers over the
   space_is_user_area above; they live in api/v4/accessors.c. */

bool space_is_tcb_area (addr_t addr)
{ return (word_t) addr >= KTCB_AREA_START && (word_t) addr < KTCB_AREA_END; }

bool space_is_copy_area (addr_t addr)
{ return (word_t) addr >= COPY_AREA_START && (word_t) addr < COPY_AREA_END; }

bool space_is_initialized (space_t *self)
{ fpage_t kip = self->kip_area; return !fpage_is_nil_fpage (&kip); }

bool space_is_mappable_addr (space_t *self, addr_t addr)
{
    fpage_t kip = self->kip_area, utcb = self->utcb_area;

    return space_is_user_area (addr) &&
	!fpage_is_addr_in_fpage (&kip, addr) &&
	!fpage_is_addr_in_fpage (&utcb, addr);
}

bool space_is_mappable_fpage (space_t *self, fpage_t fp)
{
    fpage_t kip = self->kip_area, utcb = self->utcb_area;

    return space_is_user_area_fpage (fp) &&
	!fpage_is_overlapping (&kip, fp) &&
	!fpage_is_overlapping (&utcb, fp);
}

space_t * get_current_space (void)			{ return tcb_get_space (get_current_tcb ()); }
space_t * get_current_space_c (void)			{ return get_current_space (); }
space_t * get_kernel_space_c (void)			{ return get_kernel_space (); }
bool      is_privileged_space_c (space_t *space)	{ return is_privileged_space (space); }

/* These two were inline in the C++ class: allocate was empty, and release
   flushed the whole TLB.  asid.h drives both. */
void space_allocate_hw_asid (space_t *self, word_t hw_asid)	{ (void) self; (void) hw_asid; }
void space_release_hw_asid (space_t *self, word_t hw_asid)
{ (void) hw_asid; space_flush_tlb (self, NULL); }

/* Walk the page table for vaddr, as glue/v4-x86/space.c does. */
bool space_lookup_mapping (space_t *self, addr_t vaddr, pgent_t **r_pg,
			   word_t *r_size, cpuid_t cpu)
{
    pgent_t *pg = space_pgent_cpu (self, page_table_index (size_max, vaddr), cpu);
    word_t pgsize = size_max;

    for (;;)
    {
	if (!pg)
	    return false;

	if (pgent_is_valid (pg, self, pgsize))
	{
	    if (pgent_is_subtree (pg, self, pgsize))
	    {
		if (pgsize == 0)
		    return false;
		pg = pgent_next (pgent_subtree (pg, self, pgsize), self, pgsize - 1,
				 page_table_index (pgsize - 1, vaddr));
		pgsize--;
		continue;
	    }
	    if (r_pg)   *r_pg = pg;
	    if (r_size) *r_size = pgsize;
	    return true;
	}
	return false;
    }
}

bool space_lookup_mapping_c (space_t *self, addr_t vaddr, pgent_t **r_pg, word_t *r_size)
{ return space_lookup_mapping (self, vaddr, r_pg, r_size, 0); }

fpage_t space_unmap_fpage (space_t *self, fpage_t fpage, bool flush, bool unmap_all)
{
    mdb_ctrl_t ctrl;

    ctrl.raw = 0;
    ctrl.set_rights	= !fpage_is_rwx (&fpage);
    ctrl.reset_status	= 1;
    ctrl.deliver_status	= 1;
    fpage_set_rwx (&fpage, ~fpage_get_rwx (&fpage));
    return space_mapctrl (self, fpage, ctrl, 0, unmap_all);
}
