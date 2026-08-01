/*********************************************************************
 *                
 * Copyright (C) 2005,  Karlsruhe University
 *                
 * File path:     region.h
 * Description:   Generic regions
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
 * $Id: region.h,v 1.3 2005/06/02 14:11:08 joshua Exp $
 *                
 ********************************************************************/
#ifndef __REGION_H__
#define __REGION_H__

/**
 * Descriptor for a memory region.  A memory region has a start
 * address, an end address, and an owner.
 */
/* Was four classes.  region_t had a constructor and an `operator new' that
   allocated from region_list, so `new region_t (l, h, o)' is region_new below:
   list allocation followed by the three assignments the constructor did.  The
   overloaded pairs -- allocate, insert, remove -- take distinct names.
   See doc/notes/cpp-to-c-migration.md §172. */
struct region_t
{
    L4_Paddr_t		low;
    L4_Paddr_t		high;
    L4_ThreadId_t	owner;

    struct region_t	*prev;
    struct region_t	*next;
} __attribute__ ((packed));
typedef struct region_t region_t;


/**
 * Opaque struct for holding a region_t structure.
 */
struct region_listent_t
{
    region_t	reg;		/* was private */
} __attribute__ ((packed));
typedef struct region_listent_t region_listent_t;

L4_INLINE region_t * region_listent_region (region_listent_t *self)
    { return &self->reg; }
L4_INLINE region_listent_t * region_listent_next (region_listent_t *self)
    { return *(region_listent_t **) self; }
L4_INLINE void region_listent_set_next (region_listent_t *self, region_listent_t *n)
    { *(region_listent_t **) self = n; }


/**
 * List of memory regions.
 */
struct region_list_t
{
    region_listent_t * list;	/* was private */
};
typedef struct region_list_t region_list_t;

void region_list_add (region_list_t *self, L4_Paddr_t addr, L4_Word_t size);
L4_Word_t region_list_contents (region_list_t *self);
region_t * region_list_alloc (region_list_t *self);
void region_list_free (region_list_t *self, region_t * r);


/**
 * A region pool is set of memory regions.
 */
struct region_pool_t
{
    /* were private */
    region_t first;
    region_t last;
    region_t * ptr;
};
typedef struct region_pool_t region_pool_t;

void region_pool_init (region_pool_t *self);
void region_pool_dump (region_pool_t *self);
void region_pool_insert (region_pool_t *self, region_t * r);
void region_pool_remove (region_pool_t *self, region_t * r);
void region_pool_insert_range (region_pool_t *self, L4_Paddr_t low,
			       L4_Paddr_t high, L4_ThreadId_t owner);
void region_pool_remove_range (region_pool_t *self, L4_Paddr_t low,
			       L4_Paddr_t high);
void region_pool_reset (region_pool_t *self);
region_t * region_pool_next (region_pool_t *self);


/* region_t's own operations. */
region_t * region_new (L4_Paddr_t low, L4_Paddr_t high, L4_ThreadId_t owner);
void region_swap (region_t *self, region_t * n);
void region_remove (region_t *self);
bool region_is_adjacent (region_t *self, const region_t * r);
bool region_concatenate (region_t *self, region_t * reg);
bool region_can_allocate (region_t *self, L4_Paddr_t addr, L4_Word_t log2size,
			  L4_ThreadId_t tid);
/* was the two-way overload of allocate: by address, and by size alone. */
L4_Fpage_t region_allocate_at (region_t *self, L4_Paddr_t addr, L4_Word_t log2size,
			       L4_ThreadId_t tid,
			       L4_Fpage_t (*make_fpage) (L4_Word_t, int));
L4_Fpage_t region_allocate (region_t *self, L4_Word_t log2size, L4_ThreadId_t tid,
			    L4_Fpage_t (*make_fpage) (L4_Word_t, int));


/**
 * List of free region_t structures.
 */
extern region_list_t region_list;


#endif /* !__REGION_H__ */
