/*********************************************************************
 *                
 * Copyright (C) 2002-2007,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/x64/exception.cc
 * Description:   exception handling
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
 * $Id: exception.cc,v 1.14 2006/10/21 00:46:39 reichelt Exp $ 
 *                
 ********************************************************************/

#include <debug.h>
#include <linear_ptab.h>
#include <kdb/tracepoints.h>
#include INC_ARCH(traps.h)
#include INC_ARCH(trapgate.h)
#include INC_GLUE(traphandler.h)
#include INC_API(tcb.h)
#include INC_API(space.h)
#include INC_API(kernelinterface.h)


const word_t x86_exc_reg_mr2reg[NUM_EXC_REGS][2] = 
{    
    {    19, (word_t) ~0UL			/* EXC	*/},
    {     1, X86_EXC_IPREG	/* RIP	*/},
    {     2, X86_EXC_RBXREG		/* RBX	*/},
    {     3, X86_EXC_R10REG	/* R10	*/},
    {     4, X86_EXC_R12REG	/* R12	*/},
    {     5, X86_EXC_R13REG	/* R13	*/},
    {     6, X86_EXC_R14REG	/* R14	*/},
    {     7, X86_EXC_R15REG	/* R15	*/},
    {     8, X86_EXC_RAXREG  	/* RAX	*/},
    {     9, X86_EXC_RCXREG  	/* RCX	*/},
    {    10, X86_EXC_RDXREG  	/* RDX	*/},
    {    11, X86_EXC_RSIREG  	/* RSI	*/},
    {    12, X86_EXC_RDIREG  	/* RDI	*/},
    {    13, X86_EXC_RBPREG  	/* RBP	*/},	
    {    14, X86_EXC_R8REG   	/* R8  */},
    {    15, X86_EXC_R9REG   	/* R9   */},
    {    16, X86_EXC_R11REG  	/* R11	*/},
    {    17, X86_EXC_SPREG  	/* RSP	*/},
    {    18, X86_EXC_FREG  	/* RFL	*/},
    {    20, X86_EXC_EREG  	/* ERR	*/},
};

#if defined(CONFIG_DEBUG)
const word_t x86_exceptionframe_dbgreg[X86_EXC_NUM_DBGREGS] = 
{
    X86_EXC_RAXREG,  X86_EXC_RBXREG,
    X86_EXC_RCXREG,  X86_EXC_RDXREG,
    X86_EXC_RSIREG,  X86_EXC_RDIREG,
    X86_EXC_RBPREG,  X86_EXC_FREG,
    X86_EXC_R8REG, X86_EXC_R9REG,
    X86_EXC_R10REG, X86_EXC_R11REG,
    X86_EXC_R12REG, X86_EXC_R13REG,
    X86_EXC_R14REG, X86_EXC_R15REG,
    X86_EXC_CSREG, X86_EXC_SSREG,
};

const char *x86_exceptionframe_name[X86_EXCEPTIONREGS_NUM_REGS] = 
{   "reason",    "r15",	 "r14",	 "r13",	 "r12",	 "r11",	 "r10",	 "r09",	 
    "r08",	 "rdi",	 "rsi",	 "rbp",	 "rdx",	 "rbx",	 "rcx",	 "rax",	 
    "err",	 "rip",	 "cs ",	 "rfl",	 "rsp",	 "ss "	 };    
#endif

void exc_catch_common_wrapper() 					
{							
    __asm__ (						
        ".section .data.x86.exc_common,\"aw\",@progbits		\n\t"
        ".global exc_catch_common				\n\t"
	"\t.type exc_catch_common,@function			\n\t"
	"exc_catch_common:					\n\t"
        "pushq %%rax						\n\t"
	"pushq %%rcx						\n\t"
	"pushq %%rbx						\n\t"
	"pushq %%rdx						\n\t"
	"pushq %%rbp						\n\t"
    	"pushq %%rsi						\n\t"
    	"pushq %%rdi						\n\t"
    	"pushq %%r8						\n\t"
    	"pushq %%r9						\n\t"
    	"pushq %%r10						\n\t"
    	"pushq %%r11						\n\t"
    	"pushq %%r12						\n\t" 
    	"pushq %%r13						\n\t"
    	"pushq %%r14						\n\t" 
    	"pushq %%r15						\n\t"
	"pushq %0			    			\n\t"
	"movq  %%rsp, %%rdi					\n\t"
	"call exc_catch_common_handler				\n\t"		
	"addq  $8, %%rsp					\n\t"		
    	"popq  %%r15						\n\t"		
    	"popq  %%r14						\n\t"		
    	"popq  %%r13						\n\t"		
    	"popq  %%r12						\n\t"		
    	"popq  %%r11						\n\t"		
    	"popq  %%r10						\n\t"		
    	"popq  %%r9						\n\t"		
    	"popq  %%r8						\n\t"		
    	"popq  %%rdi						\n\t"		
    	"popq  %%rsi						\n\t"		
	"popq  %%rbp						\n\t"		
	"popq  %%rdx						\n\t"		
	"popq  %%rbx						\n\t"		
	"popq  %%rcx						\n\t"		
        "popq  %%rax						\n\t"		
	"addq  $8, %%rsp					\n\t"		
	"iretq							\n\t"		
	".previous						\n\t"
	:						
	: "i"(0)					
	);						
}							
