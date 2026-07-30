/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     glue/v4-powerpc/softhvm.cc
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
#include <lib.h>		/* min() */
#include <debug.h>
#include <linear_ptab.h>
#include <kdb/tracepoints.h>

#include INC_ARCH(phys.h)
#include INC_ARCH(softhvm.h)
#include INC_ARCH(pgent.h)

#include INC_API(tcb.h)
#include INC_API(schedule.h)
#include INC_API(kernelinterface.h)

#include INC_GLUE(exception.h)

#define TRACE_EMUL(args...)
#define MAX_INSTR_EMULATE	10

extern ppc_swtlb_t swtlb; /* defined in space-swtlb.cc */


#include INC_API(schedule.h)	/* sched_* entry points */

DECLARE_TRACEPOINT(PPC_HVM_EXCEPT_PROG);
DECLARE_TRACEPOINT(PPC_HVM_EXCEPT_DECR);
DECLARE_TRACEPOINT(PPC_HVM_DTLB_MISS);
DECLARE_TRACEPOINT(PPC_HVM_ITLB_MISS);
DECLARE_TRACEPOINT(PPC_HVM_EXCEPT_ISI);
DECLARE_TRACEPOINT(PPC_HVM_EXCEPT_DSI);
DECLARE_TRACEPOINT(PPC_HVM_ALIGN);
DECLARE_TRACEPOINT(PPC_HVM_EMUL_INTERPRET);

INLINE void try_to_debug( except_regs_t *regs, word_t exc_no, word_t dar, word_t dsisr )
{
    if( EXPECT_TRUE(get_kip()->kdebug_entry == NULL) )
	return;

    debug_param_t param;

    param.exception = exc_no;
    param.frame = regs;
    param.tcb = get_current_tcb();
    param.space = (get_current_space() ? get_current_space() : get_kernel_space());
    param.dar = dar;
    param.dsisr = dsisr;

    get_kip()->kdebug_entry( (void *)&param );
#ifdef CONFIG_KDB_BREAKIN
    kdebug_check_breakin();
#endif
}

/* assumption: running in context of HVM, IP is guest-virtual */
INLINE bool read_hvm_instruction(word_t ip, word_t *instr, bool speculative)
{
    word_t origmsr, newmsr, idx;

    if (EXPECT_FALSE(speculative))
    {
	ppc_mmucr_write_search_id(2, 1);
	if (!ppc_tlbsx(ip, &idx))
	    return false;
    }

    /* read the instruction */
    asm volatile ("mfmsr %[origmsr]\n"
		  "ori %[newmsr], %[origmsr], %[dts1]\n"
		  "mtmsr %[newmsr]\n"
		  "isync\n"
		  "lwz %[instr], 0(%[ip])\n"
		  "mtmsr %[origmsr]\n"
		  "isync\n"
		  /* `*instr', not `instr': this was a word_t& out-parameter, and
		     writing the bare pointer leaves the caller's word unset --
		     which is what -Wmaybe-uninitialized reported at both call
		     sites.  Same defect as ppc_tlbsx.  Notes §140. */
		  : [instr] "=r" (*instr), [origmsr] "=&r" (origmsr), [newmsr] "=&r"(newmsr)
		  : [dts1] "i"(1 << MSR_DS), [ip] "b" (ip));
    return true;
}

NOINLINE bool 
arch_ktcb_send_hvm_fault (arch_ktcb_t *self, enum softhvm_exit_reason_e exc, except_regs_t *frame, word_t instr, 
			    word_t param, bool internal)
{
    const int untyped = 4;
    tcb_t *tcb = addr_to_tcb(self);
    acceptor_t acceptor;

    msg_tag_t tag = softhvm_fault_tag(exc, untyped, internal);
    tag.x.typed = tcb_append_ctrlxfer_item (tcb, tag, untyped + 1);
    tcb_set_tag (tcb, tag);
    tcb_set_mr (tcb, 1, instr);
    tcb_set_mr (tcb, 2, frame->srr0_ip);
    tcb_set_mr (tcb, 3, self->vm->msr);
    tcb_set_mr (tcb, 4, param);

    acceptor.raw = 0;
    acceptor.x.ctrlxfer = 1;
    tcb_set_br (tcb, 0, acceptor.raw);

    threadid_t partner = tcb_get_pager (tcb);
    tag = tcb_do_ipc (tcb, partner, partner, timeout_never());

    if (msg_tag_is_error (&tag))
	enter_kdebug("hvm fault send failed");

    return !msg_tag_is_error (&tag);
}

