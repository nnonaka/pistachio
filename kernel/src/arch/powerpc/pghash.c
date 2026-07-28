/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     src/arch/powerpc/pghash.cc
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

#include <debug.h>

#include INC_ARCH(bat.h)
#include INC_ARCH(page.h)
#include INC_ARCH(pghash.h)
#include INC_ARCH(string.h)
#include INC_ARCH(ppc_registers.h)
#include INC_ARCH(msr.h)

#include INC_GLUE(hwspace.h)
#include INC_GLUE(bat.h)


ppc_translation_t * ppc_htab_find_insertion (ppc_htab_t *self, word_t virt, word_t vsid,
					      word_t *slot, word_t *is_second_hash)
{
    ppc_translation_t *groups[2];
    word_t hash;
    int cnt, group, clean_slot, clean_hash;

    /* If we find an unreferenced page, keep track of it for eviction. */
    clean_hash = -1;
    clean_slot = -1;

    /* Locate the primary and secondary PTEGs. */
    hash = ppc_htab_primary_hash (self,  virt, vsid );
    groups[0] = ppc_htab_get_pteg (self,  hash );
    hash = ppc_htab_secondary_hash (self,  hash );
    groups[1] = ppc_htab_get_pteg (self,  hash );

    /* Search for an invalid pte, and while searching, keep track of 
     * unreferenced pages.
     */
    for( group = 0; group < 2; group++ )
	for( cnt = 0; cnt < HTAB_PTEG_SIZE; cnt++ )
	    if( groups[group][cnt].x.v == 0 ) {
		*slot = cnt;
		*is_second_hash = group;
		return &groups[group][cnt];
	    }
	    else if( !groups[group][cnt].x.r && !groups[group][cnt].x.c ) {
		clean_slot = cnt;
		clean_hash = group;
	    }

    /* We must evict a pte. */
    if( clean_slot == -1 ) {
	// Unable to find an unreferenced pte, so choose a random slot.
	clean_hash = 0;
	clean_slot = (virt >> POWERPC_PAGE_BITS) & 7;
    }
    *slot = clean_slot;
    *is_second_hash = clean_hash;
    return &groups[ clean_hash ][ clean_slot ];
}

SECTION(".init.memory") void ppc_htab_bat_map (ppc_htab_t *self)
{
    ppc_bat_t bat;
    
    /*  Map with a bat register. */
    bat.raw.upper = bat.raw.lower = 0;
    bat.x.bepi = (word_t)self->base >> BAT_BEPI;
    bat.x.bl = (self->size-1) >> 17;
    bat.x.vs = 1;
    bat.x.brpn = self->phys_base >> BAT_BRPN;
    bat.x.m = 1;
    bat.x.pp = BAT_PP_READ_WRITE;

    ppc_set_pghash_dbat( l, bat.raw.lower );
    ppc_set_pghash_dbat( u, bat.raw.upper );
    isync();
}

SECTION(".init.memory") void ppc_htab_init (ppc_htab_t *self, word_t phys_base, word_t virt_start, word_t size)
{
    self->base = (ppc_translation_t *)virt_start;
    self->size = size;
    self->phys_base = phys_base;
    self->htab_mask = (size-1) >> 16;
    self->hash_mask = (self->htab_mask << 10) | ((1 << 10) - 1);

    /* Activate the bat register, and zero the memory region (which invalidates
     * all PTE's.
     */
    ppc_htab_bat_map (self);
    zero_block( (word_t *)virt_start, size );
}


/* Locations in the inlined assembler in ppc_htab_install() */
EXTERN_C void ppc_htab_install_real( void );
EXTERN_C void ppc_htab_install_real_exit( void );

static inline void ppc_htab_install( ppc_sdr1_t sdr1, ppc_segment_t segment_val )
{
    word_t real_entry;
    word_t msr_off_mask, msr_on_mask;

    /* Invalidate the tlb!!! */
    asm volatile ("isync");
    ppc_invalidate_tlb();

    /* While in real mode (important!), install the sdr1 register and 
     * set the segment IDs.
     */
    real_entry = virt_to_phys((word_t)ppc_htab_install_real);
    msr_on_mask = (MSR_IR_ENABLED << MSR_IR) | (MSR_DR_ENABLED << MSR_DR);
    msr_off_mask = ~msr_on_mask;
    asm volatile (
	    "mfmsr %%r10 ;"		// Get the current msr.
	    "and %%r10, %%r10, %4 ;"	// Disable address translation.
	    "mtsrr1 %%r10 ;"		// Prepare to activate the new msr.
	    "mtsrr0 %0 ;"	// Prepare to jump to ppc_htab_install_real
	    "rfi ;"			// Jump to real mode.

	    "ppc_htab_install_real: ;"

	    "sync ;"			// Prepare to change the page hash.
	    "mtspr 25, %1 ;"		// Install the page hash.
	    "isync ;"			// Activate the page hash.

	    /* Set the segment ID. */
	    "mr %%r10, %2 ;"		// Prepare to set the segment ID.
	    "mtsr 0, %%r10 ; addi %%r10, %%r10, 1 ;"	
	    "mtsr 1, %%r10 ; addi %%r10, %%r10, 1 ;"
	    "mtsr 2, %%r10 ; addi %%r10, %%r10, 1 ;"
	    "mtsr 3, %%r10 ; addi %%r10, %%r10, 1 ;"
	    "mtsr 4, %%r10 ; addi %%r10, %%r10, 1 ;"
	    "mtsr 5, %%r10 ; addi %%r10, %%r10, 1 ;"
	    "mtsr 6, %%r10 ; addi %%r10, %%r10, 1 ;"
	    "mtsr 7, %%r10 ; addi %%r10, %%r10, 1 ;"
	    "mtsr 8, %%r10 ; addi %%r10, %%r10, 1 ;"
	    "mtsr 9, %%r10 ; addi %%r10, %%r10, 1 ;"
	    "mtsr 10, %%r10 ; addi %%r10, %%r10, 1 ;"
	    "mtsr 11, %%r10 ; addi %%r10, %%r10, 1 ;"
	    "mtsr 12, %%r10 ; addi %%r10, %%r10, 1 ;"
	    "mtsr 13, %%r10 ; addi %%r10, %%r10, 1 ;"
	    "mtsr 14, %%r10 ; addi %%r10, %%r10, 1 ;"
	    "mtsr 15, %%r10 ;"
	    "isync ;"			// Activate the new segment ID.

	    "mfmsr %%r10 ;"		// Get the current msr.
	    "or %%r10, %%r10, %5 ;"	// Enable address translation.
	    "mtsrr1 %%r10 ;"		// Prepare to activate the new msr.
	    "mtsrr0 %3 ;"		// Prepare to jump to virtual mode.
	    "rfi ;"			// Jump to virtual mode.
	    "ppc_htab_install_real_exit:"
	    : 
	    : "b" (real_entry), "b" (sdr1.raw),
	      "b" (segment_val.raw), "b" (ppc_htab_install_real_exit), 
	      "b" (msr_off_mask), "b" (msr_on_mask)
	    : "ctr", "10" );
}

SECTION(".init.memory") void ppc_htab_activate (ppc_htab_t *self, ppc_segment_t segment_val)
{
    ppc_sdr1_t sdr1;

    sdr1.x.htaborg = (word_t)self->phys_base >> POWERPC_HTABORG_SHIFT;
    sdr1.x.htabmask = self->htab_mask;

    ppc_htab_install( sdr1, segment_val );
}


