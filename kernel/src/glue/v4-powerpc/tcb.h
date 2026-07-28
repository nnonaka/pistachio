/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     glue/v4-powerpc/tcb.h
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
 * $Id$
 *                
 ********************************************************************/
#pragma once

#include INC_ARCH(ppc_registers.h)
#include INC_ARCH(msr.h)
#include INC_API(syscalls.h)
#include INC_API(ipc.h)
#include INC_GLUE(resource_functions.h)

#define TRACE_TCB(x...)
//#define TRACE_TCB(x...)	TRACEF(x)

/********************************************************************** 
 *
 *                      processor state
 *
 **********************************************************************/

/* NOTE: The size must be a multiple of 8-bytes.
 * Important: the ordering here is rather important.  It matches hard coded
 * offsets in the inlined assembler thread switch code, offsets in the
 * threadswitch fast path, and offsets in the notify inlined assembler.
 */
typedef struct {
    word_t ip;
    word_t unused;
    word_t r31;
    word_t r30;
} tswitch_frame_t;

/* NOTE: The size must be a multiple of 8-bytes.
 * Important: ordering for back_chain and lr_save is important, as they
 * must sit on the stack in positions defined by the eabi.
 */
typedef struct {
    word_t back_chain;	// eabi prior stack location
    word_t lr_save;	// eabi return address
    word_t unused;
    word_t arg2;
    word_t arg1;
    void (*func)( word_t, word_t );
} notify_frame_t;


INLINE addr_t get_kthread_ip( tcb_t *tcb )
{
    tswitch_frame_t *tswitch_frame = (tswitch_frame_t *)tcb->stack;
    return (addr_t)tswitch_frame->ip;
}

/* tcb_get_stack_top is declared in api/v4/tcb.h *after* it includes this
   header, so its one-line body is spelled out here instead.  Keep the two in
   step -- the definition lives in glue/v4-powerpc/thread.c. */
INLINE word_t * tcb_stack_top (tcb_t *tcb)
{
    return (word_t *) (((word_t)tcb + TOTAL_TCB_SIZE) & ~(8-1));
}

INLINE syscall_regs_t *get_user_syscall_regs( tcb_t *tcb )
{
    return (syscall_regs_t *)
	((word_t) tcb_stack_top(tcb) - sizeof(syscall_regs_t));
}

INLINE except_regs_t *get_user_except_regs( tcb_t *tcb )
{
    return (except_regs_t *)
	((word_t) tcb_stack_top(tcb) - sizeof(except_regs_t));
}


#ifdef CONFIG_DYNAMIC_TCBS
INLINE void tcb_allocate (tcb_t *self)
{
    // Write to the tcb, to ensure that the kernel maps this tcb
    // with write access.  Write to the bottom of the stack.
    // TODO: should we do this?  It wastes a cache line.
    *(word_t *)( (word_t)self + sizeof(tcb_t) ) = 0;
}
#endif


/********************************************************************** 
 *
 *                      thread switch routines
 *
 **********************************************************************/


/**********************************************************************
 *
 *                        ctrlxfer tcb functions
 *
 **********************************************************************/
#if defined(CONFIG_X_CTRLXFER_MSG)
EXTERN_TRACEPOINT(IPC_CTRLXFER_ITEM_DETAILS);

INLINE void tcb_set_fault_ctrlxfer_items (tcb_t *self, word_t fault, ctrlxfer_mask_t mask)
{
    word_t idx = fault - 2;
    if (idx < IPC_CTRLXFER_STDFAULTS + ARCH_KTCB_FAULT_MAX)
	self->fault_ctrlxfer[idx] = mask;
}

INLINE ctrlxfer_mask_t tcb_get_fault_ctrlxfer_items (tcb_t *self, word_t fault)
{  
    word_t idx = fault - 2;
    return (idx < IPC_CTRLXFER_STDFAULTS + ARCH_KTCB_FAULT_MAX) ?
	self->fault_ctrlxfer[idx] : (ctrlxfer_mask_t) { .maskvalue = 0 };
}

INLINE word_t tcb_append_ctrlxfer_item (tcb_t *self, msg_tag_t tag, word_t offset)
{
    word_t fault = (0x1000 - (msg_tag_get_label (&tag) >> 4));
    if (tcb_get_fault_ctrlxfer_items(self, fault).maskvalue)
    {
	msg_item_t item;
	TRACE_CTRLXFER_DETAILS( "append ctrlxfer item %d", fault);
	/* tcb_flags_add and TCB_FLAG_* come later in api/v4/tcb.h. */
	self->flags.maskvalue |= (1UL << 2);   /* TCB_FLAG_KERNEL_CTRLXFER_MSG */
	item = ctrlxfer_kernel_fault_item (fault);
	/* tcb_set_mr is declared later in api/v4/tcb.h; reach the UTCB
	   directly, as the other accessors in this header do. */
	self->utcb->mr[offset++] = item.raw;
	return 1;
    }
    return 0;
}
#endif /* CONFIG_X_CTRLXFER_MSG */


/**********************************************************************
 *
 *                        global tcb functions
 *
 **********************************************************************/
INLINE void set_sprg_tcb( tcb_t *tcb )
{
    ppc_set_sprg( SPRG_CURRENT_TCB, (word_t)tcb );
}

__attribute__ ((const)) INLINE tcb_t *get_sprg_tcb()
{
    return (tcb_t *)ppc_get_sprg( SPRG_CURRENT_TCB );
}

__attribute__ ((const)) INLINE tcb_t * get_current_tcb()
{
    return addr_to_tcb( __builtin_frame_address(0) );
}

#if defined(CONFIG_SMP)
INLINE cpuid_t get_current_cpu()
{
    return get_idle_tcb()->get_cpu();
}
#endif

/**
 * initial_switch_to: switch to first thread
 * @param tcb TCB of initial thread.
 *
 * Switches to the initial thread.  The stack is expected to contain a
 * notify frame.
 * We use this function, rather than tcb_switch_to(), because the outgoing
 * stack isn't a valid tcb.
 */
INLINE void NORETURN initial_switch_to( tcb_t *tcb )
{
    // Store the target thread's tcb in the appropriate sprg.
    set_sprg_tcb( tcb );

    // Activate the thread switch frame.
    asm volatile (
	    "mtctr %0 ;"	// Prepare to branch.
	    "mr %%r1, %1 ;"	// Install the new stack.
	    "bctr ;"		// Branch to the instruction pointer.
	    : /* outputs */
	    : /* inputs */
	      "r" (get_kthread_ip(tcb)), "b" (tcb->stack)
	    );

    while( 1 );
}

/**********************************************************************
 *
 *                  architecture-specific functions
 *
 **********************************************************************/