NOINLINE bool
arch_ktcb_send_hvm_pagefault (arch_ktcb_t *self, enum softhvm_exit_reason_e exc, except_regs_t *frame,
                                word_t addr, word_t instr,
				                word_t tlb0, word_t tlb1, word_t tlb2, u8_t pid, u8_t idx,
				                bool read, bool write, bool execute)
{
    const int untyped = 8;
    tcb_t *tcb = addr_to_tcb(self);
    acceptor_t acceptor;

    acceptor.raw = 0;

    msg_tag_t tag = softhvm_pagefault_tag(exc, untyped, read, write, execute);
    tag.x.typed = tcb_append_ctrlxfer_item (tcb, tag, untyped + 1);
    tcb_set_tag (tcb, tag);
    tcb_set_mr (tcb, 1, addr);
    tcb_set_mr (tcb, 2, frame->srr0_ip);
    tcb_set_mr (tcb, 3, instr);
    tcb_set_mr (tcb, 4, self->vm->msr);
    tcb_set_mr (tcb, 5, tlb0);
    tcb_set_mr (tcb, 6, tlb1);
    tcb_set_mr (tcb, 7, tlb2);
    tcb_set_mr (tcb, 8, (word_t) pid << 8 | idx);

    acceptor_set_rcv_window (&acceptor, fpage_complete_mem());
    acceptor.x.ctrlxfer = 1;
    tcb_set_br (tcb, 0, acceptor.raw);

    threadid_t partner = tcb_get_pager (tcb);
    tag = tcb_do_ipc (tcb, partner, partner, timeout_never());

    if (msg_tag_is_error (&tag))
	enter_kdebug("hvm pagefault send failed");

    return !msg_tag_is_error (&tag);
}

static void check_tlb( ppc_softhvm_t *vm )
{
#if 0
    if (vm->htlb_dirty)
	enter_kdebug("TLB still dirty?");

    for (word_t idx = 0; idx < PPC_MAX_TLB_ENTRIES; idx++)
    {
	ppc_tlb0_t tlb0;
	ppc_tlb0_read (&tlb0, idx);
	if (ppc_tlb0_is_valid (&tlb0) && tlb0.trans_space == 1)
	{
	    bool found = false;
	    for (int entry = 0; entry < PPC_MAX_TLB_ENTRIES; entry++)
		if (ppc_hvm_tlb_vaddr_in_entry (&vm->tlb[entry],
						ppc_tlb0_get_vaddr (&tlb0), vm->pid))
		    found = true;
	    if (ppc_hvm_tlb_vaddr_in_entry (&vm->shadow_tlb,
					    ppc_tlb0_get_vaddr (&tlb0), vm->pid))
		found = true;
	    if (!found)
	    {
		printf("invalid host-guest mapping found in TLB: %d\n", idx);
		enter_kdebug("invalid host mapping");
	    }
	}
    }
#endif
}

static void update_tlb_hvm( ppc_softhvm_t * vm )
{
    if (EXPECT_FALSE(vm->htlb_dirty))
    {
	space_t *space = get_current_space();
	space_flush_tlb_hvm (space, space, vm->tlb_dirty_start, vm->tlb_dirty_end);
	vm->htlb_dirty = false;
    }
}

NOINLINE void space_flush_tlb_hvm (space_t *self,  space_t *curspace, word_t start, word_t end )
{
    //TRACEF("flush %x - %x\n", start, end);
    for (word_t idx = 0; idx < swtlb.high_water; idx++)
    {
	ppc_tlb0_t tlb0;
	ppc_tlb0_read (&tlb0, idx);

	if (!ppc_tlb0_is_valid (&tlb0))
	    continue;

	if ( (tlb0.trans_space == 1) &&
	     !((ppc_tlb0_get_vaddr (&tlb0) >= end) ||
	       (ppc_tlb0_get_vaddr (&tlb0) + ppc_tlb0_get_size (&tlb0) - 1) <= start) )
	{
	    {
		ppc_tlb0_t inv = ppc_tlb0_invalid ();
		ppc_tlb0_write (&inv, idx);
	    }
	    ppc_swtlb_set_free (&swtlb, idx);
	}
    }
    isync();
}

