/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     src/arch/powerpc/softhvm.cc
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
#include <kdb/tracepoints.h>
#include INC_ARCH(softhvm.h)

#define TRACE_EMUL(args...)

DECLARE_TRACEPOINT(PPC_HVM_EMUL_MFMSR);
DECLARE_TRACEPOINT(PPC_HVM_EMUL_MTMSR);
DECLARE_TRACEPOINT(PPC_HVM_EMUL_MFSPR);
DECLARE_TRACEPOINT(PPC_HVM_EMUL_MTSPR);
DECLARE_TRACEPOINT(PPC_HVM_EMUL_TLBWE);
DECLARE_TRACEPOINT(PPC_HVM_EMUL_TLBRE);
DECLARE_TRACEPOINT(PPC_HVM_EMUL_TLBSX);
DECLARE_TRACEPOINT(PPC_HVM_EMUL_WRTEE);
DECLARE_TRACEPOINT(PPC_HVM_EMUL_WRTEEI);
DECLARE_TRACEPOINT(PPC_HVM_EMUL_RFI);
DECLARE_TRACEPOINT(PPC_HVM_EMUL_TWI);

void ppc_softhvm_init (ppc_softhvm_t *self)
{
    // clear the VTLB shadows
    for (int i = 0; i < PPC_MAX_TLB_ENTRIES; i++)
	self->tlb[i].phys_tlb1.raw = ~0U;

    // disable all timers
    self->dec_base = ~0ULL;
    self->watchdog_base = ~0ULL;
    self->fixed_interval_base = ~0ULL;
}

NOINLINE void ppc_softhvm_raise_exception (ppc_softhvm_t *self, int num, except_regs_t *regs)
{
    TRACE_EMUL("raise exception %d\n", num);
    ASSERT(num < 16);

    self->srr0 = regs->srr0_ip;
    self->srr1 = self->msr;
    ppc_softhvm_set_msr (self, self->msr & ((1 << MSR_CE) | (1 << MSR_ME) | (1 << MSR_DE)));
    regs->srr0_ip = ppc_softhvm_get_ivor_ip (self, num);
    
}

NOINLINE void ppc_softhvm_raise_crit_interrupt (ppc_softhvm_t *self, int num, except_regs_t *regs)
{
    TRACE_EMUL("raise critical interrupt %d\n", num);
    ASSERT(num < 16);
    self->csrr0 = regs->srr0_ip;
    self->csrr1 = self->msr;
    ppc_softhvm_set_msr (self, self->msr & (1 << MSR_ME));
    regs->srr0_ip = ppc_softhvm_get_ivor_ip (self, num);
}

NOINLINE void ppc_softhvm_raise_mcheck_interrupt (ppc_softhvm_t *self, int num, except_regs_t *regs)
{
    TRACE_EMUL("raise mcheck interrupt %d\n", num);
    ASSERT(num < 16);
    self->mcsrr0 = regs->srr0_ip;
    self->mcsrr1 = self->msr;
    ppc_softhvm_set_msr (self, 0);
    regs->srr0_ip = ppc_softhvm_get_ivor_ip (self, num);
}

NOINLINE void ppc_softhvm_set_msr (ppc_softhvm_t *self, word_t val)
{ 
    word_t oldmsr = self->msr;
    self->msr = val;
    
    if (self->msr & (1 << MSR_IS | 1 << MSR_DS))
	UNIMPLEMENTED();

    if ((oldmsr & (1 << MSR_PR)) != (self->msr & (1 << MSR_PR)))
        ppc_set_pid(ppc_softhvm_get_pid_for_msr (self));     
}

bool ppc_softhvm_mfmsr (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs)
{
    TRACEPOINT(PPC_HVM_EMUL_MFMSR, "mfmsr r%d, %08x", ppc_instr_rt (instr), self->msr);

    if (ppc_softhvm_is_user (self))
	ppc_softhvm_raise_exception (self, exc_program, regs);
    else
    {
	except_regs_set_register (regs, ppc_instr_rt (instr), self->msr);
	regs->srr0_ip += sizeof(instr);
    }
    return true;
}

