/****************************************************************************
 *
 * Copyright (C) 2003,  National ICT Australia (NICTA)
 *
 * File path:	glue/v4-powerpc64/pgent_inline.h
 * Description:	Inlined functions for pgent_t (pgent.h).
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, self list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, self list of conditions and the following disclaimer in the
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
 * $Id: pgent_inline.h,v 1.10 2006/11/17 17:02:04 skoglund Exp $
 *
 ***************************************************************************/

#ifndef __GLUE__V4_POWERPC64__PGENT_INLINE_H__
#define __GLUE__V4_POWERPC64__PGENT_INLINE_H__

#include <kmemory.h>

#include INC_GLUE(pgent.h)
#include INC_GLUE(space.h)
#include INC_GLUE(pghash.h)

EXTERN_KMEM_GROUP (kmem_pgtab);

extern word_t hw_pgshifts[];

INLINE word_t subtree_size (pgsize_e pgsize)
{
    ASSERT( pgsize != size_4k );
    return 1UL << (hw_pgshifts[pgsize] - hw_pgshifts[pgsize-1]);
}


// Page hash synchronization

INLINE void pgent_update_from_pghash (pgent_t *self, space_t * s, addr_t vaddr, bool large)
{
    ppc64_pte_t *pte;

    // Force the cpu to sync the tlb with the page hash before we read from it.
    sync();

    pte = get_pghash()->get_htab()->locate_pte( (word_t)vaddr, 
	    space_get_vsid (s, vaddr), self->map.pteg_slot, self->map.second_hash, large );

    if( pte )
    {
	self->map.referenced = pte->x.r;
	self->map.changed = pte->x.c;
    }
}

// Linknode access 

INLINE word_t pgent_get_linknode (pgent_t *self, space_t * s, pgsize_e pgsize)
{ 
//    return *(word_t *) ((word_t) self + (pgsize == size_4k) ? PPC64_SIZE_4k_LEVEL : PPC64_SIZE_16m_LEVEL ); 
    /* Presently, large pages are only supported for kernel */
    if ( pgsize != size_4k )
	printf("get_linknode failed: %p, %d, - %p\n", s, pgsize, self);
    ASSERT( pgsize == size_4k );
    return *(word_t *) ((word_t) self + PPC64_SIZE_4k_LEVEL ); 
}

INLINE void pgent_set_linknode (pgent_t *self, space_t * s, pgsize_e pgsize, word_t val)
{ 
//    *(word_t *) ((word_t) self + (pgsize == size_4k) ? PPC64_SIZE_4k_LEVEL : PPC64_SIZE_16m_LEVEL ) = val; 
    /* Presently, large pages are only supported for kernel */
    ASSERT( pgsize == size_4k );
    *(word_t *) ((word_t) self + PPC64_SIZE_4k_LEVEL ) = val; 
}


// Predicates

INLINE bool pgent_is_valid (pgent_t *self, space_t * s, pgsize_e pgsize)
{ 
    return (self->map.is_valid || self->tree.is_subtree);
}

INLINE bool pgent_is_writable (pgent_t *self, space_t * s, pgsize_e pgsize)
{ 
    return (self->map.pp == read_write);
}

INLINE bool pgent_is_readable (pgent_t *self, space_t * s, pgsize_e pgsize)
{ 
    return (self->map.pp != kernel_only);
}

INLINE bool pgent_is_executable (pgent_t *self, space_t * s, pgsize_e pgsize)
{ 
    return (self->map.noexecute == 0);
}

INLINE bool pgent_is_subtree (pgent_t *self, space_t * s, pgsize_e pgsize)
{ 
    return (self->tree.is_subtree && !self->tree.is_valid);
}

INLINE bool pgent_is_kernel (pgent_t *self, space_t * s, pgsize_e pgsize)
{
    return (self->map.pp == kernel_only);
}

// Retrieval

INLINE addr_t pgent_address (pgent_t *self, space_t * s, pgsize_e pgsize)
{ 
    return (addr_t)(self->map.rpn << POWERPC64_PAGE_BITS);
}
	
INLINE pgent_t * pgent_subtree (pgent_t *self, space_t * s, pgsize_e pgsize)
{ 
    return (pgent_t *) phys_to_virt( self->tree.subtree ); 
}

INLINE mapnode_t * pgent_mapnode (pgent_t *self, space_t * s, pgsize_e pgsize, addr_t vaddr)
{ 
    return (mapnode_t *) (pgent_get_linknode (self, s, pgsize) ^ (word_t) vaddr); 
}

INLINE addr_t pgent_vaddr (pgent_t *self, space_t * s, pgsize_e pgsize, mapnode_t * map)
{ 
    return (addr_t) (pgent_get_linknode (self, s, pgsize) ^ (word_t) map); 
}

