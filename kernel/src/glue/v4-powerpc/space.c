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
#include INC_ARCH(swtlb.h)

DECLARE_TRACEPOINT(hash_miss_cnt);
DECLARE_TRACEPOINT(hash_insert_cnt);
DECLARE_TRACEPOINT(ptab_4k_map_cnt);

DECLARE_KMEM_GROUP(kmem_utcb);
EXTERN_KMEM_GROUP(kmem_pgtab);
EXTERN_KMEM_GROUP(kmem_space);

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
    space_add_mapping (kernel_space,  addr, virt_to_phys(page), size_4k, true, true);

    sync_kernel_space( addr );
#endif
}

void space_map_dummy_tcb (space_t *self, addr_t addr)
{
#if !defined(CONFIG_STATIC_TCBS)
    add_mapping( addr, (addr_t)virt_to_phys(get_dummy_tcb()), size_4k, false, true );
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

#ifdef CONFIG_PPC_MMU_SEGMENT
    ASSERT(pgsize == size_4k);
    get_pghash()->insert_4k_mapping( this, vaddr, pgent);
#endif
}

#if 0
void space_add_4k_mapping (space_t *self, addr_t vaddr, paddr_t paddr, bool writable, bool kernel, word_t attrib)
{
    pgent_t *pgent = space_pgent (self,  page_table_index(size_4m, vaddr) );
    if( !pgent_is_valid (pgent, self, size_4m))
	pgent_make_subtree (pgent, self, size_4m, kernel );

    pgent = pgent_subtree (pgent, self, size_4m )->next( this, 
	    size_4k, page_table_index(size_4k, vaddr) );

    pgent_set_entry (pgent, self, size_4k, paddr, writable ? 7 : 5, 
		      attrib, kernel);

#ifdef CONFIG_PPC_MMU_SEGMENT
    get_pghash()->insert_4k_mapping( this, vaddr, pgent);
#endif
}
#endif

void space_flush_mapping (space_t *self, addr_t vaddr, word_t pgsize, pgent_t *pgent)
{
#ifdef CONFIG_PPC_MMU_SEGMENT
    ASSERT(pgsize == size_4k);
    get_pghash()->flush_4k_mapping( this, vaddr, pgent );
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
