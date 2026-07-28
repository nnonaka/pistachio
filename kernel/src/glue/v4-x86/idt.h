/*********************************************************************
 *                
 * Copyright (C) 2002, 2007-2008, 2010,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/idt.h
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
 * $Id: idt.h,v 1.10 2003/09/24 19:04:36 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __GLUE_V4_X86__X32__IDT_H__
#define __GLUE_V4_X86__X32__IDT_H__

#include <debug.h>
#include INC_ARCH(segdesc.h)
#include INC_GLUE(config.h)

/* idt gate types, as macros so C (idt.c) and C++ (via type_e) share values. */
#define IDT_TYPE_INTERRUPT	0
#define IDT_TYPE_SYSCALL	1
#define IDT_TYPE_TRAP		2

struct idt_t
{
    x86_idtdesc_t descriptors[IDT_SIZE];
};
typedef struct idt_t idt_t;

BEGIN_DECLS
/* C free-function API (defined in idt.c); replaces the former idt_t methods
   and constructor. idt_init() takes the place of the static constructor and
   is now called explicitly from the boot path. */
void idt_init(idt_t *self);
void idt_add_gate(idt_t *self, word_t index, int type, void (*address)(void));
void idt_activate(idt_t *self);
END_DECLS


extern idt_t idt;

#endif /* !__GLUE_V4_X86__X32__IDT_H__ */