bool ppc_softhvm_mtmsr (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs)
{
    TRACEPOINT(PPC_HVM_EMUL_MTMSR, "mtmsr %08x -> r%d", self->msr, ppc_instr_rt (instr));

    if (ppc_softhvm_is_user (self))
	ppc_softhvm_raise_exception (self, exc_program, regs);
    else
    {
	self->msr = except_regs_get_register (regs, ppc_instr_rt (instr));
	regs->srr0_ip += sizeof(instr);
    }
    return true;
}

word_t * ppc_softhvm_get_spr (ppc_softhvm_t *self, int spr, bool read)
{
    switch(spr) {
    case 0x16: return &self->dec;
    case 0x1a: return &self->srr0;
    case 0x1b: return &self->srr1;
    case 0x30: return &self->pid;
    case 0x36: return read ? NULL : &self->decar;
    case 0x3a: return &self->csrr0;
    case 0x3b: return &self->csrr1;
    case 0x3d: return &self->dear;
    case 0x3e: return &self->esr;
    case 0x3f: return &self->ivpr;
    case 0x110 ... 0x117: return &self->sprg[spr - 0x110];
    case 0x11c: return read ? NULL : &self->tbl;
    case 0x11d: return read ? NULL : &self->tbu;
    case 0x11e: return read ? &self->pir : NULL;
    case 0x11f: return read ? &self->pvr : NULL;
    case 0x130: return read ? &self->dbsr : NULL;
    case 0x134 ... 0x136: return &self->dbcr[spr - 0x134];
    case 0x138 ... 0x13b: return &self->iac[spr - 0x138];
    case 0x13c ... 0x13d: return &self->dac[spr - 0x13c];
    case 0x13e ... 0x13f: return &self->dvc[spr - 0x13e];
    case 0x150: return &self->tsr.raw;
    case 0x154: return &self->tcr.raw;
    case 0x190 ... 0x19f: return &self->ivor[spr - 0x190];
    case 0x23a: return &self->mcsrr0;
    case 0x23b: return &self->mcsrr1;
    case 0x23c: return &self->mcsr;
    case 0x370 ... 0x373: return &self->inv[spr - 0x370];
    case 0x374 ... 0x377: return &self->itv[spr - 0x374];
    case 0x378: return &self->ccr1;
    case 0x390 ... 0x393: return &self->dnv[spr - 0x390];
    case 0x394 ... 0x397: return &self->dtv[spr - 0x394];
    case 0x398: return &self->dvlim;
    case 0x399: return &self->ivlim;
    case 0x39b: return read ? &self->rstcfg : NULL;
    case 0x39c ... 0x39d: return read ? &self->dcdbt[spr - 0x39c] : NULL;
    case 0x39e ... 0x39f: return read ? &self->icdbt[spr - 0x39c] : NULL;
    case 0x3b2: return &self->mmucr.raw;
    case 0x3b3: return &self->ccr0;
    case 0x3d3: return &self->icdbdr;
    case 0x3f3: return &self->dbdr;
    default: return NULL;
    }
}

bool ppc_softhvm_mfspr (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs)
{
    unsigned spridx = ppc_instr_rf (instr);

    TRACEPOINT(PPC_HVM_EMUL_MFSPR, "mfspr %u/%x -> reg:%u (IP %08x)", 
	       spridx, spridx, ppc_instr_rt (instr), regs->srr0_ip);

    /* we assume that accesses to user SPRs don't come here */
    if (ppc_softhvm_is_user (self))
	ppc_softhvm_raise_exception (self, exc_program, regs);
    else
    {
	word_t *spr = ppc_softhvm_get_spr (self, spridx, true);
	if (!spr)
	    return false;

	except_regs_set_register (regs, ppc_instr_rt (instr), *spr);
	regs->srr0_ip += sizeof(instr);
    }
    return true;
}

