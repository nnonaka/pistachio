/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     arch/powerpc/pgent-pghash_functions.h
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

#ifndef __ARCH__POWERPC__PGENT_PGHASH_FUNCTIONS_H__
#define __ARCH__POWERPC__PGENT_PGHASH_FUNCTIONS_H__

#include <kmemory.h>

#include INC_ARCH(pgent.h)
#include INC_GLUE(space.h)
#include INC_GLUE(pghash.h)

EXTERN_KMEM_GROUP (kmem_pgtab);

/* The guards below read CONFIG_PPC_MMU_SEGMENTS.  Upstream spells all three
   without the S, which no .cml defines, so on a segment-MMU build this file
   never synced a page hash entry and never flushed one -- see notes §144 and
   §145.  Corrected, so the three bodies are now reached. */

// Linknode access

INLINE word_t pgent_get_linknode_raw (pgent_t *self)
{
    return *(word_t *) ((word_t) self + POWERPC_PAGE_SIZE);
}

INLINE void pgent_set_linknode_raw (pgent_t *self, word_t val)
{
    *(word_t *) ((word_t) self + POWERPC_PAGE_SIZE) = val;
}


// Predicates

INLINE bool  pgent_is_valid (pgent_t *self, space_t * s, word_t pgsize)
{
    if( pgsize == size_4m )
	return self->tree.valid;
    else
	return self->raw != 0;
}

INLINE bool  pgent_is_writable (pgent_t *self, space_t * s, word_t pgsize)
{
    return self->map.pp != read_only;
}

INLINE bool  pgent_is_readable (pgent_t *self, space_t * s, word_t pgsize)
{
    return pgent_is_valid (self, s, pgsize);
}

INLINE bool  pgent_is_executable (pgent_t *self, space_t * s, word_t pgsize)
{
    return pgent_is_valid (self, s, pgsize);
}

INLINE bool  pgent_is_subtree (pgent_t *self, space_t * s, word_t pgsize)
{
    return (pgsize == size_4m);
}

INLINE bool  pgent_is_kernel (pgent_t *self, space_t * s, word_t pgsize)
{
    return s == get_kernel_space();
}

// Retrieval

/* paddr_t, not addr_t.  The swtlb pair returns paddr_t and generic callers
   hold one; paddr_t is u32_t on every segment-MMU CPU (types.h widens it only
   for CONFIG_PLAT_PPC44X), so no value changes -- but the types now agree. */
INLINE paddr_t  pgent_address (pgent_t *self, space_t * s, word_t pgsize)
{
    return (paddr_t)(self->raw & POWERPC_PAGE_MASK);
}

INLINE pgent_t * pgent_subtree (pgent_t *self, space_t * s, word_t pgsize)
{
    return (pgent_t *) phys_to_virt( (addr_t)pgent_address (self, s, pgsize) );
}

INLINE mapnode_t * pgent_mapnode (pgent_t *self, space_t * s, word_t pgsize, addr_t vaddr)
{
    return (mapnode_t *) (pgent_get_linknode_raw (self) ^ (word_t) vaddr);
}

INLINE addr_t  pgent_vaddr (pgent_t *self, space_t * s, word_t pgsize, mapnode_t * map)
{
    return (addr_t) (pgent_get_linknode_raw (self) ^ (word_t) map);
}

/* Supplied for parity with the swtlb pair, which generic/mdb_mem.c calls
   under CONFIG_NEW_MDB.  Upstream's pghash class has no such member; the body
   is the swtlb one, which is written in terms of the three predicates above
   and so needs no knowledge of the entry layout. */
INLINE word_t  pgent_rights (pgent_t *self, space_t * s, word_t pgsize)
{
    return ((pgent_is_readable (self, s, pgsize) ? (1<<2) : 0) |
	    (pgent_is_writable (self, s, pgsize) ? (1<<1) : 0) |
	    (pgent_is_executable (self, s, pgsize) ? (1<<0) : 0));
}

INLINE word_t  pgent_attributes (pgent_t *self, space_t * s, word_t pgsize)
{
    return (self->raw & PPC_PAGE_CACHE_INHIBIT) ? 1 : 0;
}

INLINE word_t  pgent_get_translation (pgent_t *self, space_t *s, word_t pgsize)
{
    return self->raw & PPC_PAGE_PTE_MASK;
}

#ifdef CONFIG_PPC_MMU_SEGMENTS
// Page hash synchronization

INLINE void pgent_update_from_pghash (pgent_t *self, space_t * s, addr_t vaddr)
{
    ppc_translation_t *pte;

    // Force the cpu to sync the tlb with the page hash before we read from it.
    sync();

    pte = ppc_htab_locate_pte (pghash_get_htab (get_pghash()), (word_t)vaddr,
			       space_get_vsid (s, vaddr),
			       self->map.pteg_slot, self->map.second_hash);
    if( pte )
    {
	self->map.referenced = pte->x.r;
	self->map.changed = pte->x.c;
    }
}
#endif

INLINE word_t  pgent_reference_bits (pgent_t *self, space_t *s, word_t pgsize, addr_t vaddr)
{
    word_t rwx = 0;
#ifdef CONFIG_PPC_MMU_SEGMENTS
    pgent_update_from_pghash (self, s, vaddr);
#endif
    if( self->map.referenced ) rwx = 5;
    if( self->map.changed )    rwx |= 6;
    return rwx;
}

