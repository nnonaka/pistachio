/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     glue/v4-powerpc/space-swtlb.cc
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
#include <kmemory.h>
#include <generic/lib.h>
#include <linear_ptab.h>
#include <kdb/tracepoints.h>

#include INC_API(tcb.h)
#include INC_API(kernelinterface.h)

#include INC_GLUE(space.h)
#include INC_GLUE(syscalls.h)

#include INC_ARCH(swtlb.h)
#include INC_ARCH(phys.h)

EXTERN_KMEM_GROUP(kmem_utcb);
EXTERN_KMEM_GROUP(kmem_tcb);
EXTERN_KMEM_GROUP(kmem_pgtab);
EXTERN_KMEM_GROUP(kmem_space);

//#define TRACE_TLB(x...)	TRACEF(x)
#define TRACE_TLB(x...)

word_t space_pinned_mapping;
// used by initialization code...
word_t swtlb_high_water;

#ifdef CONFIG_SMP
// XXX: move into init data section!
static struct {
    ppc_tlb0_t tlb0;
    ppc_tlb1_t tlb1;
    ppc_tlb2_t tlb2;
} init_swtlb[PPC_MAX_TLB_ENTRIES];
#endif

// tlb index for replacement
UNIT("cpulocal") ppc_swtlb_t swtlb;

// ASID management
UNIT("cpulocal") asid_manager_t asid_manager;

#if 0
void dump_tlb()
{
    for (int i = 0; i < PPC_MAX_TLB_ENTRIES; i++)
    {
	ppc_tlb0_t tlb0;
	ppc_tlb1_t tlb1;
	ppc_tlb2_t tlb2;
	ppc_mmucr_t mmucr;
	ppc_tlb0_read (&tlb0, i);
	ppc_tlb1_read (&tlb1, i);
	ppc_tlb2_read (&tlb2, i);
	ppc_mmucr_read (&mmucr);

	printf("%02d: %c [%02x:%d] %08x sz:%08x [%04x:%08x] U:%c%c%c S:%c%c%c  C:[%c%c%c%c%c]\n",
	       i, ppc_tlb0_is_valid (&tlb0) ? 'V' : 'I', ppc_mmucr_get_search_id (&mmucr),
	       tlb0.trans_space, ppc_tlb0_get_vaddr (&tlb0), ppc_tlb0_get_size (&tlb0),
	       (word_t)(ppc_tlb1_get_paddr (&tlb1) >> 32), (word_t)(ppc_tlb1_get_paddr (&tlb1)),
	       tlb2.user_execute ? 'X' : '-', tlb2.user_write ? 'W' : '-', 
	       tlb2.user_read ? 'R' : '-', tlb2.super_execute ? 'X' : '-', 
	       tlb2.super_write ? 'W' : '-', tlb2.super_read ? 'R' : '-',
	       tlb2.write_through ? 'W' : '-', tlb2.inhibit ? 'I' : '-',
	       tlb2.mem_coherency ? 'M' : '-', tlb2.guarded ? 'G' : '-',
	       tlb2.endian ? 'E' : '-');
    }
}
#endif

bool space_sync_kernel_space (space_t *self, addr_t addr)
{
    /* nothing to sync; we handle the kernel space in the TLB miss
     * handler */
    return false;
}

void space_init (space_t *self, fpage_t utcb_area, fpage_t kip_area)
{
	int i;
	self->utcb_area = utcb_area;
    self->kip_area = kip_area;

    space_add_mapping (self,  fpage_get_base (&kip_area), (paddr_t)virt_to_phys(get_kip()), 
		       size_4k, false, false, cache_standard );

    // XXX: do upon migration!
    for (i = 0; i < CONFIG_SMP_MAX_CPUS; i++)
	asid_init (space_get_asid_cpu (self, i));
}

