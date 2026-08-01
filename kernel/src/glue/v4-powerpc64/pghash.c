/****************************************************************************
 *
 * Copyright (C) 2003-2004,  National ICT Australia (NICTA)
 *
 * File path:	glue/v4-powerpc64/pghash.c
 * Description:	PowerPC64 page hash handler.
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
 * $Id: pghash.cc,v 1.9 2006/11/17 17:04:18 skoglund Exp $
 *
 ***************************************************************************/

#include <debug.h>
#include <linear_ptab.h>

#include INC_API(kernelinterface.h)

#include INC_GLUE(pghash.h)
#include INC_GLUE(space.h)
#include INC_GLUE(pgent_inline.h)
#include INC_PLAT(prom.h)

pghash_t pghash;

/* If it exists, modify an entry in the Hash Table */
void pghash_update_mapping( pghash_t *self, space_t *s, addr_t vaddr, pgent_t *pgent, pgsize_e size )
{
    bool large;
    word_t vsid;
    ppc64_pte_t *pte;

#ifdef CONFIG_POWERPC64_LARGE_PAGES
    ASSERT((size == size_4k) || (size == size_16m));
#else
    ASSERT((size == size_4k));
#endif
    large = (size == size_4k) ? false : true;
    vsid = space_get_vsid( s, vaddr );

    pte = ppc64_htab_locate_pte( pghash_get_htab (self), (word_t)vaddr,
	    vsid, pgent->map.pteg_slot,
	    pgent->map.second_hash, large );

    if( pte )
    {
	ppc64_pte_create_bolted( pte, (word_t)vaddr, pgent_get_pte (pgent, s), vsid,
			pgent->map.second_hash, large , pte->x.bolted );
    }
    ppc64_invalidate_tlbe( vaddr, large );
}

void pghash_flush_mapping( pghash_t *self, space_t *s, addr_t vaddr, pgent_t *pgent, pgsize_e size )
{
    ppc64_pte_t *pte;

#ifdef CONFIG_POWERPC64_LARGE_PAGES
    ASSERT((size == size_4k) || (size == size_16m));
#else
    ASSERT((size == size_4k));
#endif

    pte = ppc64_htab_locate_pte( pghash_get_htab (self), (word_t)vaddr,
	    space_get_vsid( s, vaddr ), pgent->map.pteg_slot,
	    pgent->map.second_hash, (size == size_4k) ? 0 : 1);

    if( pte )
	pte->raw.word0 = 0;
}

void pghash_insert_mapping_bolted( pghash_t *self, space_t *s, addr_t vaddr, pgent_t *pgent, pgsize_e size, bool bolted )
{
    word_t pteg_slot, is_second_hash;
    ppc64_pte_t *pte;
    word_t vsid = space_get_vsid( s, vaddr );
    bool large = (size == size_4k) ? false : true;

#ifdef CONFIG_POWERPC64_LARGE_PAGES
    ASSERT((size == size_4k) || (size == size_16m));
#else
    ASSERT((size == size_4k));
#endif

    pte = ppc64_htab_find_insertion( pghash_get_htab (self), (word_t)vaddr, vsid,
	    &pteg_slot, &is_second_hash, large);

    // Check for a pre-existing, valid translation in the page hash.
    if( pte->x.v == 1 )
    {
	space_t *evict_space = space_lookup_space( pte->x.vsid );
	addr_t evict_addr = (addr_t)ppc64_htab_reverse_hash( pghash_get_htab (self), pte );

	TRACEF( "pghash eviction: vaddr %x, space %x\n", 
		evict_addr, evict_space );

	// TODO: lock the evict space, to prevent collisions in the data
	// structures (SMP).
	pgent_t *evict_pgent;
	pgsize_e evict_size;

	// Flush in-flight updates to the translation.
	sync();

	// Update the page table's dirty + referenced bits.
	ASSERT( space_lookup_mapping_c( evict_space, evict_addr, &evict_pgent, &evict_size ) );

#ifdef CONFIG_POWERPC64_LARGE_PAGES
	pgent_set_accessed( evict_pgent, evict_space, pte->x.l ? size_16m : size_4k, pte->x.r );
	pgent_set_dirty( evict_pgent, evict_space, pte->x.l ? size_16m : size_4k, pte->x.c );
#else
	pgent_set_accessed( evict_pgent, evict_space, size_4k, pte->x.r );
	pgent_set_dirty( evict_pgent, evict_space, size_4k, pte->x.c );
#endif
	
	/* Clear the PTE */
	pte->raw.word0 = 0;

	ppc64_invalidate_tlbe( evict_addr, pte->x.l );
    }

    // Insert a new translation.
    ppc64_pte_create_bolted( pte, (word_t)vaddr, pgent_get_pte (pgent, s), vsid, is_second_hash, large, bolted );
    pgent->map.pteg_slot = pteg_slot;
    pgent->map.second_hash = is_second_hash;
}