bool ppc_softhvm_mtspr (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs)
{
    unsigned spridx = ppc_instr_rf (instr);

    TRACEPOINT(PPC_HVM_EMUL_MTSPR, "mtspr %u/%x <- reg:%u (IP %08x)", 
	       spridx, spridx, ppc_instr_rt (instr), regs->srr0_ip);
    
    /* we assume that accesses to user SPRs don't come here */
    if (ppc_softhvm_is_user (self))
	ppc_softhvm_raise_exception (self, exc_program, regs);
    else
    {
	word_t *spr = ppc_softhvm_get_spr (self, spridx, false);
	if (!spr)
	    return false;

	word_t val = except_regs_get_register (regs, ppc_instr_rt (instr));

	// handle write-clear special cases
	switch (spridx)
	{
	case 0x16:
	    self->dec_base = ppc_get_timebase();
	    self->decar = val;
	    break;
	case 0x130:
	case 0x150:
	case 0x23c: *spr = *spr & ~val; break;
	case 0x30: 
	    TRACE_EMUL("setting PID to %x\n", val);
	    ppc_softhvm_clear_tlb_dirty (self);
	    self->htlb_dirty = true; // self->pid change--flush host TLB
	    self->tlb_dirty_start = 0;
	    self->tlb_dirty_end = ~0U;
	    /* fall through */
	default: *spr = val;
	}

	regs->srr0_ip += sizeof(instr);
	ppc_softhvm_load_guest_sprs (self);
    }
    return true;
}

bool ppc_softhvm_tlbre (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs)
{
    if (ppc_softhvm_is_user (self))
	ppc_softhvm_raise_exception (self, exc_program, regs);
    else
    {
	word_t val = 0;
	int idx = except_regs_get_register (regs, ppc_instr_ra (instr));

	if (idx < PPC_MAX_TLB_ENTRIES)
	    switch(ppc_instr_rb (instr))
	    {
	    case 0:
		val = self->tlb[idx].tlb0.raw;
		self->mmucr.search_id = self->tlb[idx].pid;
		self->mmucr.search_translation_space = 0; // XXX
		break;
	    case 1: val = self->tlb[idx].tlb1.raw; break;
	    case 2: val = self->tlb[idx].tlb2.raw; break;
	    }
	except_regs_set_register (regs, ppc_instr_rt (instr), val);
	regs->srr0_ip += sizeof(instr);

	TRACEPOINT(PPC_HVM_EMUL_TLBRE, "tlbre [%02d:%d] val=%08x", idx, ppc_instr_rb (instr), val); 
    }
    return true;
}

bool ppc_softhvm_tlbsx (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs)
{
    if (ppc_softhvm_is_user (self))
	ppc_softhvm_raise_exception (self, exc_program, regs);
    else
    {
	if (instr.xform.rc)
	{
	    regs->cr &= ~(0xf << 28);
	    if (regs->xer & 80000000) regs->cr |= (1 << 28); // XER OV
	}
	word_t eaddr = ppc_instr_ra (instr) == 0 ? 0 : except_regs_get_register (regs, ppc_instr_ra (instr));
	eaddr += except_regs_get_register (regs, ppc_instr_rb (instr));
	int idx = ppc_softhvm_find_tlb_entry (self, eaddr);
	if (idx >= 0) 
	{
	    except_regs_set_register (regs, ppc_instr_rt (instr), idx);
	    regs->cr |= (2 << 28);
	}
	regs->srr0_ip += sizeof(instr);
	TRACEPOINT(PPC_HVM_EMUL_TLBSX, "tlbsx %x -> %s, idx: %d", 
		   eaddr, idx >= 0 ? "found" : "not found", idx);
    }
    return true;
}

bool ppc_softhvm_tlbsync (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs)
{
    if (ppc_softhvm_is_user (self))
	ppc_softhvm_raise_exception (self, exc_program, regs);
    else
    {
	/* no-op on 440 */
	regs->srr0_ip += sizeof(instr);
    }
    return true;
}

