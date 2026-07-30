/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     arch/powerpc/softhvm.h
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
#pragma once

#include "msr.h"
#include "swtlb.h"
#include "frame.h"

#define LAZY_TLB

struct ppc_instr_t {
    union {
	word_t raw;
	struct {
	    word_t primary : 6;
	    word_t li : 26;
	} iform;
	struct {
	    word_t primary : 6;
	    word_t bo : 5;
	    word_t bi : 5;
	    word_t bd : 14;
	    word_t aa : 1;
	    word_t lk : 1;
	} bform;
	struct {
	    word_t primary : 6;
	    word_t     : 24;
	    word_t one : 1;
	    word_t     : 1;
	} scform;
	struct {
	    word_t primary : 6;
	    word_t rt : 5;
	    word_t ra : 5;
	    word_t d : 16;
	} dform;
	struct {
	    word_t primary : 6;
	    word_t rt : 5;
	    word_t ra : 5;
	    word_t rb : 5;
	    word_t xo : 10;
	    word_t rc : 1;
	} xform;
	struct {
	    word_t primary : 6;
	    word_t bt : 5;
	    word_t ba : 5;
	    word_t bb : 5;
	    word_t xo : 10;
	    word_t lk : 1;
	} xlform;
	struct {
	    word_t primary : 6;
	    word_t rt : 5;
	    word_t rf : 10;
	    word_t xo : 10;
	    word_t    : 1;
	} xfxform;
	struct {
	    word_t primary : 6;
	    word_t rt : 5;
	    word_t ra : 5;
	    word_t rb : 5;
	    word_t oe : 1;
	    word_t xo : 9;
	    word_t rc : 1;
	} xoform;
	struct {
	    word_t primary : 6;
	    word_t rs : 5;
	    word_t ra : 5;
	    word_t rb : 5;
	    word_t mb : 5;
	    word_t me : 5;
	    word_t rc : 1;
	} mform;
    } __attribute((packed));
};
typedef struct ppc_instr_t ppc_instr_t;

/* The ppc_instr_t(word_t) constructor becomes an explicit initialiser; the
   accessors take the instruction by value, as the const methods did. */
#define PPC_INSTR(val)	((ppc_instr_t) { .raw = (val) })

INLINE int    ppc_instr_get_primary (ppc_instr_t self)	{ return self.xform.primary; }
INLINE int    ppc_instr_get_secondary (ppc_instr_t self){ return self.xform.xo; }
INLINE word_t ppc_instr_ra (ppc_instr_t self)		{ return self.xform.ra; }
INLINE word_t ppc_instr_rb (ppc_instr_t self)		{ return self.xform.rb; }
INLINE word_t ppc_instr_rt (ppc_instr_t self)		{ return self.xform.rt; }
INLINE word_t ppc_instr_rf (ppc_instr_t self)		{ return self.xform.rb << 5 | self.xform.ra; }
INLINE s16_t  ppc_instr_d (ppc_instr_t self)		{ return self.dform.d; }


/* Was ppc_softhvm_t::tlb_t.  C has no nested types, so it is file-scope; the
   name keeps the owner as a prefix.  Layout is load-bearing -- ctrlxfer_get/set
   index it as a flat word array. */
struct ppc_hvm_tlb_t {
    /* WARNING: If you change this layout, adopt ppc_hvm_tlb_ctrlxfer_{get,set}! */
    ppc_tlb0_t tlb0;
    ppc_tlb1_t tlb1;
    ppc_tlb2_t tlb2;
    word_t     pid;
    ppc_tlb1_t phys_tlb1;	/* phys shadow */

    bool       touched;		/* true if entry was ever fetched in host TLB */
} __attribute__((packed));
typedef struct ppc_hvm_tlb_t ppc_hvm_tlb_t;

INLINE bool ppc_hvm_tlb_vaddr_in_entry (ppc_hvm_tlb_t *self, word_t vaddr, u8_t searchpid)
{
    return ppc_tlb0_is_valid (&self->tlb0) &&
	ppc_tlb0_is_vaddr_covered (&self->tlb0, vaddr) &&
	(self->pid == 0 || self->pid == searchpid);
}

INLINE void ppc_hvm_tlb_touch (ppc_hvm_tlb_t *self, unsigned index)
{ self->touched = true; }

INLINE word_t ppc_hvm_tlb_ctrlxfer_get (ppc_hvm_tlb_t *self, word_t reg)
{ return ((word_t *) &self->tlb0)[reg]; }

INLINE void ppc_hvm_tlb_ctrlxfer_set (ppc_hvm_tlb_t *self, word_t reg, word_t val)
{ ((word_t *) &self->tlb0)[reg] = val; }


