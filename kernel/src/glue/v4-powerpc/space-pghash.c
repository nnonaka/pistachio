/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     glue/v4-powerpc/space-pghash.c
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

/* Upstream carries no includes at all in this file, yet master's Makeconf
   lists it in SOURCES -- so it has never been a translation unit that could
   compile, in either language.  The block below is the one its sibling
   space-swtlb.c carries, with the arch header swapped for the page-hash one.
   Notes §144. */

#include <debug.h>
#include <kmemory.h>
#include <generic/lib.h>
#include <linear_ptab.h>
#include <kdb/tracepoints.h>

#include INC_API(tcb.h)
#include INC_API(kernelinterface.h)

#include INC_GLUE(space.h)
#include INC_GLUE(syscalls.h)
#include INC_GLUE(pghash.h)
#include INC_GLUE(memcfg.h)	/* cpu_phys_area */

#include INC_ARCH(pghash.h)
#include INC_ARCH(except.h)
#include INC_ARCH(phys.h)
#include INC_ARCH(bat.h)
#include INC_GLUE(bat.h)

/* Defined in space.c, which counts the same two events. */
EXTERN_TRACEPOINT(hash_miss_cnt);
EXTERN_TRACEPOINT(hash_insert_cnt);

bool space_handle_hash_miss (space_t *self, addr_t vaddr)
{
    pgent_t *pgent;

    /* Upstream writes TRACEPOINT(hash_miss_cnt) with no format argument;
       the macro takes (tp, str, ...) and rejects that in either language.  An
       empty format keeps it to the counter bump the bare call was after. */
    TRACEPOINT(hash_miss_cnt, "");

    pgent = space_page_lookup (self, vaddr);
    if( !pgent || !pgent_is_valid (pgent, self, size_4k) )
	return false;

    TRACEPOINT(hash_insert_cnt, "");

    pghash_insert_4k_mapping (get_pghash(), self, vaddr, pgent);
    return true;
}

#include INC_PLAT(ofppc.h)

void SECTION(".init.memory") space_init_kernel_mappings (space_t *self)
{
    addr_t page;

#if !defined(CONFIG_PPC_BAT_SYSCALLS)
    mem_region_t syscall_region;

    /* Figure out the range of the syscall code to be mapped into the user
     * space.
     */
    syscall_region.low = addr_align( (addr_t)ofppc_syscall_start(), 
	    POWERPC_PAGE_SIZE );
    syscall_region.high = addr_align_up( (addr_t)ofppc_syscall_end(), 
	    POWERPC_PAGE_SIZE );

    /* Create mappings for the system calls, so user space can access them
     * (after a sync_kernel_space()).
     */
    page = syscall_region.low;
    while( page < syscall_region.high ) 
    {
	space_add_mapping (self, page, (paddr_t)virt_to_phys(page), size_4k, false, true, true);
	page = addr_offset( page, POWERPC_PAGE_SIZE );
    }
#endif

    /* Create user-visible mappings for the KIP, so that the SystemClock 
     * system-call can read the processor speed from user-level.  This 
     * mapping obviously is not intended to be directly accessed by 
     * applications.
     */
    for( page = (addr_t)get_kip();
	    page != ofppc_kip_end();
	    page = addr_offset(page, POWERPC_PAGE_SIZE) )
    {
	space_add_mapping (self, page, (paddr_t)virt_to_phys(page), size_4k, false, true, true);
    }
}

void SECTION(".init.memory") space_init_cpu_mappings (space_t *self, cpuid_t cpu)
{
     ppc_bat_t bat;
     bat.raw.upper = bat.raw.lower = 0;
     bat.x.bepi = KERNEL_CPU_OFFSET >> BAT_BEPI;
     bat.x.bl = BAT_BL_128K;
     bat.x.vs = 1;
     bat.x.brpn = cpu_phys_area(cpu) >> BAT_BRPN;
     bat.x.m = 0;	/* We don't need memory coherency. */
     bat.x.pp = BAT_PP_READ_WRITE;
     ppc_set_cpu_dbat( l, bat.raw.lower );
     ppc_set_cpu_dbat( u, bat.raw.upper );
     isync();
}

bool space_sync_kernel_space (space_t *self, addr_t addr)
{
    word_t pdir_idx;
    pgent_t *our_pgent, *kernel_pgent;

    if( self == get_kernel_space() )
	return false;

    pdir_idx = page_table_index( size_4m, addr );
    our_pgent = space_pgent (self, pdir_idx);
    kernel_pgent = space_pgent (get_kernel_space(), pdir_idx);

    /* If the target space already has a page entry for this address,
     * or if it is an invalid kernel page description, then
     * return false.
     */
    if( pgent_is_valid (our_pgent, self, size_4m) ||
	    !pgent_is_valid (kernel_pgent, get_kernel_space(), size_4m) )
	return false;

    /* Copy the kernel mapping to the target space.
     */
    our_pgent->raw = kernel_pgent->raw;
    return true;
}

/**
 * space_init initializes the space_t
 *
 * maps the kernel area and initializes shadow ptabs etc.
 */
