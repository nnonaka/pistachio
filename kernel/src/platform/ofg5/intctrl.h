/****************************************************************************
 *
 * Copyright (C) 2003,2005  National ICT Australia (NICTA)
 *
 * File path:	platform/ofg5/intctrl.h
 * Description:	G5 interrupt controller declarations (XICS).
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
 * $Id: intctrl.h,v 1.1 2005/01/18 13:48:12 cvansch Exp $
 *
 ***************************************************************************/

#ifndef __PLATFORM__OFG5__INTCTRL_H__
#define __PLATFORM__OFG5__INTCTRL_H__

/* generic_intctrl_t was an interface-description base with no members, so as
   in platform/ofppc the struct stands alone and the members become free
   intctrl_* entry points -- the same contract every other platform exposes.
   This controller is a stub: upstream leaves all of it UNIMPLEMENTED and
   reports a single interrupt. */
struct intctrl_t
{
    word_t _unused;
};
typedef struct intctrl_t intctrl_t;

INLINE intctrl_t * get_interrupt_ctrl (void)
{
    extern intctrl_t intctrl;
    return &intctrl;
}

INLINE void intctrl_mask (word_t irq)
{
    UNIMPLEMENTED();
}
INLINE bool intctrl_unmask (word_t irq)
{
    UNIMPLEMENTED();
    return false;
}
INLINE void intctrl_disable (word_t irq)
{
    UNIMPLEMENTED();
}
INLINE bool intctrl_enable (word_t irq)
{
    UNIMPLEMENTED();
    return false;
}
INLINE void intctrl_mask_and_ack (word_t irq)	{ UNIMPLEMENTED(); }
INLINE void intctrl_ack (word_t irq)		{ UNIMPLEMENTED(); }

/* For now, we only export 1 interrupt */
INLINE word_t intctrl_get_number_irqs (void)	{ return 1; }

INLINE bool intctrl_is_irq_available (word_t irq)	{ return (irq == 0); }

INLINE void intctrl_set_cpu (word_t irq, word_t cpu)	{ UNIMPLEMENTED(); }

/* Out of line in platform/ofg5/intr.c. */
BEGIN_DECLS
void intctrl_init_arch (void);
void intctrl_init_cpu (void);
END_DECLS

#endif /* __PLATFORM__OFG5__INTCTRL_H__ */