INLINE void  pgent_update_reference_bits (pgent_t *self, space_t *s, word_t pgsize, word_t rwx)
{
    if (rwx) self->map.referenced = 1;
    if (rwx & 0x2) self->map.changed = 1;
}

// Modification

INLINE void  pgent_flush (pgent_t *self, space_t *s, word_t pgsize, bool kernel, addr_t vaddr)
{
#ifdef CONFIG_PPC_MMU_SEGMENTS
    /* Upstream calls get_pghash()->flush_mapping(s, vaddr, pgsize, this), and
       pghash_t has no such member -- not here and not in master.  The one
       flush this hash offers is flush_4k_mapping, and 4k is the only size the
       variant supports (HW_VALID_PGSIZES is 1 << 12), so pgsize carries no
       information and the substitution is forced rather than chosen.  Same
       call space.c makes, one guard over. */
    pghash_flush_4k_mapping (get_pghash(), s, vaddr, self);
#endif
}

INLINE void  pgent_clear (pgent_t *self, space_t * s, word_t pgsize, bool kernel, addr_t vaddr)
{
    pgent_t tmp;
    tmp.raw = self->raw;

    self->raw = 0;
    if( !kernel )
	pgent_set_linknode_raw (self, 0);

    pgent_flush (&tmp, s, pgsize, kernel, vaddr);
}

INLINE void  pgent_make_subtree (pgent_t *self, space_t * s, word_t pgsize, bool kernel)
{
    addr_t page = kmem_alloc(&kmem,  kmem_pgtab, POWERPC_PAGE_SIZE * (kernel ? 1:2) );

    self->raw = (word_t)virt_to_phys( page );
    if( self->raw )
	self->tree.valid = 1;
}

INLINE void  pgent_remove_subtree (pgent_t *self, space_t * s, word_t pgsize, bool kernel)
{
    addr_t ptab = (addr_t)pgent_address (self, s, pgsize);
    self->raw = 0;

    kmem_free(&kmem,  kmem_pgtab, phys_to_virt(ptab),
	    POWERPC_PAGE_SIZE * (kernel ? 1:2) );
}

INLINE void  pgent_set_entry (pgent_t *self, space_t * s, word_t pgsize, paddr_t paddr, word_t rwx, word_t attrib, bool kernel)
{
    word_t attr = rwx & 2 ? read_write : read_only;
    if( attrib )
	attr |= PPC_PAGE_CACHE_INHIBIT;

    self->raw = ((word_t)paddr & POWERPC_PAGE_MASK) |
	(attr & PPC_PAGE_FLAGS_MASK);
}

INLINE void  pgent_set_writable (pgent_t *self, space_t * s, word_t pgsize)
{
    self->map.pp = read_write;
}

INLINE void  pgent_set_readonly (pgent_t *self, space_t * s, word_t pgsize)
{
    self->map.pp = read_only;
}

/* Parity with the swtlb pair, as pgent_rights above.  The PP field carries no
   execute bit, so this is the write bit and nothing else -- the same reduction
   pgent_set_entry already makes. */
INLINE void  pgent_set_rights (pgent_t *self, space_t *s, word_t pgsize, word_t rwx)
{
    self->map.pp = rwx & 2 ? read_write : read_only;
}

INLINE void  pgent_update_rights (pgent_t *self, space_t *s, word_t pgsize, word_t rwx)
{
    if( rwx & 2 )
	pgent_set_writable (self, s, pgsize);
}

INLINE void  pgent_revoke_rights (pgent_t *self, space_t *s, word_t pgsize, word_t rwx)
{
    if( rwx & 2)
	pgent_set_readonly (self, s, pgsize);
}

INLINE void  pgent_set_attributes (pgent_t *self, space_t * s, word_t pgsize, word_t attrib)
{
    if (attrib)
	self->raw |= PPC_PAGE_CACHE_INHIBIT;
    else
	self->raw &= ~PPC_PAGE_CACHE_INHIBIT;
}

INLINE void  pgent_reset_reference_bits (pgent_t *self, space_t *s, word_t pgsize)
{
    self->map.referenced = 0;
    self->map.changed = 0;
}

INLINE void  pgent_set_accessed (pgent_t *self, space_t *s, word_t pgsize, word_t flag)
{
    self->map.referenced |= flag;
}

INLINE void  pgent_set_dirty (pgent_t *self, space_t *s, word_t pgsize, word_t flag)
{
    self->map.changed |= flag;
}

INLINE void  pgent_set_linknode (pgent_t *self, space_t * s, word_t pgsize, mapnode_t * map, addr_t vaddr)
{
    pgent_set_linknode_raw (self, (word_t) map ^ (word_t) vaddr);
}

// Movement

INLINE pgent_t * pgent_next (pgent_t *self, space_t * s, word_t pgsize, word_t num)
{
    return self + num;
}

// Debug

INLINE void  pgent_dump_misc (pgent_t *self, space_t * s, word_t pgsize)
{
}

#endif	/* !__ARCH__POWERPC__PGENT_PGHASH_FUNCTIONS_H__ */