NOINLINE bool 
space_handle_hvm_tlb_miss (space_t *self, ppc_softhvm_t *vm, ppc_hvm_tlb_t *tlbentry, word_t gvaddr, paddr_t *gpaddr)
{
    TRACE_EMUL("Inserting GV-GP TLB entry from shadow TLB: %08x %08x %08x pid=%x\n", 
	       tlbentry->tlb0.raw, tlbentry->tlb1.raw, tlbentry->tlb2.raw, tlbentry->pid);
    
    ppc_tlb1_t tlb1 = tlbentry->phys_tlb1;   /* was a copy constructor */

    size_t gsize = ppc_tlb0_get_log2size (&tlbentry->tlb0);
    /* gpaddr was a paddr_t& out-parameter; the three uses below were left as
       the reference had them.  The TRACE_EMUL one still had its static_casts,
       which compiled only because TRACE_EMUL expands to nothing and its
       arguments are therefore never parsed.  Notes §140. */
    *gpaddr = ppc_tlb1_get_paddr (&tlbentry->tlb1) | (gvaddr & (ppc_tlb0_get_size (&tlbentry->tlb0) - 1));

    if (*gpaddr >= USER_AREA_END)
	return false;

    TRACE_EMUL("GVA:%lx GPA:%lx.%lx\n", gvaddr, (word_t)(*gpaddr >> 32), (word_t)(*gpaddr));

    pgent_t *pg;
    word_t pgsize;
    if (!space_lookup_mapping (self, (addr_t) *gpaddr, &pg, &pgsize, 0))
	return false;
    
    size_t hsize = page_shift (pgsize);
    paddr_t hpaddr = pgent_address (pg, self, pgsize) | (*gpaddr & ((1ull << hsize) - 1));
	
    /* we have a valid entry in the TLB and in the ptab --> we
     * can create a TLB entry */
    TRACE_EMUL("mapping found (%p): %p -> %x.%08x (%x)\n",
	       pg, *gpaddr, (word_t)(hpaddr >> 32), (word_t)hpaddr, pgsize);
    TRACE_EMUL("[%c%c%c], cache=%x, erpn=%x\n", 
	       pg->map.read ? 'R' : ' ', pg->map.write ? 'W' : ' ',
	       pg->map.execute ? 'X' : ' ', pg->map.caching, pg->map.erpn);
    
    size_t size = min (gsize, hsize);
    while (!ppc_tlb0_is_valid_pagesize (size))
        size--;

    ppc_tlb0_t tlb0;

    /* was the ppc_tlb0_t(vaddr, log2size) constructor, whose valid/space
       arguments defaulted to true/0. */
    ppc_tlb0_init_vaddr_size (&tlb0, gvaddr & ~((1ul << size) - 1), size, true, 0);
    tlb0.trans_space = 1;

    ppc_tlb1_init_paddr (&tlb1, hpaddr & ~((1ull << size) - 1));

    ppc_tlb2_t tlb2;
    tlb2.raw = tlbentry->tlb2.raw;

    if (ppc_tlb2_is_accessible (&tlb2)) // don't bother if no access rights are set...
    {
	/* we support three protection modes right now:
	 *   user and kernel have access rights: pid0
	 *   only user: pid1
	 *   only kernel: pid2
	 */

	word_t pid = 0; // kernel+user accessible

	if (!ppc_tlb2_is_user_accessible (&tlb2))
	    pid = 2;
	else if (!ppc_tlb2_is_kernel_accessible (&tlb2))
	    pid = 1;

	// move kernel access permissions into user part
	tlb2.raw |= (tlb2.raw & 0x7) << 3;

	// fix up cache attributes
#warning fix cache attribute setting
	tlb2.raw &= 0x3f;
	tlb2.mem_coherency = 1;
	tlb2.wt_l1 = 1;
	tlb2.user2 = 1;

	word_t hwtlb_index = ppc_swtlb_allocate (&swtlb);

	TRACE_EMUL("inserting TLB entry %d: %08x, %08x, %08x, pid=%d (%x)\n",
		   hwtlb_index, tlb0.raw, tlb1.raw, tlb2.raw, pid, 
		   (&self->vm->shadow_tlb == tlbentry) ? 255 : (tlbentry - &self->vm->tlb[0]) / sizeof(ppc_hvm_tlb_t));

	ppc_mmucr_write_search_id(pid, 0);
	ppc_tlb0_write (&tlb0, hwtlb_index);
	ppc_tlb1_write (&tlb1, hwtlb_index);
	ppc_tlb2_write (&tlb2, hwtlb_index);
	isync();

	/* flush icache to avoid alias problems after PID change */
	if (tlb2.user_execute)
	    asm volatile ("iccci 0,0" : : : "memory");

	/* mark entry dirty in VTLB */
	ppc_hvm_tlb_touch (tlbentry, hwtlb_index);
    }
    return true;
}

