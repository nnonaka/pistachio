/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2007,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/intctrl.h
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
 * $Id: intctrl.h,v 1.6 2006/10/19 22:57:35 ud3 Exp $
 *                
 ********************************************************************/

#ifndef __GLUE__V4_X86__INTCTRL_H__
#define __GLUE__V4_X86__INTCTRL_H__

#include <intctrl.h>

/* find the proper intctrl_t class */
#if defined(CONFIG_IOAPIC)
#include <platform/generic/intctrl-apic.h>
#else /* !defined(CONFIG_IOAPIC) */
#include <platform/generic/intctrl-pic.h>
/* is_irq_available is INLINE in intctrl-pic.h (notes §117). */
# endif /* !defined(CONFIG_IOAPIC) */

/**
 * @return pointer to interrupt controller
 */

/* C entry points for the interrupt controller (defined in intctrl-apic.cc). */
BEGIN_DECLS
bool intctrl_has_pmtimer(void);
void intctrl_pmtimer_wait(word_t ms);
word_t intctrl_get_number_irqs(void);
bool intctrl_is_irq_available(word_t irq);
void intctrl_mask(word_t irq);
bool intctrl_unmask(word_t irq);
void intctrl_enable(word_t irq);
void intctrl_disable(word_t irq);
bool intctrl_is_pending(word_t irq);
void intctrl_set_cpu(word_t irq, word_t cpu);
void intctrl_init_cpu(void);
void intctrl_init_arch(void);
word_t apic_get_id(void);
void apic_send_init_ipi(word_t id, bool assert);
void apic_send_startup_ipi(word_t id, void (*startup)(void));
END_DECLS

#endif /* !__GLUE__V4_X86__INTCTRL_H__ */
