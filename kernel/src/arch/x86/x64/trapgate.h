/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2006-2008,  Karlsruhe University
 *                
 * File path:     arch/x86/x64/trapgate.h
 * Description:   defines macros for implementation of trap and 
 *		  interrupt gates in C
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
 * $Id: trapgate.h,v 1.5 2006/10/19 22:57:34 ud3 Exp $
 *                
 ********************************************************************/
#ifndef __ARCH__X86__X64__TRAPGATE_H__
#define __ARCH__X86__X64__TRAPGATE_H__

 
#define X86_EXCEPTIONREGS_NUM_REGS 22

/* Exception-frame register indices into regs[], as macros so C (e.g.
   x64/exception.c) can name them; the C++ reg_e enum below aliases these.
   The ambiguous case-pairs (Dreg/dreg, Breg/breg) use register-name macros. */
#define X86_EXC_R15REG	1
#define X86_EXC_R14REG	2
#define X86_EXC_R13REG	3
#define X86_EXC_R12REG	4
#define X86_EXC_R11REG	5
#define X86_EXC_R10REG	6
#define X86_EXC_R9REG	7
#define X86_EXC_R8REG	8
#define X86_EXC_RDIREG	9	/* Dreg */
#define X86_EXC_RSIREG	10	/* Sreg */
#define X86_EXC_RBPREG	11	/* Breg */
#define X86_EXC_RDXREG	12	/* dreg */
#define X86_EXC_RBXREG	13	/* breg */
#define X86_EXC_RCXREG	14	/* creg */
#define X86_EXC_RAXREG	15	/* areg */
#define X86_EXC_EREG	16
#define X86_EXC_IPREG	17
#define X86_EXC_CSREG	18
#define X86_EXC_FREG	19
#define X86_EXC_SPREG	20
#define X86_EXC_SSREG	21

/*
 * Subarchitecture-neutral aliases.  arch/x86/trapgate.h and the shared glue
 * files index the frame by role rather than by register name, so that one body
 * of code serves both subarchitectures; x32/trapgate.h defines the same set
 * over its own layout.  The x64 spellings above stay for x64-only code.
 */
#define X86_EXC_DIREG	X86_EXC_RDIREG
#define X86_EXC_SIREG	X86_EXC_RSIREG
#define X86_EXC_BPREG	X86_EXC_RBPREG
#define X86_EXC_DREG	X86_EXC_RDXREG
#define X86_EXC_BREG	X86_EXC_RBXREG
#define X86_EXC_CREG	X86_EXC_RCXREG
#define X86_EXC_AREG	X86_EXC_RAXREG

#if defined(CONFIG_DEBUG)
#define X86_EXC_NUM_DBGREGS	18
#endif

struct x86_exceptionregs_t
{

    union
    {
	struct
	{    

	    u64_t reason;		/*  0 */
	    u64_t r15;			/*  1 */
	    u64_t r14;			/*  2 */
	    u64_t r13;			/*  3 */
	    u64_t r12;			/*  4 */
	    u64_t r11;			/*  5 */
	    u64_t r10;			/*  6 */        
	    u64_t r9;			/*  7 */
	    u64_t r8;			/*  8 */
	    u64_t rdi;			/*  9 */
	    u64_t rsi;			/* 10 */
	    u64_t rbp;			/* 11 */	
	    u64_t rdx;			/* 12 */
	    u64_t rbx;			/* 13 */
	    u64_t rcx;			/* 14 */
	    u64_t rax;			/* 15 */
	    /* default trapgate frame */
	    u64_t error;		/* 16 */
	    u64_t rip;			/* 17 */
	    u64_t cs;			/* 18 */
	    u64_t rflags;		/* 19 */
	    u64_t rsp;			/* 20 */
	    u64_t ss;			/* 21 */
	};
	word_t			regs[X86_EXCEPTIONREGS_NUM_REGS];
    };

};

typedef struct x86_exceptionregs_t x86_exceptionregs_t;




/**
 * X86_EXCWITH_ERRORCODE: allows C implementation of 
 *   exception handlers and trap/interrupt gates with error 
 *   code.
 *
 * Usage: X86_EXCWITH_ERRORCODE(exc_gp)
 */

