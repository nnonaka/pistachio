/*********************************************************************
 *                
 * Copyright (C) 2002-2003, 2007-2010,  Karlsruhe University
 *                
 * File path:     arch/x86/x32/trapgate.h
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
 * $Id: trapgate.h,v 1.8 2003/09/24 19:04:27 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __ARCH__X86__X32__TRAPGATE_H__
#define __ARCH__X86__X32__TRAPGATE_H__

#define X86_EXCEPTIONREGS_NUM_REGS 17

/*
 * Exception-frame register indices into regs[], as macros so C can name them;
 * the C++ reg_e enum this replaces used the same values.  The names are the
 * subarchitecture-neutral ones arch/x86/trapgate.h and the shared glue code
 * index with -- x64/trapgate.h defines the same set over its own layout.
 */
#define X86_EXC_ESREG	1
#define X86_EXC_DSREG	2
#define X86_EXC_DIREG	3	/* Dreg */
#define X86_EXC_SIREG	4	/* Sreg */
#define X86_EXC_BPREG	5	/* Breg */
#define X86_EXC_BREG	7	/* breg -- ebx */
#define X86_EXC_DREG	8	/* dreg -- edx */
#define X86_EXC_CREG	9	/* creg -- ecx */
#define X86_EXC_AREG	10	/* areg -- eax */
#define X86_EXC_EREG	11
#define X86_EXC_IPREG	12
#define X86_EXC_CSREG	13
#define X86_EXC_FREG	14
#define X86_EXC_SPREG	15
#define X86_EXC_SSREG	16

#if defined(CONFIG_DEBUG)
#define X86_EXC_NUM_DBGREGS	12
#endif

struct x86_exceptionregs_t
{
    union
    {
	struct
	{    
	    u32_t reason;		/*  0 */
	    u32_t es;			/*  1 */
	    u32_t ds;			/*  2 */
	    u32_t edi;			/*  3 */
	    u32_t esi;			/*  4 */
	    u32_t ebp;			/*  5 */
	    u32_t __esp;		        
	    u32_t ebx;			/*  7 */
	    u32_t edx;			/*  8 */
	    u32_t ecx;			/*  9 */
	    u32_t eax;			/* 10 */
	    /* default trapgate frame */	
	    u32_t error;		/* 11 */
	    u32_t eip;			/* 12 */
	    u32_t cs;			/* 13 */
	    u32_t eflags;		/* 14 */
	    u32_t esp;			/* 15 */
	    u32_t ss;			/* 16 */
	};
	word_t			regs[X86_EXCEPTIONREGS_NUM_REGS];
    };

};
typedef struct x86_exceptionregs_t x86_exceptionregs_t;

/*
 * If KEEP_LAST_BRANCHES is enabled we should clear the LBR flag prior
 * to handling the exception so as to avoid overwriting the last
 * branch records by regular kernel code.
 */
#if defined(CONFIG_X86_KEEP_LAST_BRANCHES)
#define clr_dbgctl_lbr()		\
	"mov	$0x1d9, %%ecx		\n"\
	"rdmsr				\n"\
	"andl	$0xfffffffe, %%eax	\n"\
	"wrmsr				\n"

#define set_dbgctl_lbr()		\
	"mov	$0x1d9, %%ecx		\n"\
	"rdmsr				\n"\
	"orl	$0x1, %%eax		\n"\
	"wrmsr				\n"
#else
#define clr_dbgctl_lbr()
#define set_dbgctl_lbr()
#endif

#if defined(CONFIG_X86_SMALL_SPACES)
#define set_kds(reg)						\
	"mov $" MKSTR(X86_KDS) ", %%" #reg "	\n"		\
	"mov %%" #reg ", %%ds	    		\n"
#else
#define set_kds(reg)
#endif


#if defined(X86_EXC_KDB)