enum {
	exc_critical_input = 0,
	exc_machine_check = 1,
	exc_data_storage = 2,
	exc_instr_storage = 3,
	exc_ext_input = 4,
	exc_alignment = 5,
	exc_program = 6,
	exc_fpu_unavail = 7,
	exc_system_call = 8,
	exc_aux_unavail = 9,
	exc_decrementer = 10,
	exc_fixed_interval = 11,
	exc_watchdog = 12,
	exc_data_tlb = 13,
	exc_instr_tlb = 14,
	exc_debug = 15,
};
enum {
	evt_machine_check,
	evt_debug,
	evt_critical_input,
	evt_external_input,
	evt_last,
};

struct ppc_softhvm_t
{
    /* supervisor */
    word_t msr;		// machine status
    word_t pvr;		// proc version
    word_t pir;		// proc id
    word_t ccr0;	// core config reg 0
    word_t ccr1;	// core config reg 1
    word_t rstcfg;	// reset config
    word_t sprg[8];	// spr
    word_t esr;		// exc syndrome
    word_t mcsr;	// machine check syndrome
    word_t dear;	// data exception address
    word_t srr0, srr1;	// save/restore
    word_t csrr0, csrr1;// critical save/restore
    word_t mcsrr0, mcsrr1; // machine check save/restore
    word_t ivpr;	// int vector prefix
    word_t ivor[16];	// int vector offset
    word_t tbu, tbl;	// time base
    ppc_tcr_t tcr;	// timer ctrl
    ppc_tsr_t tsr;	// timer status
    word_t dec;		// decrementer
    word_t decar;	// decrememter auto-reload
    word_t ivlim;	// instruction cache victim limit
    word_t inv[4];	// instruction cache normal victim
    word_t itv[4];	// instruction cache transient victim
    word_t dvlim;	// data cache victim limit
    word_t dnv[4];	// data cache normal victim
    word_t dtv[4];	// data cache transient victim
    word_t pid;		// process id
    ppc_mmucr_t mmucr;	// mmu control
    word_t dbsr;	// debug status
    word_t dbdr;	// debug data
    word_t dbcr[3];	// debug control
    word_t dac[2];	// data address compare
    word_t dvc[2];	// data value compare
    word_t iac[4];	// instruction address compare
    word_t icdbdr;	// instruction cache debug data
    word_t icdbt[2];	// instruction cache debug tag
    word_t dcdbt[2];	// data cache debug tag
    word_t event_inject;	// injection of events
    /* TLB */
    ppc_hvm_tlb_t tlb[PPC_MAX_TLB_ENTRIES];
    ppc_hvm_tlb_t shadow_tlb;
    u8_t shadow_ref;
    bool htlb_dirty;
    word_t tlb_dirty_start;
    word_t tlb_dirty_end;
    /* Timer facilities */
    u64_t dec_base;
    u64_t watchdog_base;
    u64_t fixed_interval_base;
    /* emulation functions */
};
typedef struct ppc_softhvm_t ppc_softhvm_t;

bool ppc_softhvm_mfmsr (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs);

bool ppc_softhvm_mtmsr (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs);

bool ppc_softhvm_mfspr (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs);

bool ppc_softhvm_mtspr (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs);

bool ppc_softhvm_tlbre (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs);

bool ppc_softhvm_tlbsx (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs);

bool ppc_softhvm_tlbsync (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs);

bool ppc_softhvm_tlbwe (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs);

bool ppc_softhvm_tw (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs);

bool ppc_softhvm_wrtee (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs);

bool ppc_softhvm_wrteei (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs);

bool ppc_softhvm_rfi (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs, word_t srr0, word_t srr1);

bool ppc_softhvm_twi (ppc_softhvm_t *self, ppc_instr_t instr, except_regs_t *regs);

bool ppc_softhvm_set_esr (ppc_softhvm_t *self, word_t val, except_regs_t *regs);

void ppc_softhvm_set_msr (ppc_softhvm_t *self, word_t val);

word_t * ppc_softhvm_get_spr (ppc_softhvm_t *self, int spr, bool read);

void ppc_softhvm_inject_pending_events (ppc_softhvm_t *self, except_regs_t *regs);

void ppc_softhvm_update_decrementer (ppc_softhvm_t *self);

/* Forward declaration: invalidate_shadow_tlb and replace_shadow_tlb call this
   before its definition below. */
INLINE void ppc_softhvm_update_tlb_dirty (ppc_softhvm_t *self, ppc_hvm_tlb_t *entry);

INLINE bool ppc_softhvm_is_user (ppc_softhvm_t *self)
	{ return self->msr & (1U << MSR_PR); }

INLINE bool ppc_softhvm_in_shadow_tlb (ppc_softhvm_t *self, word_t vaddr)
	{
	    return ppc_hvm_tlb_vaddr_in_entry (&self->shadow_tlb, vaddr, self->pid);
	}