#define X86_EXCWITH_ERRORCODE(name, reason)			\
EXTERN_C void name##handler(x86_exceptionframe_t *frame);	\
void name##_wrapper()						\
{								\
    __asm__ (							\
        ".global "#name "		\n"			\
	"\t.type "#name",@function	\n"			\
	#name":				\n"			\
        "pushq %%rax			\n"			\
	"pushq %%rcx			\n"			\
	"pushq %%rbx			\n"			\
	"pushq %%rdx			\n"			\
	"pushq %%rbp			\n"			\
    	"pushq %%rsi			\n"			\
    	"pushq %%rdi			\n"			\
    	"pushq %%r8			\n"			\
    	"pushq %%r9			\n"			\
    	"pushq %%r10			\n"			\
    	"pushq %%r11			\n"			\
    	"pushq %%r12			\n"			\
    	"pushq %%r13			\n"			\
    	"pushq %%r14			\n"			\
    	"pushq %%r15			\n"			\
	"pushq %0			\n"			\
	"movq  %%rsp, %%rdi		\n"			\
	"call "#name"handler		\n"			\
	"addq  $8, %%rsp		\n"			\
    	"popq  %%r15			\n"			\
    	"popq  %%r14			\n"			\
    	"popq  %%r13			\n"			\
    	"popq  %%r12			\n"			\
    	"popq  %%r11			\n"			\
    	"popq  %%r10			\n"			\
    	"popq  %%r9			\n"			\
    	"popq  %%r8			\n"			\
    	"popq  %%rdi			\n"			\
    	"popq  %%rsi			\n"			\
	"popq  %%rbp			\n"			\
	"popq  %%rdx			\n"			\
	"popq  %%rbx			\n"			\
	"popq  %%rcx			\n"			\
        "popq  %%rax			\n"			\
	"addq  $8, %%rsp		\n"			\
	"iretq				\n"			\
	:							\
	: "i"(reason)						\
	);							\
}								\
void name##handler(x86_exceptionframe_t *frame)


/* JS: TODO use RIP relative addressing !!!*/
#define X86_EXCNO_ERRORCODE(name, reason)		\
EXTERN_C void name (void);					\
EXTERN_C void name##handler(x86_exceptionframe_t *frame);	\
void name##_wrapper()						\
{								\
    __asm__ (							\
        ".global "#name "		\n"			\
	"\t.type "#name",@function	\n"			\
	#name":				\n"			\
	"subq  $8, %%rsp		\n"			\
        "pushq %%rax			\n"			\
	"pushq %%rcx			\n"			\
	"pushq %%rbx			\n"			\
	"pushq %%rdx			\n"			\
	"pushq %%rbp			\n"			\
    	"pushq %%rsi			\n"			\
    	"pushq %%rdi			\n"			\
    	"pushq %%r8			\n"			\
    	"pushq %%r9			\n"			\
    	"pushq %%r10			\n"			\
    	"pushq %%r11			\n"			\
    	"pushq %%r12			\n"			\
    	"pushq %%r13			\n"			\
    	"pushq %%r14			\n"			\
    	"pushq %%r15			\n"			\
	"pushq %0			\n"			\
	"movq  %%rsp, %%rdi		\n"			\
	"call "#name"handler		\n"			\
	"addq  $8, %%rsp		\n"			\
    	"popq  %%r15			\n"			\
    	"popq  %%r14			\n"			\
    	"popq  %%r13			\n"			\
    	"popq  %%r12			\n"			\
    	"popq  %%r11			\n"			\
    	"popq  %%r10			\n"			\
    	"popq  %%r9			\n"			\
    	"popq  %%r8			\n"			\
    	"popq  %%rdi			\n"			\
    	"popq  %%rsi			\n"			\
	"popq  %%rbp			\n"			\
	"popq  %%rdx			\n"			\
	"popq  %%rbx			\n"			\
	"popq  %%rcx			\n"			\
        "popq  %%rax			\n"			\
	"addq  $8, %%rsp		\n"			\
	"iretq				\n"			\
	:							\
	: "i"(reason)						\
	);							\
}								\
void name##handler(x86_exceptionframe_t *frame)


#endif /* !__ARCH__X86__X64__TRAPGATE_H__ */