void SECTION(".init.memory") space_init_kernel_mappings (space_t *self)
{
    int i;
    space_pinned_mapping = PINNED_AREA_START;

    //initialize translation table
    for (i=0; i < TRANSLATION_TABLE_ENTRIES; ++i) {
    	transtable[i].s0addr = 0;
    	transtable[i].physaddr = 0;
    	transtable[i].size = 0;
    }

}

void SECTION(".init.memory") space_init_cpu_mappings (space_t *self, cpuid_t cpu)
{
    extern char _begin_cpu_local[], _end_cpu_local[];
    extern char _cpu_phys[];
    ppc_tlb0_t tlb0;
    ppc_tlb1_t tlb1;
    ppc_tlb2_t tlb2;
    addr_t page;

    // determine size
    int log2size = 10;
    for (; (1 << log2size) < (_end_cpu_local - _begin_cpu_local) || 
	     !ppc_tlb0_is_valid_pagesize(log2size); log2size++);
    
    if (cpu == 0)
	page = phys_to_virt(_cpu_phys);
    else
    {
	page = kmem_alloc(&kmem, kmem_pgtab, 1 << log2size);
	memcpy(page, phys_to_virt(_cpu_phys), 1 << log2size);
    }

    TRACE_INIT("\tMapping %p/%p -> %p, log2sz=%d, TLB entry: %d (CPU %d)\n", 
	       page, virt_to_phys(page), CPU_AREA_START, log2size, 
	       swtlb_high_water, cpu);

    ppc_tlb0_init_vaddr_size (&tlb0, CPU_AREA_START, log2size, true, 0);
    ppc_tlb1_init_paddr (&tlb1, (paddr_t)virt_to_phys(page));
    //ppc_tlb2_init_cpu_local (&tlb2);
    ppc_tlb2_init_shared_smp (&tlb2);
    ppc_tlb2_set_kernel_perms (&tlb2, true, true, false);

    ppc_mmucr_write_search_id(0, 0);
    ppc_tlb0_write (&tlb0, swtlb_high_water);
    ppc_tlb1_write (&tlb1, swtlb_high_water);
    ppc_tlb2_write (&tlb2, swtlb_high_water);

    /* 
     * CPU local mappings exist now 
     */
    TRACE_INIT("\tASID manager init %x -> %x (CPU %d)\n", 1, CONFIG_MAX_NUM_ASIDS-1, cpu);
    asid_manager_init (&asid_manager, 1, CONFIG_MAX_NUM_ASIDS - 1);
    ASSERT(self == get_kernel_space());
    asid_init_kernel (&self->cpu[cpu].asid, 0);
    ppc_swtlb_init (&swtlb, swtlb_high_water - 1);

#ifdef CONFIG_SMP
    /* safe all kernel mappings except CPU local */
    if (cpu == 0)
    {
	for (unsigned idx = swtlb_high_water + 1; idx < PPC_MAX_TLB_ENTRIES; idx++)
	{
	    ppc_tlb0_read (&init_swtlb[idx].tlb0, idx);
	    ppc_tlb1_read (&init_swtlb[idx].tlb1, idx);
	    ppc_tlb2_read (&init_swtlb[idx].tlb2, idx);
	    TRACEF("\tTLB%d: %lx, %lx, %lx\n", idx, init_swtlb[idx].tlb0.raw,
		   init_swtlb[idx].tlb1.raw, init_swtlb[idx].tlb2.raw);
	}
    }

    current_cpu = cpu;

#endif
}