/* js: in rare cases, we might get a NMI (or breakpoint exception) just
 * after sys_ipc has been invoked, but _before_ esp has been set to tss.esp0
 * in that case, we need to switch stacks manually and preserve the
 * processor-saved frame. We make sure that, before tss there is enough scratch
 * space to cover EFLAGS, CS, and EIP saved by the processor; this way, we can
 * get along without having to establish a interrupt task xgate for KDB
 */
#define kdb_check_stack()					\
	"push	%%ebp			\n"			\
	"lea	(tss - 12), %%ebp	\n"			\
	"cmpl	%%esp, %%ebp		\n"			\
	"pop	%%ebp			\n"			\
	"jne	1f			\n"			\
	"addl	$12, %%esp		\n"			\
	"movl	(%%esp), %%esp	\n"			\
	"subl	$12, %%esp		\n"			\
	"push	%%ebp			\n"			\
	"mov	(tss    ), %%ebp	\n"			\
	"mov	%%ebp, 12(%%esp)	\n"			\
	"mov	(tss - 4), %%ebp	\n"			\
	"mov	%%ebp,  8(%%esp)	\n"			\
	"mov	(tss -  8), %%ebp	\n"			\
	"add    $3, %%ebp		\n"			\
	"mov	%%ebp,   4(%%esp)	\n"			\
	"pop	%%ebp			\n"			\
	"1:				\n"			
#else
#define kdb_check_stack()					
#endif


/**
 * X86_EXCWITH_ERRORCODE: allows C implementation of 
 *   exception handlers and trap/interrupt gates with error 
 *   code.
 *
 * Usage: X86_EXCWITH_ERRORCODE(exc_gp)
 */
#define X86_EXCWITH_ERRORCODE(name, reason)			\
EXTERN_C void name (void);					\
static void name##handler(x86_exceptionframe_t * frame);	\
void name##_wrapper()						\
{								\
    u32_t *handler = (u32_t*)name##handler;                     \
    __asm__ (							\
        ".global "#name "		\n"			\
	"\t.type "#name",@function	\n"			\
	#name":				\n"			\
	"pusha				\n"			\
	"push	%%ds			\n"			\
	"push	%%es			\n"			\
	"push	%1			\n"			\
	"push	%%esp			\n"			\
	set_kds (eax)						\
	clr_dbgctl_lbr ()					\
	"call	%0			\n"			\
	set_dbgctl_lbr ()					\
	"addl	$8, %%esp		\n"			\
	"popl	%%es			\n"			\
	"popl	%%ds			\n"			\
	"popa				\n"			\
	"addl	$4, %%esp		\n"			\
	"iret				\n"			\
	:							\
	: "m"(*handler), "i"(reason)                            \
	);							\
}								\
static void name##handler(x86_exceptionframe_t * frame)



#define X86_EXCNO_ERRORCODE(name, reason)			\
EXTERN_C void name (void);					\
static void name##handler(x86_exceptionframe_t * frame);	\
void name##_wrapper()						\
{								\
    u32_t *handler = (u32_t*)name##handler;                     \
    __asm__ (							\
        ".global "#name "		\n"			\
	"\t.type "#name",@function	\n"			\
	#name":				\n"			\
	kdb_check_stack()					\
	"subl	$4, %%esp		\n"			\
	"pusha				\n"			\
	"push	%%ds			\n"			\
	"push	%%es			\n"			\
	"push	%1			\n"			\
	"push	%%esp			\n"			\
	set_kds (eax)						\
	clr_dbgctl_lbr ()					\
	"call	%0			\n"			\
	set_dbgctl_lbr ()					\
	"addl	$8, %%esp		\n"			\
	"popl	%%es			\n"			\
	"popl	%%ds			\n"			\
	"popa				\n"			\
	"addl	$4, %%esp		\n"			\
	"iret				\n"			\
	:							\
	: "m"(*handler), "i"(reason)                            \
	);							\
}								\
static void name##handler(x86_exceptionframe_t * frame)



#endif /* !__ARCH__X86__X32__TRAPGATE_H__ */


