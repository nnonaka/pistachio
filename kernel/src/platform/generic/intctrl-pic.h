/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2008,  Karlsruhe University
 *                
 * File path:     platform/generic/intctrl-pic.h
 * Description:   PIC cascade in standard PCs
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
 * $Id: intctrl-pic.h,v 1.2 2006/10/19 22:57:36 ud3 Exp $
 *                
 ********************************************************************/
#ifndef __PLATFORM__GENERIC__INTCTRL_PIC_H__
#define __PLATFORM__GENERIC__INTCTRL_PIC_H__

#ifndef __GENERIC__INTCTRL_H__
#error not for standalone inclusion
#endif

#include INC_PLAT(8259.h)

/* was class intctrl_t : public generic_intctrl_t.  generic_intctrl_t was an
   interface description with no data, so nothing is embedded here. */
struct intctrl_t {
    i8259_pic_t	master;
    i8259_pic_t	slave;
};
typedef struct intctrl_t intctrl_t;

extern intctrl_t intctrl;

INLINE intctrl_t * get_interrupt_ctrl (void) { return &intctrl; }

BEGIN_DECLS
void intctrl_t_init_arch (intctrl_t *self);
/* handle_irq keeps its asm name: the HW_IRQ stubs branch to it. */
void intctrl_t_handle_irq (intctrl_t *self, word_t irq) __asm__("intctrl_t_handle_irq");
END_DECLS

INLINE void intctrl_t_init_cpu (intctrl_t *self)	{ (void) self; /* dummy */ }

/* forward mask to the appropriate PIC */
INLINE void intctrl_t_mask (intctrl_t *self, word_t irq)
{
    (irq < 8) ? i8259_mask (&self->master, irq) : i8259_mask (&self->slave, irq - 8);
}

/* forward unmask to the appropriate PIC */
INLINE bool intctrl_t_unmask (intctrl_t *self, word_t irq)
{
    (irq < 8) ? i8259_unmask (&self->master, irq) : i8259_unmask (&self->slave, irq - 8);
    return false;
}

/* check if interrupt is masked on PIC */
INLINE bool intctrl_t_is_masked (intctrl_t *self, word_t irq)
{
    return (irq >= 8) ? i8259_is_masked (&self->slave, irq - 8)
		      : i8259_is_masked (&self->master, irq);
}

INLINE void intctrl_t_enable (intctrl_t *self, word_t irq)  { intctrl_t_unmask (self, irq); }
INLINE void intctrl_t_disable (intctrl_t *self, word_t irq) { intctrl_t_mask (self, irq); }
INLINE bool intctrl_t_is_enabled (intctrl_t *self, word_t irq)
{ return ! intctrl_t_is_masked (self, irq); }

INLINE void intctrl_t_ack (intctrl_t *self, word_t irq)
{
    if (irq >= 8)
    {
	i8259_ack (&self->slave, irq - 8);
	i8259_ack (&self->master, 2);
    }
    else
	i8259_ack (&self->master, irq);
}

INLINE void intctrl_t_mask_and_ack (intctrl_t *self, word_t irq)
{
    intctrl_t_mask (self, irq);
    intctrl_t_ack (self, irq);
}

INLINE word_t intctrl_t_get_number_irqs (intctrl_t *self)  { (void) self; return 16; }

INLINE void intctrl_t_set_cpu (intctrl_t *self, word_t irq, word_t cpu)
{ (void) self; (void) irq; (void) cpu; /* dummy */ }

/* was declared here and defined INLINE in glue/v4-x86/intctrl.h */
INLINE bool intctrl_t_is_irq_available (intctrl_t *self, word_t irq)
{
    (void) self;
    return (irq != 8 && irq != 2);
}

#endif /* !__PLATFORM__GENERIC__INTCTRL_PIC_H__ */
