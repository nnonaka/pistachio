/*********************************************************************
 *                
 * Copyright (C) 2005,  Karlsruhe University
 *                
 * File path:     region.cc
 * Description:   Generic regions
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
 * $Id: region.cc,v 1.3 2005/06/02 14:11:08 joshua Exp $
 *                
 ********************************************************************/
#include <l4/message.h>
#include <l4/kdebug.h>
#include <l4io.h>

#include "sigma0.h"
#include "region.h"


/**
 * List of free region_t structures.
 */
region_list_t region_list;



/* ================================================================
**
**			region_t
**
*/

/* `region_new (l, h, o)' was region_t::operator new -- which allocates from
   region_list -- followed by the constructor's three assignments.  One
   function in C. */
region_t * region_new (L4_Paddr_t l, L4_Paddr_t h, L4_ThreadId_t o)
{
    region_t * self = region_list_alloc (&region_list);

    self->low = l;
    self->high = h;
    self->owner = o;

    return self;
}


/**
 * Swap region_t backing store.  The whole region_t structure is
 * copied and pointers for surrounding region structures are relocated
 * to the new location.
 *
 * @param r	location of region_t to use instead of current memory
 */
void region_swap (region_t *self, region_t * r)
{
    *r = *self;
    r->prev->next = r->next->prev = r;
}


/**
 * Remove memory region.  Region is first removed from the pool which
 * it is allocated to before its memory is freed.  Region must not be
 * accessed after it has been removed.
 */
void region_remove (region_t *self)
{
    self->prev->next = self->next;
    self->next->prev = self->prev;
    region_list_free (&region_list, self);
}


/**
 * Check if supplied memory region is adjacent to current one.
 * @param r	memory region to check against
 * @return true it memory regions are adjacent, false otherwise
 */
bool region_is_adjacent (region_t *self, const region_t * r)
{
    return (self->low == r->high + 1 && self->low != 0) ||
	(self->high + 1 == r->low && r->low != 0);
}


/**
 * Concatenate supplied memory region with current one.
 *
 * @param r	memory region to concatenate with
 *
 * @return true if concatenation was successful, false otherwise
 */
bool region_concatenate (region_t *self, region_t * r)
{
    if (! L4_IsThreadEqual (self->owner, r->owner))
	return false;

    if (self->low == r->high + 1 && self->low != 0)
	self->low = r->low;
    else if (self->high + 1 == r->low && r->low != 0)
	self->high = r->high;
    else
	return false;

    return true;
}


/**
 * Try allocating part from memory region.  If allocation is
 * successful, current region_t might be deleted or split up into
 * multiple regions.
 *
 * @param log2size	size of region to allocate
 * @param tid		thread id to use for allocation
 * @param make_fpage	function for creating fpage
 *
 * @return fpage for allocated region if successful, nilpage otherwise
 */
