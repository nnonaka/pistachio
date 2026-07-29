/*********************************************************************
 *                
 * Copyright (C) 2002-2008, 2010,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/x64/tcb.h
 * Description:   TCB related functions for Version 4, AMD64
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
 * $Id: tcb.h,v 1.40 2007/02/21 07:09:57 stoess Exp $
 *                
 ********************************************************************/
#ifndef __GLUE_V4_X86__X64__TCB_H__
#define __GLUE_V4_X86__X64__TCB_H__

#include INC_ARCH_SA(tss.h)			/* for x86_x64_tss_t */
#include INC_API(fpage.h)
#include INC_API(thread.h)



/********************************************************************** 
 *
 *                      thread switch routines
 *
 **********************************************************************/

#ifndef BUILD_TCB_LAYOUT
#include <tcb_layout.h>	/* C-safe: pure offset #defines */

#endif /* !defined(BUILD_TCB_LAYOUT) */

/**********************************************************************
 *
 *                        global tcb functions
 *L4_Nilpage
 **********************************************************************/


INLINE tcb_t * get_current_tcb()
{
    addr_t stack;
    asm ("leaq -8(%%rsp), %0" :"=r" (stack));
    return (tcb_t *) ((word_t) stack & KTCB_MASK);
    
}


/**********************************************************************
 *
 *                  architecture-specific functions
 *
 **********************************************************************/

/**
 * initialize architecture-dependent root server properties based on
 * values passed via KIP
 * @param space the address space this server will run in   
 * @param ip the initial instruction pointer           
 * @param sp the initial stack pointer
 */

#endif /* !__GLUE_V4_X86__X64__TCB_H__ */