NOINLINE bool space_handle_tlb_miss (space_t *self, addr_t lookup_vaddr, addr_t install_vaddr, bool user, bool global)
{
    pgent_t * pg;
    word_t pgsize;

    /* check kernel fault for device mappings */
    TRACE_TLB("handle_tlb_miss %p, %p, %s\n", 
	      lookup_vaddr, install_vaddr, user ? "user" : "kernel");

    if (! space_lookup_mapping (self, lookup_vaddr, &pg, &pgsize, 0))
        return false;

    size_t  size  = page_shift (pgsize);
    word_t  vaddr = (word_t) install_vaddr;
    paddr_t paddr = pgent_address (pg, self, pgsize) | (vaddr & ((1ul << size) - 1));

    while (!ppc_tlb0_is_valid_pagesize (size))
        size--;

    vaddr &= ~((1ul << size) - 1);
    paddr &= ~((1ull << size) - 1);

    TRACE_TLB("mapping found (%p): %p -> %lx (%x)\n",
	      pg, lookup_vaddr, (word_t)paddr, size);
    TRACE_TLB("[%c%c%c], cache=%x, erpn=%x\n", 
	      pg->map.read ? 'R' : ' ', pg->map.write ? 'W' : ' ',
	      pg->map.execute ? 'X' : ' ', pg->map.caching, pg->map.erpn);

    ppc_tlb0_t tlb0;
    ppc_tlb1_t tlb1;
    ppc_tlb2_t tlb2;

    ppc_tlb0_init_vaddr_size (&tlb0, vaddr, size, true, 0);
    ppc_tlb1_init_paddr (&tlb1, paddr);

    switch (pg->map.caching)
    {
    case cache_standard:  ppc_tlb2_init_shared_smp (&tlb2); break;
    case cache_inhibited: ppc_tlb2_init_device (&tlb2); break;
    case cache_guarded: ppc_tlb2_init_guarded (&tlb2); break;
    default: UNIMPLEMENTED();
    }
    if (user)
	ppc_tlb2_set_user_perms (&tlb2, pg->map.read, pg->map.write, pg->map.execute);
    ppc_tlb2_set_kernel_perms (&tlb2, pg->map.read, pg->map.write, 0);

    ppc_mmucr_write_search_id(global ? 0 : ppc_get_pid(), 0);
    word_t tlb_index = ppc_swtlb_allocate (&swtlb);

    TRACE_TLB("inserting TLB entry %d: %08x, %08x, %08x\n",
	      tlb_index, tlb0.raw, tlb1.raw, tlb2.raw);

    ppc_tlb0_write (&tlb0, tlb_index);
    ppc_tlb1_write (&tlb1, tlb_index);
    ppc_tlb2_write (&tlb2, tlb_index);

    return true;
}

addr_t space_map_device_pinned (space_t *self, paddr_t paddr, word_t size, bool kernel, word_t attrib)
{
    word_t log2sz;
    word_t vaddr = space_pinned_mapping;

    for (log2sz = 1; log2sz < 32; log2sz++)
	if ((1U << log2sz) >= size && ppc_tlb0_is_valid_pagesize(log2sz))
	    break;

    if (log2sz >= 32)
	return NULL;

    size = 1 << log2sz;

    paddr_t paddr_align = paddr & ~((paddr_t)size - 1);
    /* Parenthesised: `==' binds tighter than `&', so this was vaddr & 1 -- the
       mapping was aligned up only for an odd vaddr, never for a merely
       size-misaligned one, and ppc_tlb0_init_vaddr_size needs the alignment.
       Upstream, and the same in the C++.  Notes §140. */
    if ((vaddr & (size - 1)) != 0)
	vaddr = (vaddr + size) & ~(size - 1);

    ppc_tlb0_t tlb0;
    ppc_tlb1_t tlb1;
    ppc_tlb2_t tlb2;

    ppc_tlb0_init_vaddr_size (&tlb0, vaddr, log2sz, true, 0);
    ppc_tlb1_init_paddr (&tlb1, paddr_align);
    ppc_tlb2_init_device (&tlb2);
    ppc_tlb2_set_kernel_perms (&tlb2, true, true, false);
    if (!kernel)
        ppc_tlb2_set_user_perms (&tlb2, true, true, false);

    word_t tlb_index = ppc_swtlb_allocate_pinned (&swtlb);
    ppc_tlb0_write (&tlb0, tlb_index);
    ppc_tlb1_write (&tlb1, tlb_index);
    ppc_tlb2_write (&tlb2, tlb_index);

    TRACE_TLB("mapping pinned device: %x.%08x, sz=%x, %x.%08x, [%08x, %08x, %08x]\n",
	      (word_t)(paddr >> 32), (word_t)paddr, size, (word_t)(paddr_align >> 32),
	      (word_t)paddr_align, tlb0.raw, tlb1.raw, tlb2.raw);

    space_pinned_mapping = vaddr + size;

    return addr_offset((addr_t)vaddr, paddr - paddr_align);
}

