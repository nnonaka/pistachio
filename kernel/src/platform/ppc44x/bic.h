/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     platform/ppc44x/bic.h
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
#ifndef __PLATFORM__PPC44X__BIC_H__
#define __PLATFORM__PPC44X__BIC_H__

#include <intctrl.h>
#include <sync.h>

#define BGP_MAX_CORE	4
#define BGP_MAX_GROUPS	15
#define BGP_MAX_IRQS	(BGP_MAX_GROUPS * 32)

/* Was bgic_t::bgic_group_t; C has no nested types, so it is file-scope.  The
   layout is hardware-defined -- keep the packed attribute and field order. */
struct bgic_group_t
{
	volatile word_t status;		// status (read and write) 0
	volatile word_t rd_clr_status;	// status (read and clear) 4
	volatile word_t status_clr;	// status (write and clear)8
	volatile word_t status_set;	// status (write and set) c
	
	// 4 bits per IRQ
	volatile word_t target_irq[4];	// target selector 10-20
	volatile word_t noncrit_mask[BGP_MAX_CORE];	// mask 20-30
	volatile word_t crit_mask[BGP_MAX_CORE];	// mask 30-40
	volatile word_t mchk_mask[BGP_MAX_CORE];	// mask 40-50
	unsigned char __align[0x80 - 0x50];
} __attribute__((packed));
typedef struct bgic_group_t bgic_group_t;

struct bgic_t
{
    bgic_group_t groups[BGP_MAX_GROUPS];
    volatile word_t core_non_crit[BGP_MAX_CORE];
    volatile word_t core_crit[BGP_MAX_CORE];
    volatile word_t core_mchk[BGP_MAX_CORE];

};
typedef struct bgic_t bgic_t;

/* The `const word_t' return types on the four index helpers were meaningless
   on a by-value return; they are plain word_t here. */
INLINE word_t bgic_irq_to_group (word_t hwirq)	{ return (hwirq >> 5) & 0xf; }
INLINE word_t bgic_group_to_irq (word_t group)	{ return group << 5; }
INLINE word_t bgic_irq_of_group (word_t hwirq)	{ return hwirq & 0x1f; }

INLINE word_t bgic_get_group_mask (word_t hwirq)
{ return 1U << (31 - bgic_irq_of_group (hwirq)); }

INLINE void bgic_set_target (bgic_t *self, word_t hwirq, word_t target)
{
    word_t offset = ((7 - (hwirq & 0x7)) * 4);
    volatile word_t *reg;
    word_t val;

    reg = &self->groups[bgic_irq_to_group (hwirq)].target_irq[bgic_irq_of_group (hwirq) / 8];
    val = *reg;
    sync();
    *reg = (val & ~(0xf << offset)) | ((target & 0xf) << offset);
    sync();
}

INLINE word_t bgic_cpu_to_noncrit_target (word_t cpu)	{ return 0x4 | cpu; }
INLINE word_t bgic_cpu_to_crit_target (word_t cpu)	{ return 0x8 | cpu; }
INLINE word_t bgic_cpu_to_mcheck_target (word_t cpu)	{ return 0xc | cpu; }

INLINE void bgic_mask_irq (bgic_t *self, word_t hwirq)
{ bgic_set_target (self, hwirq, 0); }

INLINE void bgic_unmask_irq (bgic_t *self, word_t hwirq, word_t cpu)
{ bgic_set_target (self, hwirq, bgic_cpu_to_noncrit_target (cpu)); }

INLINE bool bgic_is_masked (bgic_t *self, word_t hwirq)
{
    word_t offset = ((7 - (hwirq & 0x7)) * 4);
    word_t val;

    ASSERT(hwirq < BGP_MAX_IRQS);
    val = self->groups[bgic_irq_to_group (hwirq)].target_irq[bgic_irq_of_group (hwirq) / 8];
    sync();
    /* NB: this reads as (val & (0xf << offset)) == 0 only because == binds
       tighter than & in C; preserved verbatim from the C++ original. */
    return val & (0xf << offset) == 0;
}

INLINE bool bgic_is_pending (bgic_t *self, word_t hwirq)
{
    ASSERT(hwirq < BGP_MAX_IRQS);
    return self->groups[bgic_irq_to_group (hwirq)].status & (1 << (31 - bgic_irq_of_group (hwirq)));
}

INLINE void bgic_raise_irq (bgic_t *self, word_t hwirq)
{
    ASSERT(hwirq < BGP_MAX_IRQS);
    sync();
    self->groups[bgic_irq_to_group (hwirq)].status_set = bgic_get_group_mask (hwirq);
}

INLINE void bgic_ack_irq (bgic_t *self, word_t hwirq)
{
    ASSERT(hwirq < BGP_MAX_IRQS);
    sync();
    self->groups[bgic_irq_to_group (hwirq)].status_clr = bgic_get_group_mask (hwirq);
}