/* bolted defaulted to false. */
void pghash_insert_mapping( pghash_t *self, space_t *s, addr_t vaddr, pgent_t *pgent, pgsize_e size )
{
    pghash_insert_mapping_bolted( self, s, vaddr, pgent, size, false );
}


/* were protected members */
static bool pghash_try_location( pghash_t *self, word_t phys_start, word_t size );
static bool pghash_finish_init( pghash_t *self, word_t phys_start, word_t size );

SECTION(".init") bool pghash_init( pghash_t *self, word_t tot_phys_mem )
{
    word_t size;
    word_t phys_start;

    // Try allocating memory for the page hash, starting with the optimal
    // size, and then by reducing the size by half.
    for( size = ppc64_htab_optimal_size(tot_phys_mem);
	    size >= ppc64_htab_min_size();
	    size = size >> 1 )
    {
	// Search through phys memory for a location that fits the page
	// hash of a given size, and aligned to the size.
	for( phys_start = size; 
		phys_start < (tot_phys_mem - size); 
		phys_start += size )
	{
	    if( pghash_try_location(self, phys_start, size) )
	       	return pghash_finish_init( self, phys_start, size );
	}
    }

    return false;
}

static SECTION(".init") bool pghash_try_location( pghash_t *self, word_t phys_start, word_t size )
{
    // We are relocating everything
    kernel_interface_page_t *kip = PTRRELOC(get_kip());
    word_t phys_end = phys_start + size;
    word_t i;

    if ((word_t)kip->sigma0.mem_region.high > phys_start)
	return false;
    if ((word_t)kip->sigma1.mem_region.high > phys_start)
	return false;
    if ((word_t)kip->root_server.mem_region.high > phys_start)
	return false;

    // Walk through the KIP's memory descriptors and search for any
    // reserved memory regions that collide with our intended memory
    // allocation.
    for( i = 0; i < memory_info_get_num_descriptors (&kip->memory_info); i++ )
    {
	memdesc_t *mdesc = memory_info_get_memdesc( &kip->memory_info, i );
	word_t low, high;

	if( (memdesc_type (mdesc) == MEMDESC_CONVENTIONAL) || memdesc_is_virtual (mdesc) )
	    continue;

	low = (word_t)memdesc_low (mdesc);
	high = (word_t)memdesc_high (mdesc);

	if( (phys_start < low) && (phys_end > high) )
	    return false;
	if( (phys_start >= low) && (phys_start < high) )
	    return false;
	if( (phys_end > low) && (phys_end <= high) )
	    return false;
    }

    // No console setup yet - go via prom
    prom_print_hex( "Installing hash table at", phys_start );
    prom_print_hex( ", size", size );
    prom_puts("\n\r");
    return true;
}

static SECTION(".init") bool pghash_finish_init( pghash_t *self, word_t phys_start, word_t size )
{
    kernel_interface_page_t *kip = PTRRELOC(get_kip());
    addr_t virt_start = addr_align_up( (addr_t)PGHASH_AREA_START, size );
    pgent_t pg;
    pgsize_e pgsize;
    word_t i;

    // Insert a KIP memory descriptor to protect the page hash.
    memory_info_insert( &kip->memory_info, MEMDESC_RESERVED, 0, false,
	    (addr_t)phys_start, (addr_t)(phys_start + size) );

    // Initialize the page hash at the given location.
    ppc64_htab_init( PTRRELOC(pghash_get_htab (self)), phys_start, (word_t)virt_start, size );

    prom_print_hex( "Setup hash table at virtual", (word_t)virt_start | phys_start );
    prom_print_hex( ", physical", phys_start );
    prom_puts("\n\r");

    prom_puts( "Inserting bolted hash table mappings\n\r" );

#ifdef CONFIG_POWERPC64_LARGE_PAGES
    pgsize = size_16m;
#else
    pgsize = size_4k;
#endif

    /* Insert mappings for hash page table */
    for ( i = 0; i < (size); i += page_size(pgsize))
    {
	/* Create a dummy page table entry */
	pgent_set_entry( &pg, get_kernel_space(), pgsize,
			(addr_t)(phys_start + i),
		      6, l4default, true );

	pghash_insert_mapping_bolted( self, get_kernel_space(),
			(addr_t)(((word_t)virt_start | phys_start) + i),
			&pg, pgsize, true );
    }

    return true;
}

