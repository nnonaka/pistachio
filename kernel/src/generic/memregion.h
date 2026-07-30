/*********************************************************************
 *                
 * Copyright (C) 2002,  Karlsruhe University
 *                
 * File path:     generic/memregion.h
 * Description:   memory region management
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
 * $Id: memregion.h,v 1.5 2003/10/27 15:57:42 joshua Exp $
 *                
 ********************************************************************/
#ifndef __GENERIC__MEMREGION_H__
#define __GENERIC__MEMREGION_H__

/**
 * mem_region_t:
 */
struct mem_region_t
{
    addr_t	low;
    addr_t	high;

};
typedef struct mem_region_t mem_region_t;



/* C forms of the mem_region_t methods (low/high are C-visible; mem_region_is_empty
   already lives as a wrapper in glue space.cc). */
INLINE word_t mem_region_get_size (const mem_region_t *self)
{ return self->high == 0 ? 0 : (word_t)self->high - (word_t)self->low; }
INLINE void mem_region_set (mem_region_t *self, addr_t low, addr_t high)
{ self->low = low; self->high = high; }

/* Dropped when this header was converted, because the one caller --
   glue/v4-powerpc/pghash.cc, choosing where to put the page hash -- is in a
   configuration that did not build, so nothing missed it.  The body is
   master's, unchanged.  Notes §144. */
INLINE bool mem_region_is_intersection (const mem_region_t *self, mem_region_t reg)
{
    return ((reg.low >= self->low) && (reg.low < self->high)) ||
	   ((reg.high > self->low) && (reg.high <= self->high)) ||
	   ((reg.low <= self->low) && (reg.high >= self->high));
}


#endif /* !__GENERIC__MEMREGION_H__ */
