/****************************************************************************
 *
 * Copyright (C) 2002-2003, Karlsruhe University
 *
 * File path:	glue/v4-powerpc/pghash.c
 * Description:	PowerPC page hash handler.
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
 * $Id: pghash.c,v 1.13 2004/06/08 15:09:56 joshua Exp $
 *
 ***************************************************************************/

#include <debug.h>

#include INC_API(kernelinterface.h)
#include INC_ARCH(pgent.h)
#include INC_GLUE(pghash.h)
#include INC_GLUE(space.h)

#define TRACE_PGHASH(x...)
//#define TRACE_PGHASH(x...)     TRACEF(x)

pghash_t pghash;

void pghash_update_4k_mapping (pghash_t *self, space_t *s, addr_t vaddr, pgent_t *pgent)
{
    ppc_translation_t *pte = ppc_htab_locate_pte (pghash_get_htab (self), (word_t)vaddr,
	    space_get_vsid (s, vaddr), pgent->map.pteg_slot, pgent->map.second_hash);

    if( pte && pte->x.v )
    {
    	ppc_translation_create_from_entry( pte, (word_t)vaddr,
		pgent_get_translation (pgent, s, size_4k), pte->x.vsid );
    }
}

void pghash_insert_4k_mapping (pghash_t *self, space_t *s, addr_t vaddr, pgent_t *pgent)
{
    word_t pteg_slot, is_second_hash;
    ppc_translation_t *pte;
    word_t vsid = space_get_vsid (s, vaddr);

    pte = ppc_htab_find_insertion (pghash_get_htab (self), (word_t)vaddr, vsid,
	    &pteg_slot, &is_second_hash);

    // Check for a pre-existing, valid translation in the page hash.
    if( pte->x.v == 1 )
    {
	space_t *evict_space = space_vsid_to_space( pte->x.vsid );
	addr_t evict_addr = (addr_t)ppc_htab_reverse_hash (pghash_get_htab (self), pte);
	pgent_t *evict_pgent;

	TRACEF( "pghash eviction: vaddr %x, space %x\n", 
		evict_addr, evict_space );

	// TODO: lock the evict space, to prevent collisions in the data
	// structures (SMP).
	// TODO: ensure that we don't evict the translations of any of
	// the currently used TCB's.  If this were to happen, the kernel
	// stack for handling the page-fault would not have a translation!
	/* JTL: page-hash eviction isn't sanity checked for TCB's.  Left as a
	   comment rather than a #warning: the note is upstream's and still
	   stands, but a #warning in a file that now compiles would fire on
	   every build. */

	// Flush in-flight updates to the translation.
	sync();

	// Update the page table's dirty + referenced bits.
	evict_pgent = space_page_lookup (evict_space, evict_addr);
	ASSERT( evict_pgent && pgent_is_valid (evict_pgent, evict_space, size_4k) );
	pgent_set_accessed (evict_pgent, evict_space, size_4k, pte->x.r);
	pgent_set_dirty (evict_pgent, evict_space, size_4k, pte->x.c);
	pte->x.v = 0;

	ppc_invalidate_tlbe( evict_addr );
    }

    // Insert a new translation.
    ppc_translation_create_from_entry( pte, (word_t)vaddr,
	    pgent_get_translation (pgent, s, size_4k), vsid );
    pgent->map.pteg_slot = pteg_slot;
    pgent->map.second_hash = is_second_hash;
}

void pghash_flush_4k_mapping (pghash_t *self, space_t *s, addr_t vaddr, pgent_t *pgent)
{
    ppc_translation_t *pte = ppc_htab_locate_pte (pghash_get_htab (self), (word_t)vaddr,
	    space_get_vsid (s, vaddr), pgent->map.pteg_slot, pgent->map.second_hash);

    if( pte && pte->x.v )
	pte->x.v = 0;
}


SECTION(".init") bool pghash_init (pghash_t *self, word_t tot_phys_mem)
{
    word_t size;
    word_t phys_start;

    // Try allocating memory for the page hash, starting with the optimal
    // size, and then by reducing the size by half.
    for( size = ppc_htab_optimal_size (tot_phys_mem);
	    size >= ppc_htab_min_size();
	    size = size >> 1 )
    {
	// Search through phys memory for a location that fits the page
	// hash of a given size, and aligned to the size.
	for( phys_start = size; 
		phys_start < (tot_phys_mem - size); 
		phys_start += size )
	{
	    if( pghash_try_location (self, phys_start, size) )
	       	return pghash_finish_init (self, phys_start, size);
	}
    }

    return false;
}

SECTION(".init") bool pghash_try_location (pghash_t *self, word_t phys_start, word_t size)
{
    kernel_interface_page_t *kip = get_kip();
    mem_region_t test_region;
    word_t i;

    mem_region_set (&test_region, (addr_t)phys_start, (addr_t)(phys_start+size));

    // Look for overlap with the privileged servers.
    if( mem_region_is_intersection (&kip->root_server.mem_region, test_region) ||
	     mem_region_is_intersection (&kip->sigma0.mem_region, test_region) ||
	     mem_region_is_intersection (&kip->sigma1.mem_region, test_region) )
    {
	return false;
    }
 
    // Walk through the KIP's memory descriptors and search for any
    // reserved memory regions that collide with our intended memory
    // allocation.
    for( i = 0; i < memory_info_get_num_descriptors (&kip->memory_info); i++ )
    {
	memdesc_t *mdesc = memory_info_get_memdesc (&kip->memory_info, i);
	mem_region_t mdesc_region;

	if( (memdesc_type (mdesc) == MEMDESC_CONVENTIONAL) || memdesc_is_virtual (mdesc) )
	    continue;

	mem_region_set (&mdesc_region, memdesc_low (mdesc), memdesc_high (mdesc));

	if( mem_region_is_intersection (&mdesc_region, test_region) )
	    return false;
    }

    TRACE_PGHASH( "Installing page hash at 0x%x, size 0x%x.\n", 
	    phys_start, size );
    return true;
}

SECTION(".init") bool pghash_finish_init (pghash_t *self, word_t phys_start, word_t size)
{
    addr_t virt_start = addr_align_up( (addr_t)PGHASH_AREA_START, size );

    // Insert a KIP memory descriptor to protect the page hash.
    memory_info_insert (&get_kip()->memory_info, MEMDESC_RESERVED, 0, false,
	    (addr_t)phys_start, (addr_t)(phys_start + size));

    // Initialize the page hash at the given location.
    ppc_htab_init (pghash_get_htab (self), phys_start, (word_t)virt_start, size);

    TRACE_INIT( "Activated page hash at virtual address 0x%x,\n"
	        "    physical address 0x%x, size 0x%x.\n",
	        virt_start, phys_start, size );

    return true;
}