INLINE word_t pgent_reference_bits (pgent_t *self, space_t *s, pgsize_e pgsize, 
	addr_t vaddr)
{
    pgent_update_from_pghash (self, s, vaddr, (pgsize == size_4k) ? 0 : 1);

    word_t rwx = 0;
    if( self->map.referenced ) rwx = 5;
    if( self->map.changed )    rwx |= 6;
    return rwx;
}

INLINE void pgent_update_reference_bits (pgent_t *self, space_t *s, pgsize_e pgsize,
					    word_t rwx)
{
    if (rwx) self->map.referenced = 1;
    if (rwx & 0x2) self->map.changed = 1;
}

INLINE word_t pgent_get_pte (pgent_t *self, space_t *s)
{
    return self->raw & PPC64_PAGE_PTE_MASK;
}

INLINE word_t pgent_attributes (pgent_t *self, space_t * s, pgsize_e pgsize)
{
    return self->map.wimg;
}

// Modification

INLINE void pgent_flush (pgent_t *self, space_t *s, pgsize_e pgsize, bool kernel, 
	addr_t vaddr)
{
    get_pghash()->flush_mapping( s, vaddr, self, pgsize );
}

INLINE void pgent_clear (pgent_t *self, space_t * s, pgsize_e pgsize, bool kernel, 
	addr_t vaddr)
{ 
    pgent_t tmp;
    tmp.raw = self->raw;

    self->raw = 0;
    if( !kernel )
	pgent_set_linknode (self, s, pgsize, 0);
    
    tmp.flush( s, pgsize, kernel, vaddr );
}

INLINE void pgent_make_subtree (pgent_t *self, space_t * s, pgsize_e pgsize, bool kernel)
{
    /* Presently, large pages are only supported for kernel */
    addr_t page = kmem_alloc(&kmem,  kmem_pgtab,
		    ( subtree_size(pgsize) * sizeof(word_t) ) *
		    ( ( (pgsize-1) == size_4k ) ? ( kernel ? 1 : 2 ) : 1 ) );

    self->tree.subtree = (word_t)virt_to_phys( page );
    self->tree.is_subtree = 1;
    self->tree.is_valid = 0;
}

INLINE void pgent_remove_subtree (pgent_t *self, space_t * s, pgsize_e pgsize, bool kernel)
{
    addr_t ptab = pgent_subtree (self, s, pgsize );
    self->raw = 0;

    kmem_free(&kmem,  kmem_pgtab, ptab, 
		    ( subtree_size(pgsize) * sizeof(word_t) ) *
		    ( ( (pgsize-1) == size_4k ) ? ( kernel ? 1 : 2 ) : 1 ) );

}

INLINE void pgent_set_entry (pgent_t *self, space_t * s, pgsize_e pgsize, addr_t paddr,
				word_t rwx, word_t attrib, bool kernel)
{
    self->map.pp = kernel ? kernel_only : (rwx & 2 ?  read_write : read_only);

    self->map.wimg = attrib;

    self->map.rpn = (word_t)paddr >> POWERPC64_PAGE_BITS;
    self->map.is_valid = 1;
    self->map.noexecute = ! (rwx & 1);
}

INLINE void pgent_update_rights (pgent_t *self, space_t *s, pgsize_e pgsize, word_t rwx)
{ 
    if( rwx & 2 ) 
	self->map.pp = read_write;
    if( rwx & 1 ) 
	self->map.noexecute = 0;
}

INLINE void pgent_revoke_rights (pgent_t *self, space_t *s, pgsize_e pgsize, word_t rwx)
{ 
    if( rwx & 2 ) 
	self->map.pp = read_only;
    if( rwx & 1 ) 
	self->map.noexecute = 1;
}

INLINE void pgent_set_attributes (pgent_t *self, space_t *s, pgsize_e pgsize, word_t attrib)
{
    self->map.wimg = attrib;
}

INLINE void pgent_reset_reference_bits (pgent_t *self, space_t *s, pgsize_e pgsize)
{ 
    self->map.referenced = 0;
    self->map.changed = 0;
}

INLINE void pgent_set_accessed (pgent_t *self, space_t *s, pgsize_e pgsize, word_t flag)
{
    self->map.referenced |= flag;
}

INLINE void pgent_set_dirty (pgent_t *self, space_t *s, pgsize_e pgsize, word_t flag)
{
    self->map.changed |= flag;
}

INLINE void pgent_set_linknode (pgent_t *self, space_t * s, pgsize_e pgsize,
	mapnode_t * map, addr_t vaddr)
{ 
    pgent_set_linknode (self, s, pgsize, (word_t) map ^ (word_t) vaddr); 
}

// Movement

INLINE pgent_t * pgent_next (pgent_t *self, space_t * s, pgsize_e pgsize, word_t num)
{ 
    return self + num; 
}

// Debug

INLINE void pgent_dump_misc (pgent_t *self, space_t * s, pgsize_e pgsize)
{
}

#endif	/* __GLUE__V4_POWERPC64__PGENT_INLINE_H__ */