INLINE void ppc_softhvm_invalidate_shadow_tlb (ppc_softhvm_t *self)
	{
	    if (!ppc_tlb0_is_valid (&self->shadow_tlb.tlb0))
		return;

	    if (self->tlb[self->shadow_ref].tlb0.raw != self->shadow_tlb.tlb0.raw ||
		self->tlb[self->shadow_ref].tlb1.raw != self->shadow_tlb.tlb1.raw ||
		self->tlb[self->shadow_ref].tlb2.raw != self->shadow_tlb.tlb2.raw)
	    {
#if 0
		if (self->shadow_tlb.touched)
		    TRACEF("invalidate shadow TLB and flush (entry: %d, %08x, %08x, %08x, dirty: %d)\n", 
			   self->shadow_ref, self->shadow_tlb.tlb0.raw, self->shadow_tlb.tlb1.raw, self->shadow_tlb.tlb2.raw, self->shadow_tlb.touched);
#endif
		ppc_softhvm_update_tlb_dirty (self, &self->shadow_tlb);
	    }
	    self->shadow_tlb.tlb0 = ppc_tlb0_invalid ();
	}

INLINE void ppc_softhvm_replace_shadow_tlb (ppc_softhvm_t *self, ppc_hvm_tlb_t *tlbentry, int idx)
	{
	    ppc_softhvm_update_tlb_dirty (self, &self->shadow_tlb);
#if 0	    
	    self->shadow_ref = idx;
	    self->shadow_tlb = *tlbentry;
#else
	    self->shadow_tlb.tlb0 = ppc_tlb0_invalid ();
#endif
	    self->shadow_tlb.touched = false;
	}

/* The parameter was named `tlb', shadowing the member of the same name. */
INLINE void ppc_softhvm_update_tlb_dirty (ppc_softhvm_t *self, ppc_hvm_tlb_t *entry)
{
    if (entry->touched)
    {
	self->htlb_dirty = true;
	self->tlb_dirty_start = ppc_tlb0_get_vaddr (&entry->tlb0);
	self->tlb_dirty_end = self->tlb_dirty_start + ppc_tlb0_get_size (&entry->tlb0) - 1;
	entry->touched = false;
    }
}

INLINE void ppc_softhvm_clear_tlb_dirty (ppc_softhvm_t *self)
	{
	    self->shadow_tlb.touched = false;
	    for (int i = 0; i < PPC_MAX_TLB_ENTRIES; i++)
		self->tlb[i].touched = false;
	}

INLINE int ppc_softhvm_find_tlb_entry (ppc_softhvm_t *self, word_t vaddr)
	{
	    for (int i = 0; i < PPC_MAX_TLB_ENTRIES; i++) 
		if (ppc_hvm_tlb_vaddr_in_entry (&self->tlb[i], vaddr, self->pid))
		    return i;
	    return -1;
	}

INLINE word_t ppc_softhvm_get_ivor_ip (ppc_softhvm_t *self, int index)
	{
	    return self->ivpr | self->ivor[index];
	}

void ppc_softhvm_raise_exception (ppc_softhvm_t *self, int num, except_regs_t *regs);

void ppc_softhvm_raise_noncrit_interrupt (ppc_softhvm_t *self, int num, except_regs_t *regs);

void ppc_softhvm_raise_crit_interrupt (ppc_softhvm_t *self, int num, except_regs_t *regs);

void ppc_softhvm_raise_mcheck_interrupt (ppc_softhvm_t *self, int num, except_regs_t *regs);

bool ppc_softhvm_emulate_instruction (ppc_softhvm_t *self, word_t instr, except_regs_t *regs);

void ppc_softhvm_update_timers (ppc_softhvm_t *self, u64_t time);

INLINE void ppc_softhvm_handle_pending_events (ppc_softhvm_t *self, except_regs_t *regs)
	{
	    if ((self->event_inject & ((1 << evt_last) - 1)) || ppc_tsr_pending_irqs (&self->tsr))
		ppc_softhvm_inject_pending_events (self, regs);
	}

INLINE void ppc_softhvm_load_guest_sprs (ppc_softhvm_t *self)
	{
	    ppc_set_spr(SPR_SPRG4, self->sprg[4]);
	    ppc_set_spr(SPR_SPRG5, self->sprg[5]);
	    ppc_set_spr(SPR_SPRG6, self->sprg[6]);
	    ppc_set_spr(SPR_SPRG7, self->sprg[7]);
	}

INLINE word_t ppc_softhvm_get_pid_for_msr (ppc_softhvm_t *self)
	{ return ppc_is_kernel_mode(self->msr) ? 2 : 1; }

void ppc_softhvm_init (ppc_softhvm_t *self);