bool ppc_softhvm_tlbwe (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs)
{
    if (ppc_softhvm_is_user (self))
	ppc_softhvm_raise_exception (self, exc_program, regs);
    else
    {
	int idx = except_regs_get_register (regs, ppc_instr_ra (instr));
	if (idx < PPC_MAX_TLB_ENTRIES)
	{
	    ppc_softhvm_update_tlb_dirty (self, &self->tlb[idx]);
	    word_t val = except_regs_get_register (regs, ppc_instr_rt (instr));
	    switch(ppc_instr_rb (instr))
	    {
	    case 0:
		self->tlb[idx].tlb0.raw = val;
		self->tlb[idx].pid = self->mmucr.search_id;
		break;
	    case 1: self->tlb[idx].tlb1.raw = val; break;
	    case 2: self->tlb[idx].tlb2.raw = val; break;
	    }
	    TRACEPOINT(PPC_HVM_EMUL_TLBWE, "tlbwe entry=[%02d:%d], val=%08x\n", idx, ppc_instr_rb (instr), val);
	}
	regs->srr0_ip += sizeof(instr);
    }
    return true;
}

bool ppc_softhvm_set_esr (ppc_softhvm_t *self, word_t val, except_regs_t *regs)
{
    TRACE_EMUL("set_esr: %x (%d)\n", val & (1 << MSR_EE) ? 1 : 0);
    if (ppc_softhvm_is_user (self))
	ppc_softhvm_raise_exception (self, exc_program, regs);
    else
    {
	const word_t mask = (1U << MSR_EE);
	ppc_softhvm_set_msr (self, (self->msr & ~mask) | (val & mask));
	regs->srr0_ip += sizeof(ppc_instr_t);
    }
    return true;
}

bool ppc_softhvm_wrtee (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs)
{
    TRACEPOINT(PPC_HVM_EMUL_WRTEE, "wrtee %08x", except_regs_get_register (regs, ppc_instr_rt (instr)));
    return ppc_softhvm_set_esr (self, except_regs_get_register (regs, instr.xfxform.rt), regs);
}

bool ppc_softhvm_wrteei (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs)
{
    TRACEPOINT(PPC_HVM_EMUL_WRTEEI, "wrteei");
    return ppc_softhvm_set_esr (self, instr.raw, regs);
}

bool ppc_softhvm_rfi (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs, word_t srr0, word_t srr1)
{
    TRACEPOINT(PPC_HVM_EMUL_RFI, "rfi -> srr0=%08x, srr1=%08x", srr0, srr1);
    if (ppc_softhvm_is_user (self))
	ppc_softhvm_raise_exception (self, exc_program, regs);
    else
    {
	ppc_softhvm_invalidate_shadow_tlb (self);
	regs->srr0_ip = srr0;
	ppc_softhvm_set_msr (self, srr1);
    }
    return true;
}

bool ppc_softhvm_twi (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs)
{
    TRACEPOINT(PPC_HVM_EMUL_TWI, "twi");

    ppc_esr_t newesr;
    newesr.raw = 0;
    newesr.x.trap = 1;

    ppc_softhvm_raise_exception (self, exc_program, regs);
    self->esr = newesr.raw;

    return true;
}

bool ppc_softhvm_emulate_instruction (ppc_softhvm_t *self, word_t opcode, except_regs_t *regs)
{
    ppc_instr_t instr = PPC_INSTR(opcode);

    TRACE_EMUL("emulate instruction (%08x): prim: %d, sec: %d, sz: %d\n",
	       instr.raw, ppc_instr_get_primary (instr), ppc_instr_get_secondary (instr), sizeof(instr));

    switch(ppc_instr_get_primary (instr))
    {
    case 31:
	switch(ppc_instr_get_secondary (instr)) {
	case  83: return ppc_softhvm_mfmsr (self, instr, regs);
	case 339: return ppc_softhvm_mfspr (self, instr, regs);
	case 146: return ppc_softhvm_mtmsr (self, instr, regs);
	case 467: return ppc_softhvm_mtspr (self, instr, regs);
	case 946: return ppc_softhvm_tlbre (self, instr, regs);
	case 914: return ppc_softhvm_tlbsx (self, instr, regs);
	case 566: return ppc_softhvm_tlbsync (self, instr, regs);
	case 978: return ppc_softhvm_tlbwe (self, instr, regs);
	case 131: return ppc_softhvm_wrtee (self, instr, regs);
	case 163: return ppc_softhvm_wrteei (self, instr, regs);
	case 966: asm volatile ("iccci 0,0" : : : "memory"); return false;
	};
	break;

    case 19:
	switch(ppc_instr_get_secondary (instr)) {
	case 38: return ppc_softhvm_rfi (self, instr, regs, self->mcsrr0, self->mcsrr1);
	case 50: return ppc_softhvm_rfi (self, instr, regs, self->srr0, self->srr1);
	case 51: return ppc_softhvm_rfi (self, instr, regs, self->csrr0, self->csrr1);
	};
	break;

    case 3: return ppc_softhvm_twi (self, instr, regs);
    }
    return false;
}