void space_init (space_t *self, fpage_t utcb_area, fpage_t kip_area)
{
    /* Copy the kernel area's page directory into the user's page directory.
     * This is an optimization.  It could be done lazily instead.
     * TODO: how much should we precopy?  (this also preloads the page hash).
     */
    addr_t addr = (addr_t)KTCB_AREA_START;
    while( addr < (addr_t)KTCB_AREA_END ) {
	space_sync_kernel_space (self, addr);
	addr = addr_offset( addr, PPC_PAGEDIR_SIZE );
    }

    self->utcb_area = utcb_area;
    self->kip_area = kip_area;
    space_add_mapping (self, fpage_get_base (&kip_area), (paddr_t)virt_to_phys(get_kip()),
		       size_4k, false, false, cache_standard);
}

void space_flush_tlb (space_t *self, space_t *curspace)
{
    // TODO: flush the tlb for a given address space.
    ppc_invalidate_tlb();
}

void space_flush_tlbent (space_t *self, space_t *curspace, addr_t addr,
			 word_t log2size)
{
    ppc_invalidate_tlbe( addr );
}

/* space-swtlb.c carries these two as well.  Neither is specific to a software
   TLB -- one walks transtable[], which space.c defines for both halves, and
   the other only maps an address range onto a cache_e -- but the two files are
   the mutually exclusive halves of space_t's MMU-specific API and exactly one
   of them is ever compiled, so the pghash half has to supply its own.
   Upstream defines them only in the swtlb half, which is why linking a
   segment-MMU kernel leaves both unresolved.  Notes §144. */

paddr_t space_sigma0_translate (addr_t addr, word_t size)
{
    word_t i;
    paddr_t paddr = (paddr_t)addr;

    for (i = 0; i < TRANSLATION_TABLE_ENTRIES; ++i) {
	if ((transtable[i].size > 0) && ((word_t)addr >= transtable[i].s0addr) &&
		((word_t)addr <= transtable[i].s0addr + transtable[i].size - 1)) {
	    paddr = transtable[i].physaddr + (word_t)addr_offset(addr, -transtable[i].s0addr);
	    transtable[i].size = 0;
	    break;
	}
    }
    return paddr;
}

word_t space_sigma0_attributes (pgent_t *pg, paddr_t addr, word_t size)
{
    /* device memory is guarded */
    if (addr >= 0x100000000ULL)
	return cache_inhibited;
    else
	return cache_standard;
}

DECLARE_TRACEPOINT(PPC_EXCEPT_ISI);
DECLARE_TRACEPOINT(PPC_EXCEPT_DSI);

/* Upstream calls try_to_debug() at the head of both handlers.  That function
   is INLINE -- static inline -- in except_handlers.c and declared in no
   header, so the calls have never resolved from this translation unit, in
   either language.  They are dropped rather than given a declaration that
   would change what the debugger sees: the swtlb handlers, which are the ones
   that do compile, make no such call either.  Notes §144. */

EXCDEF( isi_handler )
{
    space_t *space;

    TRACEPOINT(PPC_EXCEPT_ISI, "except_isi_cnt");

    space = tcb_get_space (get_current_tcb());
    if( EXPECT_FALSE(space == NULL) )
	space = get_kernel_space();

    if( EXCEPT_IS_ISI_MISS(srr1) ) 
	if( EXPECT_TRUE(space_handle_hash_miss (space, (addr_t)srr0)) )
	    return_except();

    space_handle_pagefault (space, (addr_t)srr0, (addr_t)srr0,
	    SPACE_ACCESS_EXECUTE, ppc_is_kernel_mode(srr1) );

    return_except();
}

EXCDEF( dsi_handler )
{
    word_t dar = ppc_get_spr(SPR_DAR);
    word_t dsisr = ppc_get_spr(SPR_DSISR);
    tcb_t *tcb;
    space_t *space;

    TRACEPOINT(PPC_EXCEPT_DSI, "except_dsi_cnt");

    tcb = get_current_tcb();
    space = tcb_get_space (tcb);
    if( EXPECT_FALSE(space == NULL) )
	space = get_kernel_space();

    // Do we have a page hash miss?
    if( EXCEPT_IS_DSI_MISS(dsisr) )
    {

	// Is the page hash miss in the copy area?
	if( EXPECT_FALSE(space_is_copy_area((addr_t)dar)) )
	{
	    // Resolve the fault using the partner's address space!
	    tcb_t *partner = tcb_get_tcb( tcb_get_partner (tcb) );
	    if( partner )
	    {
		addr_t real_fault = tcb_copy_area_real_address (tcb, (addr_t)dar);
		if( space_handle_hash_miss (tcb_get_space (partner), real_fault) )
	    	    return_except();
	    }
	}

	// Normal page hash miss.
	if( EXPECT_TRUE(space_handle_hash_miss (space, (addr_t)dar)) )
	    return_except();
    }

    space_handle_pagefault (space, (addr_t)dar, (addr_t)srr0,
	    EXCEPT_IS_DSI_WRITE(dsisr) ?  SPACE_ACCESS_WRITE : SPACE_ACCESS_READ,
	    ppc_is_kernel_mode(srr1) );

    return_except();
}
