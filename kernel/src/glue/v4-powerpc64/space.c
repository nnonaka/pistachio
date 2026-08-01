/*********************************************************************
 *                
 * Copyright (C) 2003-2004, 2006, 2010,  National ICT Australia (NICTA)
 *                
 * File path:     glue/v4-powerpc64/space.c
 * Description:   address space management
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
 * $Id: space.cc,v 1.13 2006/11/17 17:04:18 skoglund Exp $
 *                
 ********************************************************************/

#include <debug.h>
#include <kmemory.h>
#include <linear_ptab.h>
#include <kdb/tracepoints.h>

#include INC_API(tcb.h)
#include INC_API(kernelinterface.h)

#include INC_ARCH(page.h)
#include INC_ARCH(pghash.h)
#include INC_ARCH(pgtab.h)
//#include INC_ARCH(phys.h)

#include INC_GLUE(space.h)
#include INC_GLUE(pghash.h)
#include INC_GLUE(pgent_inline.h)

DECLARE_TRACEPOINT(hash_miss_cnt);
DECLARE_TRACEPOINT(hash_insert_cnt);
//DECLARE_TRACEPOINT(ptab_4k_map_cnt);

DECLARE_KMEM_GROUP(kmem_utcb);
DECLARE_KMEM_GROUP(kmem_tcb);
EXTERN_KMEM_GROUP(kmem_pgtab);
EXTERN_KMEM_GROUP(kmem_space);

#define TRACE_SPACE(x...)
//#define TRACE_SPACE(x...)	TRACEF(x)

/* The kernel space is statically defined beause it is needed
 * before the virtual memory has been setup and the kernel
 * memory allocator.
 */
char kernel_space_object[sizeof(space_t)] __attribute__((aligned(POWERPC64_PAGE_SIZE)));
#if CONFIG_POWERPC64_STAB
char kernel_space_segment_table[POWERPC64_STAB_SIZE] __attribute__((aligned(POWERPC64_STAB_SIZE)));
#endif

space_t *kernel_space = (space_t*)&kernel_space_object;
tcb_t *dummy_tcb = NULL;

//translation table
struct transTable_t transTable[TRANSLATION_TABLE_ENTRIES];

INLINE word_t pagedir_idx (addr_t addr)
{
    return page_table_index (size_max, addr);
}

INLINE tcb_t * get_dummy_tcb(void)
{
    return dummy_tcb;
}

bool space_handle_hash_miss( space_t *self, addr_t vaddr )
{
    pgent_t *pg;
    pgsize_e pgsize;

    /* Upstream passed a printf() call as the macro's `str' argument and called
       the second one with no arguments at all -- the shape TRACEPOINT had
       before it grew a format string, which master still carries here even
       though its own tracepoints.h has taken (tp, str, args...) for years.
       Rewritten to the current contract.  Notes §167. */
    TRACEPOINT( hash_miss_cnt, "hash miss @ %p (current=%p, space=%p)\n",
	vaddr, get_current_tcb(), self );

    if ( space_lookup_mapping_c( self, vaddr, &pg, &pgsize ) )
    {
	TRACEPOINT( hash_insert_cnt, "hash insert\n" );

	pghash_insert_mapping( get_pghash(), self, vaddr, pg, pgsize );

	return true;
    }

    return false;
}

bool space_handle_protection_fault( space_t *self, addr_t vaddr, bool dsi )
{
    pgent_t *pg;
    pgsize_e pgsize;

    // Check if mapping exist in page table
    if ( space_lookup_mapping_c ( self, vaddr, &pg, &pgsize) )
    {
	/* If data exception - check if write access denied */
	if (dsi)
	{
	    // Is it writeable (rights have changed)
	    if (pgent_is_writable (pg, self, pgsize))
	    {
		pghash_update_mapping( get_pghash(), self, vaddr, pg, pgsize );
		return true;
	    }
	} else {    /* Instruction access */
	    // Check if rights have been updated
	    if (pgent_is_executable( pg, self, pgsize ))
	    {
		pghash_update_mapping( get_pghash(), self, vaddr, pg, pgsize );
		return true;
	    }
	}
    }

    return false;
}

