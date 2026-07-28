/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2006, 2010,  Karlsruhe University
 *                
 * File path:     platform/generic/intctrl-pic.cc
 * Description:   Implementation of class handling the PIC cascade in
 *                standard PCs
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
 * $Id: intctrl-pic.c,v 1.2 2006/10/07 16:34:09 ud3 Exp $
 *                
 ********************************************************************/

#include <debug.h>
#include INC_GLUE(hwirq.h)
#include INC_GLUE(intctrl.h)
#include INC_GLUE(idt.h)
#include INC_ARCH(trapgate.h)

/* the global interrupt controller */
intctrl_t intctrl;

HW_IRQ( 0);
HW_IRQ( 1);
HW_IRQ( 2);
HW_IRQ( 3);
HW_IRQ( 4);
HW_IRQ( 5);
HW_IRQ( 6);
HW_IRQ( 7);
HW_IRQ( 8);
HW_IRQ( 9);
HW_IRQ(10);
HW_IRQ(11);
HW_IRQ(12);
HW_IRQ(13);
HW_IRQ(14);
HW_IRQ(15);

/* have this asm _after_ the entry points to get forward jumps */
HW_IRQ_COMMON();


void SECTION (".init") intctrl_t_init_arch (intctrl_t *self)
{
    const int base = 0x20;

    /* setup the IDT */
    idt_add_gate (&idt, base+ 0, IDT_TYPE_INTERRUPT, (void(*)(void)) &hwirq_0);
    idt_add_gate (&idt, base+ 1, IDT_TYPE_INTERRUPT, (void(*)(void)) &hwirq_1);
    idt_add_gate (&idt, base+ 2, IDT_TYPE_INTERRUPT, (void(*)(void)) &hwirq_2);
    idt_add_gate (&idt, base+ 3, IDT_TYPE_INTERRUPT, (void(*)(void)) &hwirq_3);
    idt_add_gate (&idt, base+ 4, IDT_TYPE_INTERRUPT, (void(*)(void)) &hwirq_4);
    idt_add_gate (&idt, base+ 5, IDT_TYPE_INTERRUPT, (void(*)(void)) &hwirq_5);
    idt_add_gate (&idt, base+ 6, IDT_TYPE_INTERRUPT, (void(*)(void)) &hwirq_6);
    idt_add_gate (&idt, base+ 7, IDT_TYPE_INTERRUPT, (void(*)(void)) &hwirq_7);
    idt_add_gate (&idt, base+ 8, IDT_TYPE_INTERRUPT, (void(*)(void)) &hwirq_8);
    idt_add_gate (&idt, base+ 9, IDT_TYPE_INTERRUPT, (void(*)(void)) &hwirq_9);
    idt_add_gate (&idt, base+10, IDT_TYPE_INTERRUPT, (void(*)(void)) &hwirq_10);
    idt_add_gate (&idt, base+11, IDT_TYPE_INTERRUPT, (void(*)(void)) &hwirq_11);
    idt_add_gate (&idt, base+12, IDT_TYPE_INTERRUPT, (void(*)(void)) &hwirq_12);
    idt_add_gate (&idt, base+13, IDT_TYPE_INTERRUPT, (void(*)(void)) &hwirq_13);
    idt_add_gate (&idt, base+14, IDT_TYPE_INTERRUPT, (void(*)(void)) &hwirq_14);
    idt_add_gate (&idt, base+15, IDT_TYPE_INTERRUPT, (void(*)(void)) &hwirq_15);

    /* initialize master
       - one slave connected to pin 2
       - reports to vectors 0x20-0x21,0x23-0x27 */
    i8259_init (&self->master, 0x20, base, (1 << 2));

    /* initialize slave
       - its slave id is 2
       - reports to vectors 0x28-0x2F*/
    i8259_init (&self->slave, 0xa0, base+8, 2);

    /* unmask the slave on the master */
    i8259_unmask (&self->master, 2);
}


void intctrl_t_handle_irq (intctrl_t *self, word_t irq)
{
    if (intctrl_t_is_masked (self, irq))
	/* Bogus IRQ raised */
	return;

    intctrl_t_mask_and_ack (self, irq);
    handle_interrupt (irq);
}


/*
 * C entry points declared in glue/v4-x86/intctrl.h.  The APIC path defines the
 * same set in intctrl-apic.c; only one of the two is ever built.
 */

word_t intctrl_get_number_irqs (void)
{ return intctrl_t_get_number_irqs (get_interrupt_ctrl ()); }

bool intctrl_is_irq_available (word_t irq)
{ return intctrl_t_is_irq_available (get_interrupt_ctrl (), irq); }

void intctrl_mask (word_t irq)
{ intctrl_t_mask (get_interrupt_ctrl (), irq); }

bool intctrl_unmask (word_t irq)
{ return intctrl_t_unmask (get_interrupt_ctrl (), irq); }

void intctrl_enable (word_t irq)
{ intctrl_t_enable (get_interrupt_ctrl (), irq); }

void intctrl_disable (word_t irq)
{ intctrl_t_disable (get_interrupt_ctrl (), irq); }

bool intctrl_is_pending (word_t irq)
{ (void) irq; return false; }

void intctrl_set_cpu (word_t irq, word_t cpu)
{ intctrl_t_set_cpu (get_interrupt_ctrl (), irq, cpu); }

void intctrl_init_cpu (void)
{ intctrl_t_init_cpu (get_interrupt_ctrl ()); }

void SECTION (".init") intctrl_init_arch (void)
{ intctrl_t_init_arch (get_interrupt_ctrl ()); }