INLINE void bgic_mask_and_clear_all (bgic_t *self)
{
    unsigned idx;

    sync();
    for (idx = 0; idx < BGP_MAX_GROUPS; idx++)
    {
	self->groups[idx].target_irq[0] = 0;
	self->groups[idx].target_irq[1] = 0;
	self->groups[idx].target_irq[2] = 0;
	self->groups[idx].target_irq[3] = 0;
	self->groups[idx].status = 0;
	eieio();
    }
}

int  bgic_get_pending_irq (bgic_t *self, word_t core);
void bgic_dump (bgic_t *self);



/* generic_intctrl_t was an interface-description base with no members; it is
   gone, so as on x86 the struct stands alone.  The intctrl_* entry points take
   no receiver, matching the contract api/v4 and generic code already call --
   they operate on the single `intctrl' object. */
struct intctrl_t
{
    bgic_t *ctrl;
    /* we can route to 4 CPUs, and thus can encode 16 targets in a word */
    u8_t routing[BGP_MAX_IRQS / 4];	/* 4 IRQs per byte */
    spinlock_t lock;
    word_t num_irqs;

    u64_t phys_addr;
    word_t mem_size;
};
typedef struct intctrl_t intctrl_t;

/* Defined here rather than in glue/v4-powerpc/intctrl.h, which includes this
   header before it would get to the definition -- the inline entry points
   below need it. */
INLINE intctrl_t * get_interrupt_ctrl (void)
{
    extern intctrl_t intctrl;
    return &intctrl;
}

INLINE word_t intctrl_get_irq_routing (intctrl_t *self, word_t irq)
{
    word_t shift = (irq % 4) * 2;
    return (self->routing[irq / 4] >> shift) & 3;
}

INLINE void intctrl_set_irq_routing (intctrl_t *self, word_t irq, word_t cpu)
{
    word_t shift = (irq % 4) * 2;
    self->routing[irq / 4] = (self->routing[irq / 4] & ~(3 << shift)) | (cpu << shift);
}

INLINE word_t intctrl_get_ipi_irq (word_t cpu, word_t ipi)
{ return cpu * 8 + ipi; }

INLINE word_t intctrl_get_number_irqs (void)
{ return get_interrupt_ctrl()->num_irqs; }

INLINE bool intctrl_is_irq_available (word_t irq)
{ return irq >= 32; }

INLINE bool intctrl_is_masked (word_t irq)
{ return bgic_is_masked (get_interrupt_ctrl()->ctrl, irq); }

INLINE bool intctrl_is_pending (word_t irq)
{ return bgic_is_pending (get_interrupt_ctrl()->ctrl, irq); }

INLINE void intctrl_mask (word_t irq)
{
    intctrl_t *self = get_interrupt_ctrl();

    spinlock_lock (&self->lock);
    bgic_mask_irq (self->ctrl, irq);
    spinlock_unlock (&self->lock);
}

INLINE bool intctrl_unmask (word_t irq)
{
    intctrl_t *self = get_interrupt_ctrl();
    bool pending;

    ASSERT(irq < BGP_MAX_IRQS);
    spinlock_lock (&self->lock);
    bgic_ack_irq (self->ctrl, irq);
    pending = intctrl_is_pending (irq);
    if (!pending)
	bgic_unmask_irq (self->ctrl, irq, intctrl_get_irq_routing (self, irq));
    spinlock_unlock (&self->lock);
    return pending;
}

INLINE void intctrl_enable (word_t irq)		{ intctrl_unmask (irq); }
INLINE void intctrl_disable (word_t irq)	{ intctrl_mask (irq); }
INLINE bool intctrl_is_enabled (word_t irq)	{ return intctrl_is_masked (irq); }

INLINE void intctrl_set_cpu (word_t irq, word_t cpu)
{
    intctrl_t *self = get_interrupt_ctrl();

    intctrl_set_irq_routing (self, irq, cpu);
    if (!bgic_is_masked (self->ctrl, irq))
	bgic_unmask_irq (self->ctrl, irq, intctrl_get_irq_routing (self, irq));
}

INLINE void intctrl_dump (void)
{ bgic_dump (get_interrupt_ctrl()->ctrl); }

/* Defined in platform/ppc44x. */
void intctrl_init_arch (void);
void intctrl_init_cpu (int cpu);
void intctrl_handle_irq (word_t cpu);	/* handler invoked on interrupt */
void intctrl_map (void);		/* map routine provided by glue */
void intctrl_start_new_cpu (word_t cpu);	/* SMP support */
void intctrl_send_ipi (word_t cpu);

#endif /* !__PLATFORM__PPC44X__BIC_H__ */