void space_add_mapping( space_t *self, addr_t vaddr, addr_t paddr,
		    bool writable, bool executable,
		    bool kernel, pgsize_e size )
{
    pgent_t * pg = space_pgent_cpu( self, pagedir_idx (vaddr), 0 );
    pgsize_e pgsize = size_max;

    //TRACEF("space %p: vaddr = %p, pgent = %p, size = %d\n", self, vaddr, pg, size);
    /*
     * Sanity check size
     */
#ifdef CONFIG_POWERPC64_LARGE_PAGES
    ASSERT((size == size_4k) || (size == size_16m));
#else
    ASSERT((size == size_4k));
#endif

    /*
     * Lookup mapping
     */
    while (1) {
	if ( pgent_is_valid( pg, self, pgsize ) )
	{
	    // Sanity check
	    if ( !pgent_is_subtree( pg, self, pgsize) )
	    {
		printf ("%dKB mapping @ %p space %p already exists.\n",
			page_size (pgsize) >> 10, vaddr, self);
		enter_kdebug ("mapping exists");
		return;
	    }
	}

	if ( pgsize == size )
	    break;

	// Create subtree
	if ( !pgent_is_valid( pg, self, pgsize ) )
	    pgent_make_subtree( pg, self, pgsize, kernel );

	pg = pgent_next( pgent_subtree( pg, self, pgsize ),
			 self, pgsize-1, page_table_index( pgsize-1, vaddr ) );
	pgsize--;
    }

    /*
     * Modify page table
     */
    pgent_set_entry (pg, self, pgsize, paddr,
		   4 | (writable ? 2 : 0) | (executable ? 1 : 0),
		   l4default, kernel );

    pghash_insert_mapping( get_pghash(), self, vaddr, pg, size );
}

/**********************************************************************
 *
 *                         System initialization 
 *
 **********************************************************************/

void SECTION(".init.memory") init_kernel_space()
{
    ASSERT(!dummy_tcb);
    dummy_tcb = (tcb_t*)kmem_alloc(&kmem,  kmem_tcb, POWERPC64_PAGE_SIZE );
    ASSERT(dummy_tcb);
    dummy_tcb = virt_to_phys(dummy_tcb);

    TRACE_SPACE( "initialised kernel space of size %x @ %p\n",
	         sizeof(space_t), kernel_space);

    space_init_kernel_mappings (kernel_space);
}

void SECTION(".init.memory") space_init_kernel_mappings( space_t *self )
{
    /* Initialize the ASID cache */
    vsid_asid_cache_init( get_vsid_asid_cache(), self );

    /* XXX Insert kernel into linear page table?
     * we should never fault on it. */
}

/* We need to map the kernel early.
 * This mapping is bolted(not replaceable).
 * This is because the kernel memory allocator has not started.
 */
void SECTION(".init.memory") early_kernel_map(void)
{
    pgent_t pg;
    word_t i;

    // XXX - use a block zero function
    for ( i = 0; i < sizeof(space_t); i+= 8 )
	*(word_t *)((word_t) kernel_space + i) = 0;

#if CONFIG_POWERPC64_STAB
    /* Setup the kernel segment table */
    ppc64_stab_t *stab = space_get_seg_table (kernel_space);
    /* Set the ASR directly, with the valid bit */
    *(word_t *)stab = (word_t)virt_to_phys(&kernel_space_segment_table) | 1;

    // XXX - use a block zero function
    for ( i = 0; i < POWERPC64_STAB_SIZE; i+= 8 )
	*(word_t *)((word_t)&kernel_space_segment_table + i) = 0;
#else
    /* Set the initial Address Space ASID to 0
     * as we are running on the kernel ASID
     */
    asm volatile ( "mtasr   %0;" :: "r" (0) );
#endif

#ifdef CONFIG_POWERPC64_LARGE_PAGES
    /* Create a dummy page table entry */
    pgent_set_entry( &pg, kernel_space, size_16m, 0,
		  7, l4default, true );
    /* Insert the kernel mapping, bolted */
    pghash_insert_mapping_bolted( get_pghash(), kernel_space, (addr_t) KERNEL_OFFSET,
			&pg, size_16m, true );

#else
    /* Insert mappings for kernel (16MB area) */
    for ( i = 0; i < (MB(16)); i += page_size(size_4k) )
    {
	/* Create a dummy page table entry */
	pgent_set_entry( &pg, kernel_space, size_4k,
		      (addr_t)(i), 7, l4default, true );

	/* Insert the kernel mapping, bolted */
	pghash_insert_mapping_bolted( get_pghash(), kernel_space,
			(addr_t)(KERNEL_OFFSET + i), &pg, size_4k, true );
    }
#endif
}