EXCDEF( hvm_dtlb_miss_handler )
{
    tcb_t *tcb = get_current_tcb();
    ppc_softhvm_t *vm = (&tcb->arch)->vm;
    word_t dear = ppc_get_spr(SPR_DEAR);
    ppc_esr_t esr; ppc_esr_read (&esr);
    paddr_t gpaddr;

    TRACEPOINT(PPC_HVM_DTLB_MISS,
	       "HVM DTLB MISS: IP: %08x, DEAR: %lx",
	       srr0, dear);

    if (ppc_is_kernel_mode(srr1))
    {
        if (!space_handle_tlb_miss (get_kernel_space(), (addr_t)dear, (addr_t)dear, false, true))
            panic("kernel accessed unmapped device @ %08x, IP %08x\n", dear, srr0);
        return_except();
    }
    
    ppc_hvm_tlb_t *tlbentry = NULL;
    int tlbidx = -1;

    if (ppc_softhvm_in_shadow_tlb (vm, dear))
	tlbentry = &vm->shadow_tlb;
    else if ( (tlbidx = ppc_softhvm_find_tlb_entry (vm, dear)) != -1 )
	tlbentry = &vm->tlb[tlbidx];

    if (!tlbentry)
    {
	vm->dear = dear;
	vm->esr = esr.raw;      // XXX: Interpret this correctly!
	ppc_softhvm_raise_exception (vm, exc_data_tlb, frame);
    }
    else
    {
	if (space_handle_hvm_tlb_miss (tcb_get_space (tcb), vm, tlbentry, dear, &gpaddr))
	{
	    if (tlbidx != -1)
	    {
		ppc_softhvm_replace_shadow_tlb (vm, tlbentry, tlbidx);
		update_tlb_hvm(vm);
	    }
	}
	else
	{
	    word_t instr;
	    read_hvm_instruction(frame->srr0_ip, &instr, false);

	    arch_ktcb_send_hvm_pagefault (&tcb->arch, 
		er_tlb, frame, dear, instr,
		tlbentry->tlb0.raw, tlbentry->tlb1.raw, tlbentry->tlb2.raw,
		tlbentry->pid, tlbidx, esr.x.store == 0, esr.x.store != 0, false);
	}
    }

    ppc_softhvm_handle_pending_events (vm, frame);
    check_tlb(vm);
    return_except();
}

EXCDEF( hvm_itlb_miss_handler )
{
    tcb_t *tcb = get_current_tcb();
    ppc_softhvm_t *vm = (&tcb->arch)->vm;
    paddr_t gpaddr;

    TRACEPOINT(PPC_HVM_ITLB_MISS,
	       "HVM ITLB MISS: IP: %08x, LR: %08x",
	       srr0, frame->lr);

    ASSERT(!ppc_is_kernel_mode(srr1));

    ppc_hvm_tlb_t *tlbentry = NULL;
    int tlbidx = -1;

    if (ppc_softhvm_in_shadow_tlb (vm, srr0))
	tlbentry = &vm->shadow_tlb;
    else if ( (tlbidx = ppc_softhvm_find_tlb_entry (vm, srr0)) != -1 )
	tlbentry = &vm->tlb[tlbidx];

    if (!tlbentry)
    {
	ppc_softhvm_raise_exception (vm, exc_instr_tlb, frame);
    }
    else 
    {
	if (space_handle_hvm_tlb_miss (tcb_get_space (tcb), vm, tlbentry, srr0, &gpaddr))
	{
	    if (tlbidx != -1)
	    {
		ppc_softhvm_replace_shadow_tlb (vm, tlbentry, tlbidx);
		update_tlb_hvm(vm);
	    }
	}
	else
	{
	    arch_ktcb_send_hvm_pagefault (&tcb->arch, 
		er_tlb, frame, srr0, 0,
		tlbentry->tlb0.raw, tlbentry->tlb1.raw, tlbentry->tlb2.raw,
		tlbentry->pid, tlbidx, false, false, true);
	}
    }

    ppc_softhvm_handle_pending_events (vm, frame);
    check_tlb(vm);
    return_except();
}

