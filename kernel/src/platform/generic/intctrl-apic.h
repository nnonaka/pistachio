/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2006-2007,  Karlsruhe University
 *                
 * File path:     platform/generic/intctrl-apic.h
 * Description:   Driver for APIC+IOAPIC
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
 * $Id: intctrl-apic.h,v 1.5 2006/10/19 22:57:36 ud3 Exp $
 *                
 ********************************************************************/
#ifndef __PLATFORM__GENERIC__INTCTRL_APIC_H__
#define __PLATFORM__GENERIC__INTCTRL_APIC_H__

#ifndef __GENERIC__INTCTRL_H__
#error not for standalone inclusion
#endif

#include INC_PLAT(82093.h)
#include INC_ARCH(apic.h)
#include INC_ARCH(apic.h)
#include INC_ARCH(ioport.h)
#include <sync.h>
#include <linear_ptab.h>

#define NUM_REDIR_ENTRIES	(CONFIG_MAX_IOAPICS * I82093_NUM_IRQS)


/* C mirror of intctrl_t and its nested types.  generic_intctrl_t is an empty
   base (methods only) so it contributes nothing to the layout; the members are
   restated in declaration order.  local_apic is a static member (no storage). */
struct intctrl_ioapic_t {
    word_t	id;
    i82093_t *	i82093;
    spinlock_t	lock;
};
typedef struct intctrl_ioapic_t intctrl_ioapic_t;

struct intctrl_redir_table_t {
    ioapic_redir_t	entry;
    intctrl_ioapic_t *	ioapic;
    word_t		line;
    bool		pending;
};
typedef struct intctrl_redir_table_t intctrl_redir_table_t;

struct intctrl_t {
    intctrl_ioapic_t	 ioapics[CONFIG_MAX_IOAPICS];
    intctrl_redir_table_t redir[NUM_REDIR_ENTRIES];

    word_t	num_intsources;
    word_t	max_intsource;

    /* apic id handling */
    word_t	num_ioapics;
    word_t	num_cpus;
    spinlock_t	idt_lock;

    bool	pmtimer_available;
    word_t	pmtimer_ioport;
};
typedef struct intctrl_t intctrl_t;

/* sync_redir_part_e */
#define INTCTRL_SYNC_LOW	0
#define INTCTRL_SYNC_HIGH	1
#define INTCTRL_SYNC_ALL	2

#define INTCTRL_PMTIMER_TICKS	3579545
#define INTCTRL_PMTIMER_MASK	0xFFFFFF

extern intctrl_t intctrl;
INLINE intctrl_t * get_interrupt_ctrl (void) { return &intctrl; }



#endif /* !__PLATFORM__GENERIC__INTCTRL_APIC_H__ */