/**********************************************************************
 *
 *                    space_t implementation
 *
 **********************************************************************/

/**
 * space_t::init initializes the space_t
 *
 * maps the kernel area and initializes shadow ptabs etc.
 */
void space_init(space_t *self, fpage_t utcb_area, fpage_t kip_area)
{
    //TRACEF("uctb %p, kip %p\n", utcb_area,kip_area);

    self->x.utcb_area = utcb_area;
    self->x.kip_area = kip_area;
    vsid_asid_init (&self->x.vsid_asid);

#if CONFIG_POWERPC64_STAB
    ppc64_stab_init (&self->x.segment_table);
#endif

    /* Map the kip. Not writeable, user */
    space_add_4k_mapping( self, fpage_get_base (&kip_area), virt_to_phys(get_kip()),
	    false, false );
}

void space_allocate_tcb(space_t *self, addr_t addr)
{
    pgsize_e pgsize;
    pgent_t * pg;
    addr_t page;

    ASSERT(self == kernel_space);

    page = kmem_alloc(&kmem, kmem_tcb, POWERPC64_PAGE_SIZE);

    // Check if mapping exist in page table (dummy page)
    if ( space_lookup_mapping_c ( self, addr, &pg, &pgsize) )
    {
	pgent_clear(pg, self, pgsize, false, addr);
	space_flush_tlbent( self, self, addr, POWERPC64_PAGE_BITS );
    }

    space_add_4k_mapping_noexecute(self, addr, virt_to_phys(page), true, true);
}

void space_release_kernel_mapping (space_t *self, addr_t vaddr, addr_t paddr,
				   word_t log2size)
{
    fpage_t utcb_area = space_get_utcb_page_area (self);

    /* Free up memory used for UTCBs */
    if (fpage_is_addr_in_fpage (&utcb_area, vaddr))
	kmem_free(&kmem, kmem_utcb, phys_to_virt (paddr), 1UL << log2size);
}

utcb_t *space_allocate_utcb( space_t *self, tcb_t *tcb )
{
    addr_t utcb;
    addr_t page;
    pgsize_e pgsize;
    pgent_t * pg;

    ASSERT (tcb);
    utcb = (addr_t) tcb_get_utcb_location (tcb);

    if( space_lookup_mapping_c (self, (addr_t) utcb, &pg, &pgsize) )
    {
        addr_t kaddr = addr_mask( pgent_address (pg, self, pgsize),
                                  ~page_mask (pgsize) );
        return (utcb_t *)phys_to_virt
            ( addr_offset( kaddr, (word_t) utcb & page_mask( pgsize )) );
    }

    page = kmem_alloc(&kmem,  kmem_utcb, page_size( size_4k ) );

    space_add_4k_mapping(self, (addr_t) utcb, virt_to_phys(page),
		 true, false);

    return (utcb_t *)
        addr_offset (page, addr_mask (utcb, page_size (size_4k) - 1));
}

void space_map_dummy_tcb(space_t *self, addr_t addr)
{
    ASSERT(self == kernel_space);
    /* XXX We map the dummy page (kernel/user read-only) since PPC has no
     * support for super-readonly,user-noaccess
     */
    space_add_4k_mapping( self, addr, (addr_t)virt_to_phys(get_dummy_tcb()), false, false );
}

void space_map_sigma0(space_t *self, addr_t addr)
{
    ASSERT( 
	    ((addr < get_kip()->reserved_mem0.low) 
	     || (addr >= get_kip()->reserved_mem0.high))
    	    && 
	    ((addr < get_kip()->reserved_mem1.low) 
	     || (addr >= get_kip()->reserved_mem1.high)) 
	    );

    space_add_4k_mapping( self, addr, addr, true, false );
}