EXCDEF( hvm_dsi_handler )
{
    /* just reflect the exception back to the guest, the VTLB should
     * take care of this */

    ppc_esr_t esr; ppc_esr_read (&esr);
    word_t dear = ppc_get_spr(SPR_DEAR);

    TRACEPOINT(PPC_HVM_EXCEPT_DSI,
	       "HVM DSI EXCEPT: IP: %08x, LR: %08x, DEAR: %08x, ESR: %08x",
	       srr0, frame->lr, dear, esr.raw);

    ASSERT(!ppc_is_kernel_mode(frame->srr1_flags));

    ppc_softhvm_t *vm = (&get_current_tcb()->arch)->vm;

    vm->esr = esr.raw;
    vm->dear = dear;
    ppc_softhvm_raise_exception (vm, exc_data_storage, frame);
    check_tlb(vm);
 
    return_except();
}

EXCDEF( hvm_isi_handler )
{
    /* just reflect the exception back to the guest, the VTLB should
     * take care of this */

    ppc_esr_t esr; ppc_esr_read (&esr);

    TRACEPOINT(PPC_HVM_EXCEPT_ISI,
	       "HVM ISI EXCEPT: IP: %08x, LR: %08x, ESR: %08x",
	       srr0, frame->lr, esr.raw);

    ASSERT(!ppc_is_kernel_mode(frame->srr1_flags));
    
    ppc_softhvm_t *vm = (&get_current_tcb()->arch)->vm;

    vm->esr = esr.raw;
    ppc_softhvm_raise_exception (vm, exc_instr_storage, frame);
    check_tlb(vm);
 
    return_except();
}

EXCDEF( hvm_alignment_handler )
{
    ppc_esr_t esr; ppc_esr_read (&esr);
    word_t dear = ppc_get_spr(SPR_DEAR);

    TRACEPOINT(PPC_HVM_ALIGN,
	       "HVM ALIGNMENT: IP: %08x, LR: %08x, DEAR: %08x, ESR: %08x",
	       srr0, frame->lr, dear, esr.raw);
    
    ASSERT(!ppc_is_kernel_mode(frame->srr1_flags));

    ppc_softhvm_t *vm = (&get_current_tcb()->arch)->vm;

    vm->esr = esr.raw;
    vm->dear = dear;
    ppc_softhvm_raise_exception (vm, exc_alignment, frame);
    check_tlb(vm);
 
    return_except();
}

EXCDEF( hvm_program_handler )
{
    if( ppc_is_kernel_mode(frame->srr1_flags) )
    {
	if( get_kip()->kdebug_entry )
	{
	    // If the debugger exists, let it have the first try at handling
	    // the exception.
	    word_t start_ip = frame->srr0_ip;
	    word_t start_flags = frame->srr1_flags;
	    
	    try_to_debug( frame, EXCEPT_ID(PROGRAM), 0, 0 );
	    
	    if( (frame->srr0_ip != start_ip) || (frame->srr1_flags != start_flags) )
		return_except();	// The kernel debugger handled the exception.
	}
	else
	    panic( "program exception in kernel thread.\n" );
    }
    else
    {
	ppc_esr_t esr; ppc_esr_read (&esr);
	ppc_softhvm_t *vm = (&get_current_tcb()->arch)->vm;

	TRACEPOINT(PPC_HVM_EXCEPT_PROG, 
		   "PROG EXC: IP %p, MSR %08x, ESR %08x", 
		   frame->srr0_ip, frame->srr1_flags, esr.raw);

	ppc_softhvm_update_timers (vm, ppc_get_timebase());

	if (esr.x.privileged_instr)
	{
	    word_t instr;
	    word_t oldip = frame->srr0_ip;

	    read_hvm_instruction(frame->srr0_ip, &instr, false);

	    TRACE_EMUL("Faulting instruction: %08x @ %08x\n", instr, frame->srr0_ip);

	    if (ppc_softhvm_emulate_instruction (vm, instr, frame))
	    {
#if 0
		/* we put an upper bound on emulation to avoid malicious
		 * tasks to run a DOS with interrupts disabled */
		for (unsigned num = 0; num < MAX_INSTR_EMULATE; num++)
		{
		    /* only emulate privileged code */
		    if (ppc_softhvm_is_user (vm))
			break;

		    /* speculative fetch if TLB is dirty or next instruction
		     * is in different 1k page (i.e., minimal page size) */
		    bool speculative = vm->htlb_dirty | ((frame->srr0_ip & ~0x3ff) != (oldip & ~0x3ff));
		    oldip = frame->srr0_ip;
		    update_tlb_hvm(vm);
		    
		    if (!read_hvm_instruction(frame->srr0_ip, instr, speculative) || 
			!ppc_softhvm_emulate_instruction (vm, instr, frame))
			break;

		    TRACEPOINT(PPC_HVM_EMUL_INTERPRET, "instr emulation opcode %08x, IP %08x", 
			       instr, frame->srr0_ip);
		}
#endif
		update_tlb_hvm(vm);
	    }
	    else
	    {
		//TRACEF("send exception IPC\n");
		arch_ktcb_send_hvm_fault (&get_current_tcb()->arch,
					  er_program, frame, instr, 0, false);
	    }
	}
	else
	{
	    ppc_softhvm_raise_exception (vm, exc_program, frame);
	    vm->esr = esr.raw;

	    if (esr.x.floating_point)
	    {
		enter_kdebug("fpu emulation");
	    } 
	    else if (!esr.x.trap)
	    {
		/* reflect exception? */
		TRACEF("program exception @ %p, ESR: %08x\n", frame->srr0_ip, esr.raw);
		enter_kdebug("prog except");
	    }
	}
	ppc_softhvm_handle_pending_events (vm, frame);
	check_tlb(vm);
    }

    return_except();
}

