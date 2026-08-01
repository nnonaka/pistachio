/****************************************************************************
 *
 * Copyright (C) 2003,  National ICT Australia (NICTA)
 *
 * File path:	glue/v4-powerpc64/thread.c
 * Description:	Misc thread stuff.
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
 * $Id: thread.cc,v 1.4 2004/06/04 06:38:41 cvansch Exp $
 *
 ***************************************************************************/

#include INC_ARCH(msr.h)
#include INC_ARCH(frame.h)
#include INC_API(tcb.h)
//#include INC_GLUE(tracepoints.h)

//#define TRACE_THREAD(x...)	TRACEF(x)
#define TRACE_THREAD(x...)

EXTERN_C void powerpc64_initial_to_user( void );

/* Everything below the startup-stack helper was an INLINE member of tcb_t in
   glue/v4-powerpc64/tcb.h.  api/v4/tcb.h declares the tcb_* entry points
   out-of-line, so the bodies move here -- the same arrangement as
   glue/v4-powerpc/thread.c.  Notes §165. */

/**
 * read value of message register
 * @param index number of message register
 */
word_t tcb_get_mr (tcb_t *self, word_t index)
{
    return tcb_get_utcb (self)->mr[index];
}

/**
 * set the value of a message register
 * @param index number of message register
 * @param value value to set
 */
void tcb_set_mr (tcb_t *self, word_t index, word_t value)
{
    tcb_get_utcb (self)->mr[index] = value;
}

/**
 * copies a set of message registers from one UTCB to another
 * @param dest destination TCB
 * @param start MR start index
 * @param count number of MRs to be copied
 */
void tcb_copy_mrs (tcb_t *self, tcb_t * dest, word_t start, word_t count)
{
    ASSERT(start + count <= IPC_NUM_MR);

    asm volatile (
	"   mtctr	%0		\n"	/* Initialize the count register. */
	"   1:				\n"
	"   ldu		%0, 8(%1)	\n"	/* Load from src utcb. */
	"   stdu	%0, 8(%2)	\n"	/* Store to dest utcb. */
	"   bdnz	1b		\n"	/* Decrement ctr and branch if not zero. */
	: /* outputs */
	  "+r" (count)
	: /* inputs */
	  /* Handle pre-increment with -1 offset. */
	  "b" (&tcb_get_utcb (self)->mr[start-1]),
	  "b" (&tcb_get_utcb (dest)->mr[start-1])
	: /* clobbers */
	  "ctr"
    );
}

/**
 * read value of buffer register
 * @param index number of buffer register
 */
word_t tcb_get_br (tcb_t *self, word_t index)
{
    return tcb_get_utcb (self)->br[index];
}

/**
 * set the value of a buffer register
 * @param index number of buffer register
 * @param value value to set
 */
void tcb_set_br (tcb_t *self, word_t index, word_t value)
{
    tcb_get_utcb (self)->br[index] = value;
}


/**
 * set the address space a TCB belongs to
 * @param space address space the TCB will be associated with
 */
void tcb_set_space (tcb_t *self, space_t * space)
{
    self->space = space;
    // sometimes it might be desirable to use a pdir cache,
    // like in cases where it's not cheap to derive the page
    // directory from the space
    //self->pdir_cache = (word_t)space_get_pdir (space);
}


/**
 * set the cpu in a TCB
 * @param cpu	new cpu number
 */
void tcb_set_cpu (tcb_t *self, cpuid_t cpu)
{
    self->cpu = cpu;
    tcb_get_utcb (self)->processor_no = cpu;
}


void tcb_return_from_ipc (tcb_t *self)
{
    register word_t r14 asm("r14");
    extern char _restore_all_ipc[];
    powerpc64_irq_context_t * context = tcb_irq_context (self);

    r14 = tcb_get_tag (self).raw;

    do {
	asm volatile (
	    "mr	    %%r1, %0;"
	    "mtlr   %1;	    "
	    "blr;	    "
	    :: "r" (context), "r" (&_restore_all_ipc),
	       "r" (r14)
	);
    } while (1);
}

/**
 * Short circuit a return path from a user-level interruption or
 * exception.  That is, restore the complete exception context and
 * resume execution at user-level.
 */