L4_Fpage_t region_allocate (region_t *self, L4_Word_t log2size, L4_ThreadId_t tid,
			    L4_Fpage_t (*make_fpage) (L4_Word_t, int))
{
    L4_Word_t size = 1UL << log2size;
    L4_Fpage_t ret;

	L4_Fpage_t kip_area, utcb_area;
	L4_Word_t control, shortaddr;
	L4_ThreadId_t redirector;

    // Low and high address of region within mwmregion when they are
    // aligned according to log2size.  Note that these values might
    // overflow and must as such be handled with care.
    L4_Paddr_t low_a = (self->low + size - 1) & ~(size-1);
    L4_Paddr_t high_a = ((self->high + 1) & ~(size-1)) - 1;

    if (low_a > high_a			// Low rounded up to above high
	|| self->low > low_a			// Low wrapped around
	|| self->high < size-1		// High wrapped around
	|| (high_a - low_a) < size-1	// Not enough space in region
	|| (! L4_IsThreadEqual (self->owner, tid) && ! L4_IsThreadEqual (self->owner, L4_anythread)))
    {
	// Allocation failed
	ret = L4_Nilpage;
    }
    else if (low_a == self->low)
    {
	// Allocate from start of region


	if (self->low != (L4_Word_t)self->low) { //extended mapping
		shortaddr = (L4_Word_t)self->low;
		kip_area.X.s = log2size;
		kip_area.X.b = shortaddr >> 10;
		redirector.raw = self->low >> 32;
		utcb_area.raw = shortaddr;

		L4_SpaceControl(sigma0_id,1 << 29, kip_area, utcb_area, redirector,
				&control);
	}

    ret = L4_FpageAddRights (make_fpage ((L4_Word_t)self->low, log2size), L4_FullyAccessible);
	if (self->low + size == self->high + 1)
	    region_remove (self);
	else
	    self->low += size;
    }
    else if (high_a == self->high)
    {
	// Allocate from end of region

	if (high_a != (L4_Word_t)high_a) { //extended mapping
		shortaddr = (L4_Word_t)(high_a - size + 1);
		kip_area.X.s = log2size;
		kip_area.X.b = shortaddr >> 10;
		redirector.raw = self->low >> 32;
		utcb_area.raw = shortaddr;

		L4_SpaceControl(sigma0_id,1 << 29, kip_area, utcb_area, redirector,
				&control);
	}

    ret = L4_FpageAddRights (make_fpage ((L4_Word_t)(high_a) - size + 1, log2size), L4_FullyAccessible);
	self->high -= size;
    }
    else
    {
	// Allocate from middle of region

	if (low_a != (L4_Word_t)low_a) { //extended mapping
		shortaddr = (L4_Word_t)low_a;
		kip_area.X.s = log2size;
		kip_area.X.b = shortaddr >> 10;
		redirector.raw = self->low >> 32;
		utcb_area.raw = shortaddr;

		L4_SpaceControl(sigma0_id,1 << 29, kip_area, utcb_area, redirector,
				&control);
	}


	ret = L4_FpageAddRights (make_fpage ((L4_Word_t)low_a, log2size), L4_FullyAccessible);
	region_t * r = region_new (low_a + size, self->high, self->owner);
	r->next = self->next;
	r->prev = self;
	r->next->prev = self->next = r;
	self->high = low_a - 1;
    }

    return ret;
}


/**
 * Try allocating part from memory region.  If allocation is
 * successful, current region_t might be deleted or split up into
 * multiple regions.
 *
 * @param addr		location of region to allocate
 * @param log2size	size of region to allocate
 * @param tid		thread id to use for allocation
 * @param make_fpage	function for creating fpage
 *
 * @return fpage for allocated region if successful, nilpage otherwise
 */
L4_Fpage_t region_allocate_at (region_t *self, L4_Paddr_t addr, L4_Word_t log2size,
			       L4_ThreadId_t tid,
			       L4_Fpage_t (*make_fpage) (L4_Word_t, int))
{
    L4_Word_t size = 1UL << log2size;
    L4_Fpage_t ret;

    // Low and high address of region within mwmregion when they are
    // aligned according to log2size.  Note that these values might
    // overflow and must as such be handled with care.
    L4_Paddr_t low_a = (self->low + size - 1) & ~(size-1);
    L4_Paddr_t high_a = ((self->high + 1) & ~(size-1)) - 1;

    if (addr < low_a			// Address range below low
	|| (addr + size - 1) > high_a	// Address range above high
	|| (high_a - low_a) < size-1	// Not enough space in region
	|| self->low > low_a			// Low wrapped around
	|| self->high < size-1		// High wrapper around
	|| (! L4_IsThreadEqual (self->owner, tid) && ! L4_IsThreadEqual (self->owner, L4_anythread)))
    {
	// Allocation failed
	ret = L4_Nilpage;
    }
    else if (low_a == self->low && addr == self->low)
    {
	// Allocate from start of region
	ret = L4_FpageAddRights (make_fpage ((L4_Word_t)self->low, log2size), L4_FullyAccessible);
	if (self->low + size == self->high + 1)
	    region_remove (self);
	else
	    self->low += size;
    }
    else if (high_a == self->high && (addr + size - 1) == self->high)
    {
	// Allocate from end of region
	ret = L4_FpageAddRights (make_fpage ((L4_Word_t)(high_a - size + 1), log2size), L4_FullyAccessible);
	self->high -= size;
    }
    else
    {
	// Allocate from middle of region
	ret = L4_FpageAddRights (make_fpage ((L4_Word_t)addr, log2size), L4_FullyAccessible);
	region_t * r = region_new (addr + size, self->high, self->owner);
	r->next = self->next;
	r->prev = self;
	r->next->prev = self->next = r;
	self->high = addr - 1;
    }

    return ret;
}