asid_t *space_get_asid (space_t *self)
{
    return space_get_asid_cpu (self, get_current_cpu());
}

void space_allocate_asid (space_t *self)
{
    asid_manager_allocate_asid (&asid_manager, self);
}

void space_flush_tlb (space_t *self, space_t *curspace)
{
    space_flush_tlb_range (self, curspace, (addr_t)0, (addr_t)~0U);
}

void space_flush_tlb_range (space_t *self, space_t *curspace, addr_t start, addr_t end)
{
    asid_t *asid = space_get_asid (self);
    if (!asid_is_valid (asid))
	return;

    TRACEF("flush_tlb %p, [%p-%p]\n", self, start, end);

    word_t hw_asid = asid_get (asid);
    for (word_t idx = 0; idx < swtlb.high_water; idx++)
    {
	ppc_tlb0_t tlb0;
	ppc_mmucr_t mmucr;
	ppc_tlb0_read (&tlb0, idx);
	ppc_mmucr_read (&mmucr);

	if (!ppc_tlb0_is_valid (&tlb0))
	    continue;

	if (ppc_mmucr_get_search_id (&mmucr) == hw_asid &&
	    (addr_t)ppc_tlb0_get_vaddr (&tlb0) >= start && 
	    addr_offset((addr_t)ppc_tlb0_get_vaddr (&tlb0), ppc_tlb0_get_size (&tlb0) - 1) <= end )
	{
	    {
		ppc_tlb0_t inv = ppc_tlb0_invalid ();
		ppc_tlb0_write (&inv, idx);
	    }
	    ppc_swtlb_set_free (&swtlb, idx);
	}
    }
}

void space_flush_tlbent (space_t *self, space_t *curspace, addr_t addr, word_t log2size)
{
    asid_t *asid = space_get_asid (self);
    if (!asid_is_valid (asid))
	return;

    word_t idx;
    ppc_mmucr_write_search_id(asid_get (asid), 0);
    isync();

    if (ppc_tlbsx((word_t)addr, &idx))
    {
	TRACEF("invalidating TLB entry %d\n", idx);
	{
	    ppc_tlb0_t inv = ppc_tlb0_invalid ();
	    ppc_tlb0_write (&inv, idx);
	}
	ppc_swtlb_set_free (&swtlb, idx);
    }
}

void space_arch_free (space_t *self)
{
    space_flush_tlb_range (self, self, (addr_t)USER_AREA_START, (addr_t)USER_AREA_END);
}

/* Unused since space_sigma0_translate below became a loop over transtable[];
   kept, but no longer C++.  Both this and the static_cast above survived the
   conversion because their only expansion is inside a TRACE_TLB that expands to
   nothing, so neither was ever parsed as an expression.  Notes §140. */
#define RELOC(s0addr, physaddr, size) \
    case s0addr ... s0addr + size - 1: paddr = physaddr + (paddr_t)(word_t)addr_offset(addr, -s0addr); break;

