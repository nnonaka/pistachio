/*********************************************************************
 *                
 * Copyright (C) 2002, 2004-2010,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/x32/tcb.h
 * Description:   TCB related functions for Version 4, IA-32
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
 * $Id: tcb.h,v 1.69 2007/01/22 21:03:13 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __GLUE_V4_X86__X32__TCB_H__
#define __GLUE_V4_X86__X32__TCB_H__

#include INC_ARCH_SA(tss.h)			/* for x86_x32_tss_t */
#include INC_API(fpage.h)
#include INC_API(thread.h)

#ifndef BUILD_TCB_LAYOUT
#include <tcb_layout.h>	/* C-safe: pure offset #defines */
#endif /* !defined(BUILD_TCB_LAYOUT) */

/*
 * What used to be here -- copy_mrs, switch_to, do_ipc, the notify trio,
 * return_from_ipc, return_from_user_interruption -- are now C functions in
 * glue/v4-x86/thread.c, beside their x64 counterparts and under the same
 * CONFIG_IS_64BIT split that file already used for return_to_user.  This
 * header is left with what x64/tcb.h also keeps: the current-TCB lookup.
 */

INLINE tcb_t * get_current_tcb()
{
    addr_t stack;
    asm ("lea -4(%%esp), %0" :"=r" (stack));
    return (tcb_t *) ((word_t) stack & KTCB_MASK);
}

/**
 * ipc_string_copy: architecture specific string copy.  
 * TODO: consider the string copy memory hints.
 */
#define IPC_STRING_COPY ipc_string_copy
INLINE void ipc_string_copy(void *dst, const void *src, word_t len)
{
    word_t dummy1, dummy2, dummy3;
#if defined(CONFIG_X86_SMALL_SPACES)
    asm volatile ("mov %0, %%es" : : "r" (X86_KDS));
#endif
    asm volatile (
	    "jecxz 1f\n"
	    "repnz movsl (%%esi), (%%edi)\n"
	    "1: test $3, %%edx\n"
	    "jz 1f\n"
	    "mov %%edx, %%ecx\n"
	    "repnz movsb (%%esi), (%%edi)\n"
	    "1:\n"
	    : "=S"(dummy1), "=D"(dummy2), "=c"(dummy3)
	    : "S"(src), "D"(dst), "c"(len >> 2), "d"(len & 3));
#if defined(CONFIG_X86_SMALL_SPACES)
    asm volatile ("mov %0, %%es" : : "r" (X86_UDS));
#endif
}

#endif /* __GLUE_V4_X86__X32__TCB_H__ */