void tcb_return_from_user_interruption (tcb_t *self)
{
    extern char _restore_all[];
    powerpc64_irq_context_t * context = tcb_irq_context (self);

    do {
	asm volatile (
	    "mr	    %%r1, %0;"
	    "mtlr   %1;	    "
	    "blr;	    "
	    :: "r" (context), "r" (&_restore_all)
	);
    } while (1);
}


/**
 * switches to another tcb thereby switching address spaces if needed
 * @param dest tcb to switch to
 */
void tcb_switch_to (tcb_t *self, tcb_t * dest)
{
    space_t *space = tcb_get_space (dest);
    space_t *currspace = tcb_get_space (self);

    if (space == NULL)
	space = get_kernel_space ();
    if (currspace == NULL)
	currspace = get_kernel_space ();


#if CONFIG_POWERPC64_SLB
    word_t asr = space_get_vsid_asid (space);
#elif CONFIG_POWERPC64_STAB
    word_t asr = ppc64_stab_get_asr (space_get_seg_table (space));
    asm volatile (
	"mfsprg	%%r7, %0;	"
	"std	%1, %2(%%r7);	"
	:: "i" (SPRG_LOCAL),
	   "r" (space_get_vsid_asid (space)),
	   "i" (LOCAL_VSID_ASID)
	: "r7"
    );
#endif

    asm volatile (
	"stdu	%%r1, -64(%%r1);	"	/* Create switch stack			*/
	"lis	%%r3, 1f@highest;	"	/* Load return address			*/
	"ori	%%r3, %%r3, 1f@higher;	"
	"rldicr	%%r3, %%r3, 32, 31;	"
	"oris	%%r3, %%r3, 1f@h;	"
	"ori	%%r3, %%r3, 1f@l;	"
	"std	%%r3, 16+64(%%r1);		"	/* Save the return address		*/

	"std	%%r1, 0(%[from_stack_save]);	    "	/* Save the stack pointer	*/
	"std	%%r2, 24(%%r1);		"	/* Save TOC in temp0			*/
	"std	%%r13, 32(%%r1);	"	/* Save Thread in temp1			*/
	"std	%%r30, 40(%%r1);	"	/* Save r30 in temp2			*/
	"std	%%r31, 48(%%r1);	"	/* Save r31 in temp3			*/

	"cmpld	cr0, %[from_space], %[dest_space];  "	/* Are the spaces the same?	*/
	"beq+	0f ;			"	/* Skip addr space switch if possible.	*/

#if CONFIG_PLAT_OFPOWER4 || CONFIG_CPU_POWERPC64_PPC970

	/* Optimized Power4 Context Switch
	 * Clear Only the Valid SLB Entries
	 * Preload New Entries
	 */
	"li	%%r13, 16;			"
	"slbmfee    %%r4, %%r13;		"
	"3:;					"
	"andis.	%%r5, %%r4, 0x0800;		"
	"beq-	7f;				"

	"slbmfev    %%r5, %%r13;		"

	"std	%%r4, 568(%[from_space]);	"
	"std	%%r5, 576(%[from_space]);	"

	"rldicl	%%r5, %%r4,36,28;		"
	"rldicr	%%r5, %%r5,28,35;		"
	"slbie	%%r5;				"

	"addi	%%r13, %%r13, 1;		"
	"cmpdi	%%r13, 40;			"
	"slbmfee	%%r4, %%r13;		"
	"addi	%[from_space], %[from_space], 16;	"
	"blt+	3b;				"
	"7:;					"
	"li	%%r4, 0;			"
	"std	%%r4, 568(%[from_space]);	"

	"mtasr	%[asr];			"	/* Setup new ASR			*/

	"li	%%r13, 16;			"
	"mr	%%r3, %[dest_space];		"
	"11:;					"
	"ld	%%r4, 568(%%r3);		"
	"ld	%%r5, 576(%%r3);		"
	"cmpdi	%%r4, 0;			"
	"beq-	10f;				"
	"or	%%r4, %%r4, %%r13;		"
	"slbmte	%%r5, %%r4;			"
	"addi	%%r3, %%r3, 16;			"
	"addi	%%r13, %%r13, 1;		"
	"b	11b;				"
	"10:;					"

	"isync;				"
#elif CONFIG_PLAT_OFPOWER3

	"isync;				"
	"mtasr %[asr];			"
	"slbia;				"	/* Flush the SLB			*/
	"isync;				"

#endif

	"0:				"

	"mtsprg	%[tcb_sprg], %[desttcb];	    "   /* TCB into Supervisor reg	*/
	"mr	%%r1, %[dest_stack];	"	/* Set the new stack			*/
	"ld	%%r2, 24(%%r1);		"	/* Load TOC in temp0			*/
	"ld	%%r13, 32(%%r1);	"	/* Load Thread in temp1			*/
	"ld	%%r30, 40(%%r1);	"	/* Load r30 in temp2			*/
	"ld	%%r31, 48(%%r1);	"	/* Load r31 in temp3			*/
	"addi	%%r1, %%r1, 64;		"	/* Unstack the frame			*/
	"ld	%%r3, 16(1);		"	/* Load the return address		*/
	"mtlr	%%r3;			"
	"blr;				"
	"1:				"
	::
	[desttcb] "r" (dest),
	[dest_stack] "r" (dest->stack),
	[from_stack_save] "b" (&self->stack),
	[asr] "r" (asr),
	[dest_space] "b" (space),
	[from_space] "r" (currspace),
	[tcb_sprg] "i" (SPRG_TCB)
	: /* Trashed */
	"r3", "r4", "r5", "lr",
	"cr0","cr1","cr2","cr3","cr4","cr5","cr6","cr7",
	"memory"
    );
    /* Trash the rest. This allows the compiler to choose which
     * inputs to use in the switch code
     */
    asm volatile (
	"" :::
	"r0", "r6", "r7", "r8", "r9", "r10",
	"r11", "r12", "r14", "r15", "r16", "r17", "r18",
	"r19", "r20", "r21", "r22", "r23", "r24", "r25",
	"r26", "r27", "r28", "r29",
	"ctr", "xer", "memory"
    );
}