/**
 * Check if it is possible to allocate from memory region.
 *
 * @param addr		location of region to allocate
 * @param log2size	size of region to allocate
 * @param tid		thread id to use for allocation
 *
 * @return true if allocation is possible, false otherwise
 */
bool region_can_allocate (region_t *self, L4_Paddr_t addr, L4_Word_t log2size,
				L4_ThreadId_t tid)
{
    L4_Word_t size = 1UL << log2size;

    // Low and high address of region within mwmregion when they are
    // aligned according to log2size.  Note that these values might
    // overflow and must as such be handled with care.
    L4_Paddr_t low_a = (self->low + size - 1) & ~(size-1);
    L4_Paddr_t high_a = ((self->high + 1) & ~(size-1)) - 1;

    if (addr < low_a			// Address range below low
	|| (addr + size - 1) > high_a	// Address range above high
	|| (high_a - low_a) < size-1	// Not enough space in region
	|| self->low > low_a			// Low wrapped around
	|| self->high < size-1		// High wrapper around
	|| (! L4_IsThreadEqual (self->owner, tid) && ! L4_IsThreadEqual (self->owner, L4_anythread)))
	return false;
    else
	return true;
}



/* ================================================================
**
**			region_list_t
**
*/


/**
 * Add more memory to be used for region_t structures.
 *
 * @param addr	location of memory to add
 * @param size	amount of memory to add
 */
void region_list_add (region_list_t *self, L4_Paddr_t addr, L4_Word_t size)
{
    if (addr == 0)
    {
	// Avoid inserting a NULL pointer into the list.
	addr += sizeof (region_listent_t);
	size -= sizeof (region_listent_t);
    }

    region_listent_t * m = (region_listent_t *) addr;

    for (; (L4_Word_t) (m+1) < addr + size; m++)
	region_listent_set_next (m, m+1);

    region_listent_set_next (m, self->list);
    self->list = (region_listent_t *) addr;
}


/**
 * @return number of region_t structures in pool
 */
L4_Word_t region_list_contents (region_list_t *self)
{
    L4_Word_t n = 0;
    for (region_listent_t * m = self->list; m != NULL; m = region_listent_next (m))
	n++;
    return n;
}


/**
 * Allocate a region_t structure.
 * @return newly allocated structure
 */
region_t * region_list_alloc (region_list_t *self)
{
    if (! self->list)
    {
	// We might need some memory for allocating memory.
	region_t tmp;
	region_list_add (self, (L4_Word_t) &tmp, sizeof (tmp));

	// Allocate some memory to sigma0.
	L4_MapItem_t dummy;
	if (! allocate_page (sigma0_id, min_pgsize, &dummy))
	{
	    printf ("s0: Unable to allocate memory.\n");
	    for (;;)
		L4_KDB_Enter ("s0: out of memory");
	}

	bool was_alloced = (self->list == NULL);
	if (! was_alloced)
	    self->list = (region_listent_t *) NULL;

	// Add newly allocated memory to pool.
	region_list_add (self, L4_Address (L4_MapItemSndFpage (dummy)), (1UL << min_pgsize));

	if (was_alloced)
	    // Swap temorary structure with a newly allocated one.
	    region_swap (&tmp, region_list_alloc (self));
    }

    // Remove first item from free list.
    region_listent_t * r = self->list;
    self->list = region_listent_next (r);

    return region_listent_region (r);
}


/**
 * Free a region_t structure.
 * @param r	region structure to free
 */
void region_list_free (region_list_t *self, region_t * r)
{
    region_listent_t * e = (region_listent_t *) r;
    region_listent_set_next (e, self->list);
    self->list = e;
}


/* ================================================================
**
**			region_pool_t
**
*/


/**
 * Initialize the memory pool structure.  Must be done prior to any
 * insertions into the pool.
 */