EXCDEF( hvm_fp_unavail_handler )
{
    tcb_t *current_tcb = get_current_tcb();

    /* FPU can be unavailable for 2 reasons: guest disabled it or it
     * is not restored */
    ppc_softhvm_t *vm = (&current_tcb->arch)->vm;
    if (!(vm->msr & (1 << MSR_FP)))
    {
	ppc_softhvm_raise_exception (vm, exc_fpu_unavail, frame);
    }
    else
    {
	/* FPU is enabled in guest but not in host--enable it */
	tcb_resources_fpu_unavail_exception (&current_tcb->resources, current_tcb);
    }
    return_except();
}

EXCDEF( hvm_syscall_handler )
{
    ppc_softhvm_raise_exception ((&get_current_tcb()->arch)->vm,
				 exc_system_call, frame);
    return_except();
}

EXCDEF( hvm_debug_handler )
{
    ppc_softhvm_raise_exception ((&get_current_tcb()->arch)->vm,
				 exc_debug, frame);
    return_except();
}

EXCDEF( hvm_decrementer_handler )
{
    /* Don't go back to sleep if the thread was in power savings mode.
     * We will only see timer interrupts from user mode, or from the sleep
     * function in kernel mode.
     */
    if( EXPECT_FALSE(ppc_is_kernel_mode(srr1)) ) {
	srr1 = processor_wake( srr1 );
	frame->srr1_flags = srr1;
    }

    TRACEPOINT(PPC_HVM_EXCEPT_DECR, 
	       "Decrementer Intr: IP=%p, MSR %08x", srr0, srr1);

    // BookE uses auto-reload decrementer; just ack
    {
	ppc_tsr_t tsr = ppc_tsr_dec_irq ();
	ppc_tsr_write (&tsr);
    }
    sched_handle_timer_interrupt ();

    // tick the VM and fire necessary interrupts
    ppc_softhvm_t *vm = (&get_current_tcb()->arch)->vm;
    ppc_softhvm_update_timers (vm, ppc_get_timebase());
    ppc_softhvm_handle_pending_events (vm, frame);

    return_except();
}


FEATURESTRING("powerpc-hvm");

DECLARE_KMEM_GROUP(kmem_hvm);
void arch_ktcb_init_hvm (arch_ktcb_t *self, tcb_t *tcb)
{
    const int allocsize = ((sizeof(ppc_softhvm_t) / KMEM_CHUNKSIZE) + 1) * KMEM_CHUNKSIZE;
    self->vm = (ppc_softhvm_t*)kmem_alloc(&kmem, kmem_hvm, allocsize);
    resource_bits_add (&tcb->resource_bits, SOFTHVM);
    ppc_softhvm_init (self->vm);
}
