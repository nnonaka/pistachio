/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     arch/powerpc/pgent-swtlb_functions.h
 * Description:   
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
 * $Id$
 *                
 ********************************************************************/
#pragma once

#include <kmemory.h>
#include INC_GLUE(space.h)

EXTERN_KMEM_GROUP (kmem_pgtab);

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
    return self->tree.valid != 0;
}

INLINE bool  pgent_is_writable (pgent_t *self, space_t * s, word_t pgsize)
{
    return self->map.write;
}

INLINE bool  pgent_is_readable (pgent_t *self, space_t * s, word_t pgsize)
{
    return self->map.read;
}

INLINE bool  pgent_is_executable (pgent_t *self, space_t * s, word_t pgsize)
{
    return self->map.execute;
}

INLINE bool  pgent_is_subtree (pgent_t *self, space_t * s, word_t pgsize)
{
    return pgsize == size_4m && 
	self->tree.is_subtree == cache_subtree;
}

INLINE bool  pgent_is_kernel (pgent_t *self, space_t * s, word_t pgsize)
{
    return s == get_kernel_space();
}

// Retrieval
INLINE paddr_t  pgent_address (pgent_t *self, space_t * s, word_t pgsize)
{
    return (paddr_t)(self->raw & POWERPC_PAGE_MASK) + 
	((paddr_t)self->map.erpn << 32);
}

INLINE pgent_t * pgent_subtree (pgent_t *self, space_t * s, word_t pgsize)
{ 
    return (pgent_t *) pgent_address(self, s, pgsize); 
}

INLINE mapnode_t * pgent_mapnode (pgent_t *self, space_t * s, word_t pgsize, addr_t vaddr)
{ 
    return (mapnode_t *) (pgent_get_linknode_raw(self) ^ (word_t) vaddr); 
}

INLINE addr_t  pgent_vaddr (pgent_t *self, space_t * s, word_t pgsize, mapnode_t * map)
{ 
    return (addr_t) (pgent_get_linknode_raw(self) ^ (word_t) map); 
}

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

INLINE word_t  pgent_reference_bits (pgent_t *self, space_t *s, word_t pgsize, addr_t vaddr)
{
    word_t rwx = 0;
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

}

INLINE void  pgent_clear (pgent_t *self, space_t * s, word_t pgsize, bool kernel, addr_t vaddr)
{ 
    pgent_t tmp;
    tmp.raw = self->raw;

    self->raw = 0;
    if( !kernel )
	pgent_set_linknode_raw(self, 0);
}

INLINE void  pgent_make_subtree (pgent_t *self, space_t * s, word_t pgsize, bool kernel)
{
    self->raw = (word_t)kmem_alloc(&kmem,  kmem_pgtab, POWERPC_PAGE_SIZE * (kernel ? 1:2) );

    /* the following is a no-op */
    self->tree.is_subtree = cache_subtree;

    if( self->raw )
	self->tree.valid = 1;
}

INLINE void  pgent_remove_subtree (pgent_t *self, space_t * s, word_t pgsize, bool kernel)
{
    addr_t ptab = (addr_t) pgent_address(self, s, pgsize);
    self->raw = 0;

    kmem_free(&kmem,  kmem_pgtab, ptab, POWERPC_PAGE_SIZE * (kernel ? 1:2) );
}

INLINE void  pgent_set_entry (pgent_t *self, space_t * s, word_t pgsize, paddr_t paddr, word_t rwx, word_t attrib, bool kernel)
{
    self->raw = paddr & POWERPC_PAGE_MASK;
    self->map.erpn = (paddr >> 32) & 0xf;
    self->map.read = rwx >> 2 & 1;
    self->map.write = rwx >> 1 & 1;
    self->map.execute = rwx >> 0 & 1;
    self->map.caching = attrib;
}


INLINE void  pgent_update_rights (pgent_t *self, space_t *s, word_t pgsize, word_t rwx)
{ 
    if (rwx & 4) self->map.read = 1;
    if (rwx & 2) self->map.write = 1;
    if (rwx & 1) self->map.execute = 1;
}

INLINE void  pgent_revoke_rights (pgent_t *self, space_t *s, word_t pgsize, word_t rwx)
{ 
    if (rwx & 4) self->map.read = 0;
    if (rwx & 2) self->map.write = 0;
    if (rwx & 1) self->map.execute = 0;
}

INLINE void  pgent_set_rights (pgent_t *self, space_t *s, word_t pgsize, word_t rwx)
{
    self->map.read = rwx & 4 ? 1 : 0;
    self->map.write = rwx & 2 ? 1 : 0;
    self->map.execute = rwx & 1;
}

INLINE void  pgent_set_attributes (pgent_t *self, space_t * s, word_t pgsize, word_t attrib)
{
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
    printf("%s",
	   self->map.caching == 1 ? "inhibit " :
	   self->map.caching == 2 ? "coherent " :
	   self->map.caching == 3 ? "guarded " :
	   self->map.caching == 4 ? "write-through " : "");
}