word_t * tcb_get_stack_top (tcb_t *self)
{
    return tcb_stack_top (self);
}


/**
 * intialize stack for given thread
 */
void tcb_init_stack (tcb_t *self)
{
    /* Create space for an exception context */
    powerpc64_irq_context_t * context = tcb_irq_context (self);
    word_t * t;

    self->stack = (word_t *) context;	/* Update new stack position */

    /* Clear whole context */
    for (t = (word_t *) context; t < tcb_stack_top (self); t++)
	*t = 0;

    //TRACEF("[%p] stack = %p\n", self, self->stack);
}


/**********************************************************************
 *
 *                        notification functions
 *
 **********************************************************************/

/* Upstream had three overloads of tcb_t::notify, one per argument count;
   api/v4/tcb.h names them tcb_notify, tcb_notify_word and tcb_notify_word2.
   The bodies differ only in how many of temp1..temp3 they fill, and the frame
   is not cleared first, so the arity cannot be collapsed the way an
   all-zeroing constructor could. */

/**
 * create stack frame to invoke notify procedure
 * @param func notify procedure to invoke
 *
 * Create a stack frame in TCB so that next thread switch will invoke
 * the indicated notify procedure.
 */
void tcb_notify (tcb_t *self, void (*func)(void))
{
    powerpc64_switch_stack_t *frame = (powerpc64_switch_stack_t *)self->stack;
    register word_t toc asm("r2");

    frame->lr_save = *(word_t *) &powerpc64_do_notify;
    frame--;
    frame->back_chain = (word_t)frame;
    frame->temp0 = toc;
    frame->temp1 = *(word_t *)func;

    self->stack = (word_t *)frame;

    //TRACEF("%p (%p) , %016lx\n", self, frame->temp1, (word_t)self->stack);
}

/**
 * create stack frame to invoke notify procedure
 * @param func notify procedure to invoke
 * @param arg1 1st argument to notify procedure
 *
 * Create a stack frame in TCB so that next thread switch will invoke
 * the indicated notify procedure.
 */
void tcb_notify_word (tcb_t *self, void (*func)(word_t), word_t arg1)
{
    powerpc64_switch_stack_t *frame = (powerpc64_switch_stack_t *)self->stack;
    register word_t toc asm("r2");

    frame->lr_save = *(word_t *) &powerpc64_do_notify;
    frame--;
    frame->back_chain = (word_t)frame;
    frame->temp0 = toc;
    frame->temp1 = *(word_t *)func;
    frame->temp2 = arg1;

    self->stack = (word_t *)frame;

    //TRACEF("%p (%p)(0x%x), %016lx\n", self, frame->temp1, arg1, (word_t)self->stack);
}