void ppc_softhvm_update_timers (ppc_softhvm_t *self, u64_t time)
{
    if (self->dec_base <= time - self->decar)
    {
	//enter_kdebug("decrementer underflow");
	self->tsr.decrementer_irq_status = 1;
	if (self->tcr.auto_reload)
	    self->dec_base += self->decar;
	else
	    self->dec_base = ~0ULL;	// disable self->dec
    }
    self->dec = (self->dec_base <= time - self->decar) ? time - self->dec_base : 0;

    if (self->watchdog_base <= time - ppc_tcr_get_watchdog_period (&self->tcr))
    {
	enter_kdebug("watchdog timer");
	self->watchdog_base += ppc_tcr_get_watchdog_period (&self->tcr);
	self->tsr.watchdog_irq_status = 1;
    }

    if (self->fixed_interval_base <= time - ppc_tcr_get_fixed_interval_period (&self->tcr))
    {
	enter_kdebug("fixed interval timer");
	self->fixed_interval_base += ppc_tcr_get_fixed_interval_period (&self->tcr);
	self->tsr.fixed_interval_irq_status = 1;
    }
}

void ppc_softhvm_inject_pending_events (ppc_softhvm_t *self, except_regs_t *regs)
{
    if ( (self->event_inject & (1 << evt_machine_check)) &&
	 (self->msr & (1 << MSR_ME)) )
    {
	ppc_softhvm_raise_mcheck_interrupt (self, exc_critical_input, regs);
    } 
    else if ( (self->event_inject & (1 << evt_debug)) &&
	      (self->msr & (1 << MSR_DE)) )
    {
	ppc_softhvm_raise_crit_interrupt (self, exc_critical_input, regs);
    }
    else if ( (self->event_inject & (1 << evt_critical_input)) &&
	      (self->msr & (1 << MSR_CE)) )
    {
	ppc_softhvm_raise_crit_interrupt (self, exc_critical_input, regs);
    }
    else if ( self->tcr.watchdog_irq_enable && 
	      self->tsr.watchdog_irq_status &&
	      (self->msr & (1 << MSR_CE)) )
    {
	/* XXX: add reset state machine */
	ppc_softhvm_raise_crit_interrupt (self, exc_watchdog, regs);
    }
    else if ( (self->event_inject & (1 << evt_external_input)) &&
	      (self->msr & (1 << MSR_EE)) )
    {
	ppc_softhvm_raise_exception (self, exc_ext_input, regs);
    }
    else if ( self->tcr.fixed_interval_irq_enable && 
	      self->tsr.fixed_interval_irq_status &&
	      (self->msr & (1 << MSR_EE)) )
    {
	enter_kdebug("raise fixed interval IRQ");
	ppc_softhvm_raise_exception (self, exc_fixed_interval, regs);
    }
    else if ( self->tcr.dec_irq_enable && 
	      self->tsr.decrementer_irq_status &&
	      (self->msr & (1 << MSR_EE)) )
    {
	//TRACEF("raise decrementer IRQ (%x)\n", static_cast<word_t>(self->dec_base));
	ppc_softhvm_raise_exception (self, exc_decrementer, regs);
    }
}
