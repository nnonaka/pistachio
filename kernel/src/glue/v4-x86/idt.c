/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2007-2008, 2010,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/idt.cc
 * Description:   v4 specific idt implementation
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
 * $Id: idt.cc,v 1.11 2004/05/31 14:15:59 stoess Exp $
 *                
 ********************************************************************/


/* trap definition */
#include INC_ARCH(traps.h)
#include INC_ARCH(segdesc.h)

#include INC_GLUE(syscalls.h)
#include INC_GLUE(idt.h)
#include INC_GLUE(traphandler.h)

#include <debug.h>
#include <ctors.h>

/**
 * idt: the global IDT (see: IA32 Vol 3)
 *
 * Formerly a CTORPRIO_GLOBAL static object; the constructor is now the
 * explicit idt_init() below, called from the boot path (see init.cc).
 */
idt_t idt UNIT("x86.idt");


static void SECTION(".init.system")
idt_init_gate(idt_t *self, word_t index, int type, void (*address)(void))
{
    ASSERT(index < IDT_SIZE);

    switch (type)
    {
    case IDT_TYPE_INTERRUPT:
	x86_idtdesc_set(&self->descriptors[index], X86_KCS, address, X86_IDTDESC_INTERRUPT, 0, 0);
	break;
    case IDT_TYPE_SYSCALL:
	x86_idtdesc_set(&self->descriptors[index], X86_KCS, address, X86_IDTDESC_INTERRUPT, 3, 0);
	break;
    case IDT_TYPE_TRAP:
	x86_idtdesc_set(&self->descriptors[index], X86_KCS, address, X86_IDTDESC_TRAP, 0, 0);
	break;
    }
}

void idt_add_gate(idt_t *self, word_t index, int type, void (*address)(void))
{
    ASSERT(index < IDT_SIZE);


    switch (type)
    {
    case IDT_TYPE_INTERRUPT:
	x86_idtdesc_set(&self->descriptors[index], X86_KCS, address, X86_IDTDESC_INTERRUPT, 0, 0);
	break;
    case IDT_TYPE_SYSCALL:
	x86_idtdesc_set(&self->descriptors[index], X86_KCS, address, X86_IDTDESC_INTERRUPT, 3, 0);
	break;
    case IDT_TYPE_TRAP:
	x86_idtdesc_set(&self->descriptors[index], X86_KCS, address, X86_IDTDESC_TRAP, 0, 0);
	break;
    }
}



/**
 * idt_activate: activates the previously set up IDT
 */
void idt_activate(idt_t *self)
{
    x86_descreg_t reg;
    x86_descreg_set(&reg, (word_t) self->descriptors, sizeof(self->descriptors));
    x86_descreg_setdescreg(&reg, X86_DESCREG_IDTR);
}

void SECTION(".init.cpu") idt_init(idt_t *self)
{
    for (word_t i=0;i<IDT_SIZE;i++){
	/*
	 * Synthesize call to exc_catch_common
	 *
	 * idt
	 * exc_catch_all[IDT_SIZE]
	 * exc_catch_common
	 *
	 * e8 = Near call with 4 byte offset (5 byte)
	 *
	 */
	exc_catch_all[i] = ( (sizeof(exc_catch_all) - i * sizeof(u64_t) - 5) << 8) | 0xe8;
	idt_add_gate(self, i, IDT_TYPE_INTERRUPT, (func_exc) &exc_catch_all[i]);
    }

    /* setup the exception gates */
#if defined(CONFIG_DEBUG)
    idt_init_gate(self, X86_EXC_DEBUG, IDT_TYPE_INTERRUPT, exc_debug);
    idt_init_gate(self, X86_EXC_NMI, IDT_TYPE_INTERRUPT, exc_nmi);
    idt_init_gate(self, X86_EXC_BREAKPOINT, IDT_TYPE_SYSCALL, exc_breakpoint);
#endif
    idt_init_gate(self, X86_EXC_INVALIDOPCODE, IDT_TYPE_INTERRUPT, exc_invalid_opcode);
    idt_init_gate(self, X86_EXC_NOMATH_COPROC, IDT_TYPE_INTERRUPT, exc_nomath_coproc);
    idt_init_gate(self, X86_EXC_GENERAL_PROTECTION, IDT_TYPE_INTERRUPT, exc_gp);
    idt_init_gate(self, X86_EXC_PAGEFAULT, IDT_TYPE_INTERRUPT, exc_pagefault);
    // 15 reserved

#if defined(CONFIG_SUBARCH_X32)
    // syscalls
    idt_init_gate(self, 0x30, IDT_TYPE_SYSCALL, exc_user_sysipc);
    idt_init_gate(self, 0x31, IDT_TYPE_SYSCALL, exc_user_syscall);
#endif
}