/**
 * create stack frame to invoke notify procedure
 * @param func notify procedure to invoke
 * @param arg1 1st argument to notify procedure
 * @param arg2 2nd argument to notify procedure
 *
 * Create a stack frame in TCB so that next thread switch will invoke
 * the indicated notify procedure.
 */
void tcb_notify_word2 (tcb_t *self, void (*func)(word_t, word_t), word_t arg1, word_t arg2)
{
    powerpc64_switch_stack_t *frame = (powerpc64_switch_stack_t *)self->stack;
    register word_t toc asm("r2");

    frame->lr_save = *(word_t *) &powerpc64_do_notify;
    frame--;
    frame->back_chain = (word_t)frame;
    frame->temp0 = toc;
    frame->temp1 = *(word_t *)func;
    frame->temp2 = arg1;
    frame->temp3 = arg2;

    self->stack = (word_t *)frame;

    //TRACEF("%p (%p)(0x%x,0x%x), %016lx\n", self, frame->temp1, arg1, arg2, (word_t)self->stack);
}

/**********************************************************************
 * 
 *            access functions for ex-regs'able registers
 *
 **********************************************************************/

/**
 * read the user-level instruction pointer
 * @return	the user-level stack pointer
 */
addr_t tcb_get_user_ip (tcb_t *self)
{
    return (addr_t) tcb_irq_context (self)->srr0;
}

/**
 * read the user-level stack pointer
 * @return	the user-level stack pointer
 */
addr_t tcb_get_user_sp (tcb_t *self)
{
    return (addr_t) tcb_irq_context (self)->r1;
}


/**
 * set the user-level instruction pointer
 * @param ip	new user-level instruction pointer
 */
void tcb_set_user_ip (tcb_t *self, addr_t ip)
{
    tcb_irq_context (self)->srr0 = (word_t)ip;
}

/**
 * set the user-level stack pointer
 * @param sp	new user-level stack pointer
 */
void tcb_set_user_sp (tcb_t *self, addr_t sp)
{
    tcb_irq_context (self)->r1 = (word_t)sp;
}

word_t tcb_get_utcb_location (tcb_t *self)
{
    return threadid_get_raw (&self->myself_local);
}

void tcb_set_utcb_location (tcb_t *self, word_t utcb_location)
{
    powerpc64_irq_context_t * context = tcb_irq_context (self);

    //TRACEF( "(%p) utcb -> %p\n", self, utcb_location );

    threadid_set_raw (&self->myself_local, utcb_location);
    context->r13 = utcb_location;
}


/**
 * read the user-level flags (one word)
 * @return	the user-level flags
 */
word_t tcb_get_user_flags (tcb_t *self)
{
    return tcb_irq_context (self)->srr1;
}

/**
 * set the user-level flags
 * @param flags	new user-level flags
 */
void tcb_set_user_flags (tcb_t *self, const word_t flags)
{
    powerpc64_irq_context_t * context = tcb_irq_context (self);

    /* Make sure user can't promote themselves etc */
    word_t old_flags = context->srr1;
    context->srr1 = (flags & MSR_USER_MASK) | (~MSR_USER_MASK & old_flags);
}

/**********************************************************************
 *
 *                  copy-area related functions
 *
 **********************************************************************/

void tcb_adjust_for_copy_area (tcb_t *self, tcb_t * dst, addr_t * s, addr_t * d)
{
    UNIMPLEMENTED ();
}

void tcb_release_copy_area (tcb_t *self)
{
//    UNIMPLEMENTED (); XXX if should not be a problem as long as get_copy_area is UNIMPLENTED
}

addr_t tcb_copy_area_real_address (tcb_t *self, addr_t addr)
{
    UNIMPLEMENTED ();
    return addr;
}


/**
 * invoke an IPC from within the kernel
 *
 * @param to_tid destination thread id
 * @param from_tid from specifier
 * @param timeout IPC timeout
 * @return IPC message tag (MR0)
 */
msg_tag_t tcb_do_ipc (tcb_t *self, threadid_t to_tid, threadid_t from_tid,
		      timeout_t timeout)
{
    msg_tag_t tag;
    sys_ipc(to_tid, from_tid, timeout);
    tag.raw = tcb_get_mr (self, 0);

    return tag;
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
void tcb_arch_init_root_server (tcb_t *self, space_t * space, word_t ip, word_t sp)
{
}
