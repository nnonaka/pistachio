/*********************************************************************
 *                
 * Copyright (C) 2005-2006,  Karlsruhe University
 *                
 * File path:     sigma0_io.cc
 * Description:   IO-Port (IA32/AMD64) specific stuff
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
 * $Id: sigma0_io.cc,v 1.4 2006/06/08 11:15:46 skoglund Exp $
 *                
 ********************************************************************/
#include <l4/arch.h>
#include <l4/kip.h>
#include <l4/message.h>
#include <l4/kdebug.h>
#include <l4io.h>

#include "sigma0.h"
#include "region.h"

/**
 * Memory regions for IO-ports available for allocation.
 */
region_pool_t io_pool;

/**
 * IO-Memory regions already allocated.
 */
region_pool_t io_alloc_pool;


/**
 * Initialze IO port pool.
 */
void init_iopool (void)
{
    // Insert IO-port memory.
    region_pool_insert_range (&io_pool, L4_IO_PORT_START, L4_IO_PORT_END, L4_anythread);

}

/**
 * Allocate an IO-fpage
 * 
 * @param tid		id of thread doing allocation
 * @param iofp		iofpage (port, size)
 * @param map		genrated map item corresponding to alloced page
 *
 * @return true upon success, false otherwise
 */
bool allocate_iopage (L4_ThreadId_t tid, L4_Fpage_t iofp, L4_MapItem_t * map)
{
    region_t * r;

    
    // Make sure that addr is properly aligned.
    L4_Word_t addr = L4_IoFpagePort(iofp) & ~(L4_IoFpageSize(iofp) - 1);
    L4_Word_t addr_high = L4_IoFpagePort(iofp) + L4_IoFpageSize(iofp) - 1;
    L4_Word_t log2size = L4_IoFpageSizeLog2(iofp);

    // Test if valid IO-Fpage
    if (!L4_IsIoFpage (iofp) || log2size > 16)
    {
	dprintf (0, "s0: cannot map invalid IO page %x\n", (int) iofp.raw);
	return false;
    }

    region_pool_reset (&io_pool);
    
    while ((r = region_pool_next (&io_pool)) != NULL)
    {
	if (r->low > addr_high || r->high < addr)
	    continue;
 	L4_Fpage_t result = region_allocate_at (r, addr, log2size, tid, L4_IoFpageLog2);
	if (! L4_IsNilFpage (result))
	{
	    dprintf(1, "s0: map IO fpage %x port %x [size %x]\n", 
		    (int) result.raw, (int) addr, (int) log2size);
	    *map = L4_MapItem (result, L4_IoFpagePort(result));
	    region_pool_insert (&io_alloc_pool, region_new (addr, addr_high, tid));
	    return true;
	}
    }

    // Check if memory has already been allocated.
    region_pool_reset (&io_alloc_pool);
    while ((r = region_pool_next (&io_alloc_pool)) != NULL)
    {
	if (region_can_allocate (r, addr, log2size, tid))
	{
	    *map = L4_MapItem (iofp, L4_IoFpagePort(iofp));
	    return true;
	}
    }

    
    // If all the above failed we have to try the slow way of
    // allocating parts of memory from the IO page pool.

    region_pool_t * all_io_pools[] = { &io_pool,
					  &io_alloc_pool,
					  (region_pool_t *) NULL };
    
    // Loop once for checking followed by once for allocating
    for (L4_Word_t phase = 0; phase < 2; phase++)
    {
	// Use 1 byte as stepping
	for (L4_Word_t a = addr; a < addr_high; a += 1)
	{
	    L4_Word_t a_end = a + 1;
	    bool failed = true;

	    // Try the different pools
	    for (L4_Word_t i = 0; failed && all_io_pools[i] != NULL; i++)
	    {
		region_pool_reset (all_io_pools[i]);
		while ((r = region_pool_next (all_io_pools[i])) != NULL)
		{
		    if (r->low > a_end || r->high < a)
			continue;

		    // Test if allocation is possible
		    if (region_can_allocate (r, a, 0, tid))
		    {
			failed = false;
			if (phase == 1 && all_io_pools[i] != &io_alloc_pool)
			{
			    // Allocation phase
			    region_allocate_at (r, a, 0, tid, L4_IoFpageLog2);
			    region_pool_insert (&io_alloc_pool, region_new (a, a_end - 1, tid));
			}
		    }
		}
	    }
	    if (failed){
		dprintf (2, "s0: tried %lx, phase = %lx, failed = %s\n", a , phase, (failed ? "true" : "false"));
		return false;
	    }
	}
    }

    *map = L4_MapItem (iofp, L4_IoFpagePort(iofp));
    return true;

    
}