void region_pool_init (region_pool_t *self)
{
    self->first.next = self->first.prev = &self->last;
    self->last.next = self->last.prev = &self->first;
    self->first.low = self->first.high = 0;
    self->last.low = self->last.high = ~0UL;
    self->first.owner = self->last.owner = L4_nilthread;
    self->ptr = &self->last;
}


/**
 * Insert region into region pool.  Concatenate region with existing
 * regions if possible.
 *
 * @param r	region to insert into pool
 */
void region_pool_insert (region_pool_t *self, region_t * r)
{
    region_t * p = &self->first;
    region_t * n = self->first.next;

    // Find correct insert location
    while (r->low > n->high)
    {
	p = n;
	n = n->next;
    }

    if (region_concatenate (p, r))
    {
	// Region concatenated previous one
	region_list_free (&region_list, r);
	if (region_concatenate (p, n))
	    region_pool_remove (self, n);
    }
    else if (region_concatenate (n, r))
    {
	// Region concatenated to next one
	region_list_free (&region_list, r);
    }
    else
    {
	// No concatenation possible.  Insert into list
	r->next = n;
	r->prev = p;
	p->next = n->prev = r;
    }
}


/**
 * Remove region from region pool.  It is assumed that the region is
 * indeed contained in the pool.  Region must not be accessed after it
 * has been removed from pool.
 */
void region_pool_remove (region_pool_t *self, region_t * r)
{
    region_remove (r);
}


/**
 * Insert specified region into memory pool.  Concatenate with
 * existing regions if possible.
 *
 * @param low		lower limit of memory region
 * @param high		upper limit of memory region
 * @param owner		owner of memory region
 */
void region_pool_insert_range (region_pool_t *self, L4_Paddr_t low, L4_Paddr_t high,
			       L4_ThreadId_t owner)
{
    region_pool_insert (self, region_new (low, high, owner));
}


/**
 * Remove specified region from memory pool.
 *
 * @param low		lower limit of memory region to remove
 * @param high		upper limit of memory region to remove
 */
void region_pool_remove_range (region_pool_t *self, L4_Paddr_t low, L4_Paddr_t high)
{
    region_t * n = self->first.next;

    while (low > n->high)
	n = n->next;

    while (n != &self->last)
    {
	if (low <= n->low && high >= n->high)
	{
	    // Remove whole region node.
	    n = n->next;
	    region_pool_remove (self, n->prev);
	}
	else if (low <= n->low && high >= n->low)
	{
	    // Only need to modify lower limit.
	    n->low = high + 1;
	    break;
	}
	else if (low > n->low && low <= n->high)
	{
	    // Need to modify upper limit.
	    L4_Word_t old_high = n->high;
	    n->high = low - 1;
	    if (high < old_high)
	    {
		// Must split region into two separate regions.
		region_pool_insert_range (self, high + 1, old_high, n->owner);
		break;
	    }
	    n = n->next;
	}
	else
	    n = n->next;
    }
}


/**
 * Dump contents of memory region pool.
 */
void region_pool_dump (region_pool_t *self)
{
    region_t * r;
    region_pool_reset (self);
    while ((r = region_pool_next (self)) != NULL)
    {
	printf ("s0:  %p-%p   %p %s\n",
		(void *) r->low, (void *) r->high,
		(void *) r->owner.raw,
		L4_IsThreadEqual (r->owner, sigma0_id) ? "(sigma0)" :
		L4_IsThreadEqual (r->owner, sigma1_id) ? "(sigma1)" :
		L4_IsThreadEqual (r->owner, rootserver_id) ? "(root server)" :
		is_kernel_thread (r->owner) ? "(kernel)" :
		L4_IsThreadEqual (r->owner, L4_anythread) ? "(anythread)" :
		"");
    }
}

void region_pool_reset (region_pool_t *self)
{
    self->ptr = self->first.next;
}

region_t * region_pool_next (region_pool_t *self)
{
    if (self->ptr == &self->last)
	return (region_t *) NULL;
    region_t * ret = self->ptr;
    self->ptr = self->ptr->next;
    return ret;
}