paddr_t space_sigma0_translate (addr_t addr, word_t size)
{
	word_t i;
	paddr_t paddr = (paddr_t)addr;
    for (i = 0; i < TRANSLATION_TABLE_ENTRIES; ++i) {
    	if ((transtable[i].size > 0) && ((word_t)addr >= transtable[i].s0addr) && ((word_t)addr <= transtable[i].s0addr + transtable[i].size - 1)) {
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
    //if (sigma0_translate(addr, size) >= 0x100000000ULL)
	if (addr >= 0x100000000ULL)
	return cache_inhibited;
    else
	return cache_standard;
}

/**********************************************************************
 *		    Paging initialization; unpaged mode!
 **********************************************************************/

EXTERN_C SECTION(".einit") void init_paging( int cpu )
{
    u32_t curr_entry;
    word_t index;

    /* switch to global space */
    ppc_set_pid(0);

    /* set search id to global */
    ppc_mmucr_write_search_id(0, 0);

    ppc_tlbsx((u32_t)&init_paging, &curr_entry); /* can't fail */

    // Clear out all mappings except for the one we run on
    for (index = 0; index < PPC_MAX_TLB_ENTRIES; index++)
    {
	if (index == curr_entry)
	    continue;
	{
	    ppc_tlb0_t inv = ppc_tlb0_invalid ();
	    ppc_tlb0_write (&inv, index);
	}
    }

    ppc_tlb0_t tlb0;
    ppc_tlb1_t tlb1;
    ppc_tlb2_t tlb2;
    ppc_mmucr_t mmucr;
    word_t log2size;

    // initialize MMUCR
    mmucr.raw = 0;
#ifdef CONFIG_SUBPLAT_440_BGP
    mmucr.u2_store_without_allocate = 1;
#endif
    ppc_mmucr_write (&mmucr);

    for (log2size = KERNEL_AREA_LOG2SIZE; 
         !ppc_tlb0_is_valid_pagesize(log2size);
         log2size--);

    index = PPC_MAX_TLB_ENTRIES - 1;

    ppc_tlb0_init_vaddr_size (&tlb0, KERNEL_OFFSET, log2size, true, 0);
    ppc_tlb1_init_paddr (&tlb1, 0ULL);
    ppc_tlb2_init_shared_smp (&tlb2);
    ppc_tlb2_set_kernel_perms (&tlb2, true, true, true);
    ppc_tlb2_set_user_perms (&tlb2, false, false, true);

    // map the kernel area
    for (; ppc_tlb0_get_vaddr (&tlb0) < KERNEL_AREA_END; 
	 ppc_tlb0_add_offset (&tlb0, 1 << log2size),
	     ppc_tlb1_add_offset (&tlb1, 1 << log2size), index--)
    {
	if (index == curr_entry)
	    index--;
	ppc_tlb0_write (&tlb0, index);
	ppc_tlb1_write (&tlb1, index);
	ppc_tlb2_write (&tlb2, index);
    }
    isync();

    /*
     * Paging is on now and we can access kernel data
     */
    if (cpu == 0)
	swtlb_high_water = index;
    else
    {
#ifdef CONFIG_SMP
	for (unsigned idx = swtlb_high_water + 1; idx < PPC_MAX_TLB_ENTRIES; idx++)
	{
	    ppc_tlb0_write (&init_swtlb[idx].tlb0, idx);
	    ppc_tlb1_write (&init_swtlb[idx].tlb1, idx);
	    ppc_tlb2_write (&init_swtlb[idx].tlb2, idx);
	}
	isync();
#endif
    }
}

#if defined(CONFIG_TRACEBUFFER)
FEATURESTRING ("tracebuffer");
tracebuffer_t * tracebuffer;
EXTERN_KMEM_GROUP (kmem_misc);
void setup_tracebuffer (void)
{
    tracebuffer = (tracebuffer_t *) kmem_alloc(&kmem, kmem_misc, TRACEBUFFER_SIZE);
    if (!tracebuffer)
        return;
    
    addr_t vaddr = space_map_device_pinned (get_kernel_space_c (),
					    virt_to_phys((paddr_t)tracebuffer),
					    TRACEBUFFER_SIZE, false, cache_standard);
    /* The four-argument memory_info_t::insert forwarded to the five-argument
       one with subtype 0; only the latter has a C form. */
    memory_info_insert (&get_kip()->memory_info, MEMDESC_RESERVED, 0, true, vaddr,
			addr_offset(vaddr, TRACEBUFFER_SIZE -1));

    tracebuffer_initialize (tracebuffer);
}
#endif /* CONFIG_TRACEBUFFER */

addr_t setup_console_mapping(paddr_t paddr, int log2size)
{
    static word_t console_area = CONSOLE_AREA_START;

    word_t size = 1 << log2size;
    word_t vaddr = console_area;
    paddr_t paddr_align = paddr & ~((paddr_t)size - 1);

    /* Parenthesised; see space_map_device_pinned above. */
    if ((vaddr & (size - 1)) != 0)
	vaddr = (vaddr + size) & ~(size - 1);

    ppc_tlb0_t tlb0;
    ppc_tlb1_t tlb1;

    ppc_tlb0_init_vaddr_size (&tlb0, vaddr, log2size, true, 0);
    ppc_tlb1_init_paddr (&tlb1, paddr_align);
    ppc_tlb2_t tlb2;
    ppc_tlb2_init_device (&tlb2);
    ppc_tlb2_set_kernel_perms (&tlb2, true, true, false);

    ppc_mmucr_write_search_id(0, 0);
    ppc_tlb0_write (&tlb0, swtlb_high_water);
    ppc_tlb1_write (&tlb1, swtlb_high_water);
    ppc_tlb2_write (&tlb2, swtlb_high_water);
    isync();

    swtlb_high_water--;
    console_area = vaddr + size;

    return addr_offset((addr_t)vaddr, paddr - paddr_align);
}

SECTION(".init") void setup_kernel_mappings( void )
{
    /* flush boot mapping */
    u32_t entry;
    ppc_tlb0_t tlb0;
    ppc_tlbsx((u32_t)&init_paging, &entry);
    ppc_tlb0_read (&tlb0, entry);

    TRACE_INIT("Flush boot mapping %x, vaddr=%x, size=%x (%x)\n", 
               entry, ppc_tlb0_get_vaddr (&tlb0), ppc_tlb0_get_size (&tlb0), tlb0.raw);
    {
	ppc_tlb0_t inv = ppc_tlb0_invalid ();
	ppc_tlb0_write (&inv, entry);
    }
}

/**********************************************************************
 * Exception handlers
 **********************************************************************/

DECLARE_TRACEPOINT(PPC_DTLB_MISS);
DECLARE_TRACEPOINT(PPC_ITLB_MISS);
DECLARE_TRACEPOINT(PPC_EXCEPT_ISI);
DECLARE_TRACEPOINT(PPC_EXCEPT_DSI);

EXCDEF( isi_handler )
{
    TRACEPOINT(PPC_EXCEPT_ISI,
	       "ISI MISS: IP: %08x", srr0);
    
    space_t *space = get_current_space();
    ASSERT(space);

    space_handle_pagefault (space,  (addr_t)srr0, (addr_t)srr0, 
	    SPACE_ACCESS_EXECUTE, ppc_is_kernel_mode(srr1) );

    return_except();
}

EXCDEF( dsi_handler )
{
    word_t dear = ppc_get_spr(SPR_DEAR);
    ppc_esr_t esr;
    ppc_esr_read (&esr);

    TRACEPOINT(PPC_EXCEPT_DSI,
	       "DSI MISS: IP: %08x, ESR: %08x, DEAR: %p", 
	       srr0, esr.raw, dear);

    tcb_t *tcb = get_current_tcb();
    space_t *space = tcb_get_space (tcb);
    ASSERT(space);

    space_handle_pagefault (space,  (addr_t)dear, (addr_t)srr0, 
	    esr.x.store ?  SPACE_ACCESS_WRITE : SPACE_ACCESS_READ,
	    ppc_is_kernel_mode(srr1) );

    return_except();
}

EXCDEF( dtlb_miss_handler )
{
    addr_t dear = (addr_t)ppc_get_spr(SPR_DEAR);
    bool kernel_mode = ppc_is_kernel_mode(srr1);
    ppc_esr_t esr; ppc_esr_read (&esr);
    bool user = false;
    
    
    TRACEPOINT(PPC_DTLB_MISS,
	       "DTLB MISS: IP: %08x, ESR: %08x, DEAR: %p", 
	       srr0, esr.raw, dear);

    tcb_t *tcb = get_current_tcb();
    space_t *space = tcb_get_space (tcb);
    if (!space) 
    {
        ASSERT(space_is_kernel_paged_area (dear));
        space = get_kernel_space();
    }
   
    if (space_is_user_area (dear))
    {
	if ( EXPECT_TRUE(space_handle_tlb_miss (space, dear, dear, true, false)) )
	    return_except();
	user = true;
    }
    else if (EXPECT_FALSE(kernel_mode))
    {
	if (space_is_kernel_paged_area (dear))
	{
	    if (!space_handle_tlb_miss (get_kernel_space(), dear, dear, user, true))
		panic("kernel accessed unmapped device @ %08x (IP=%08x)", dear, srr0);
	    return_except();
	}
	else if (space_is_copy_area (dear))
	{
	    // Resolve the fault using the partner's address space!
	    tcb_t *partner = tcb_get_tcb (tcb_get_partner (tcb));
	    if( partner )
	    {
		addr_t real_fault = tcb_copy_area_real_address (tcb,  (addr_t)dear );
		TRACE_TLB("copy area DTLB miss: %p -> %p\n", dear, real_fault);
		if (!space_handle_tlb_miss (tcb_get_space (partner), real_fault, dear, user, false))
		{
		    space_handle_pagefault (space, dear, (addr_t)srr0, SPACE_ACCESS_WRITE, 
					    ppc_is_kernel_mode(srr1));
		    space_handle_tlb_miss (tcb_get_space (partner), real_fault, dear, user, false);
		}
		return_except();
	    }
	}
	else 
	{
	    printf("kernel page fault @ %08x?", dear);
	    enter_kdebug("unhandled kernel TLB miss");
	}
    }

    space_handle_pagefault (space,  dear, (addr_t)srr0,
			     esr.x.store ?  SPACE_ACCESS_WRITE : SPACE_ACCESS_READ,
			     ppc_is_kernel_mode(srr1) );

    // pro-actively try to load the TLB; if miss wasn't fulfilled we
    // are in trouble anyhow...
    space_handle_tlb_miss (space, dear, dear, user, false);

    return_except();
}

EXCDEF( itlb_miss_handler )
{
    ppc_esr_t esr; 
    ppc_esr_read (&esr);

    TRACEPOINT(PPC_ITLB_MISS,
	       "ITLB MISS: IP: %08x, LR: %08x, ESR: %08x",
	       srr0, frame->lr, esr.raw);

    space_t *space = tcb_get_space (get_current_tcb());

    ASSERT(space);
    ASSERT(!ppc_is_kernel_mode(srr1));

    // first try to refill TLB
    if ( EXPECT_FALSE(!space_handle_tlb_miss (space, (addr_t)srr0, (addr_t)srr0, true, false)) )
    {
	space_handle_pagefault (space,  (addr_t)srr0, (addr_t)srr0,
				 SPACE_ACCESS_EXECUTE, ppc_is_kernel_mode(srr1) );

	// pro-actively try to load the TLB; if miss wasn't fulfilled we
	// are in trouble anyhow...
	space_handle_tlb_miss (space, (addr_t)srr0, (addr_t)srr0, true, false);
    }

    return_except();
}
