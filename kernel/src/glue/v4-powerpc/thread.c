/*********************************************************************
 *                
 * Copyright (C) 1999-2011,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     glue/v4-powerpc/thread.cc
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

#include INC_ARCH(msr.h)
#include INC_ARCH(frame.h)
#include INC_API(tcb.h)
#include INC_GLUE(tracepoints.h)

//#define TRACE_THREAD(x...)	TRACEF(x)
#define TRACE_THREAD(x...)

__attribute__ ((noreturn)) static void notify_trampoline()
{
    tcb_t *tcb;
    notify_frame_t *notify_frame;

    tcb = get_current_tcb();
    ASSERT(tcb);
    ASSERT(tcb == get_sprg_tcb());

    // Locate the notify frame.
    notify_frame = (notify_frame_t *)
	addr_offset( tcb->stack, sizeof(tswitch_frame_t) );

    // Call the notify function.
    notify_frame->func( notify_frame->arg1, notify_frame->arg2 );

    // Restore the stack to its pre-notification position.
    // The value of tcb->stack may be invalid if the notify callback function
    // caused a thread switch.
    tcb->stack = (word_t *) addr_offset( notify_frame, sizeof(notify_frame_t) );

    //  Resume this tcb's original thread of execution prior to the notify.
    //  This code is modeled after the tcb_t switch_to() functions.
    asm volatile (
	    "addi %%r1, %0, 16 ;"	// Install the new stack.
	    "lwz  %%r3, -16(%%r1) ;"	// Grab the old instruction pointer.
	    "lwz  %%r30, -4(%%r1) ;"	// Restore r30.
	    "lwz  %%r31, -8(%%r1) ;"	// Restore r31.
	    "mtctr %%r3 ;"		// Prepare to branch.
	    "bctr ;"			// Branch to the old instr pointer.
	    : /* outputs */
	    : /* inputs */
	      "b" (tcb->stack)
	    );

    enter_kdebug( "notify" );
    while( 1 );
}

void tcb_notify_word2 (tcb_t *self, void (*func)(word_t, word_t), word_t arg1, word_t arg2)
{
    /*  Create the stack frame seen by the notify trampoline.
     *  An old thread switch record will precede this info if the tcb
     *  is already a live thread.
     */
    notify_frame_t *notify_frame = (notify_frame_t *)
	addr_offset( self->stack,  -sizeof(notify_frame_t) );
    self->stack = (word_t *)notify_frame;

    tswitch_frame_t *tswitch_frame = (tswitch_frame_t *)
	addr_offset( self->stack, -sizeof(tswitch_frame_t) );
    self->stack = (word_t *)tswitch_frame;

    notify_frame->func = func;
    notify_frame->arg1 = arg1;
    notify_frame->arg2 = arg2;
    notify_frame->lr_save = 0;
    notify_frame->back_chain = 0;

    tswitch_frame->ip = (word_t)notify_trampoline;
    tswitch_frame->r30 = 0;
    tswitch_frame->r31 = 0;
}

__attribute__ ((noreturn)) static void enter_user_thread( tcb_t *tcb, 
	void (*func)() )
{
    /* Initialize the user thread. */
    func();

    /* make sure all the resources are initialized */
    if( resource_bits_have_resources (&tcb->resource_bits) )
	tcb_resources_load (&tcb->resources, tcb);

    syscall_regs_t *regs = get_user_syscall_regs(tcb);
    word_t user_ip = regs->srr0_ip;
    word_t user_sp = regs->r1_stack;
    threadid_t local_id = tcb_get_local_id (tcb);
    word_t user_utcb = threadid_get_raw (&local_id);
    word_t msr = regs->srr1_flags;

    TRACE_THREAD( "tcb %p ip %p, sp %p, utcb %p, msr %08x\n", 
	          tcb, user_ip, user_sp, user_utcb, msr );

    asm volatile (
	    "mr " MKSTR(ABI_LOCAL_ID) ", %3 ; "	// Install the local ID.
	    "mr %%r1, %2 ; "		// Stick the stack in r1.
	    "mtspr 27, %0 ; "		// Stick the MSR in srr1.
	    "mtspr 26, %1 ; "		// Stick the ip in srr0.
	    "rfi ; "
	    : /* no outputs */
	    : "r" (msr), "r" (user_ip), "b" (user_sp), "b" (user_utcb)
	    );

    ASSERT(0);
    while(1);
}

void tcb_create_startup_stack (tcb_t *self, void (*func)(void))
{
    /* Allocate the space for the user exception frame. */
    syscall_regs_t *regs = get_user_syscall_regs(self);
    self->stack = (word_t *)regs;

    /* Put sentinels in some of the user exception frame. */
    regs->r1_stack = 0x12345678;
    regs->srr0_ip  = 0x87654321;
    regs->lr = 0;

    /* Init the user flags so that the kernel doesn't mistake
     * this thread for an interrupted kernel thread.
     */
    regs->srr1_flags = MSR_USER;

    /* Init the user's local ID, so that an exchange registers
     * on an inactive thread will succeed. */
    ASSERT( self->utcb );
#if (ABI_LOCAL_ID != 2)
# error "Expected register R2 to store the user's local ID."
#endif
    {
	threadid_t local_id = tcb_get_local_id (self);
	regs->r2_local_id = threadid_get_raw (&local_id);
    }

#ifdef CONFIG_X_PPC_SOFTHVM
    if (tcb_get_space(self)->hvm_mode)
    {
	//TRACEF("initializing HVM mode for %x...\n", get_global_id().get_raw());
	regs->srr1_flags = MSR_SOFTHVM;
	arch_ktcb_init_hvm (&self->arch, self);
	self->pdir_cache = 0;
    }
#endif

    /* Create the thread switch context record for starting this
     * kernel thread. */
    tcb_notify_word2 (self, (void (*)(word_t,word_t))enter_user_thread,
		      (word_t)self, (word_t)func);
}


#if defined(CONFIG_X_CTRLXFER_MSG)

word_t arch_ktcb_get_powerpc_frameregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr)
{
    /* transfer from frame to dst */
    const word_t *hwreg = ctrlxfer_hwregs[id];
    except_regs_t *frame = get_user_except_regs(addr_to_tcb(self));

    word_t num = 0;
    
    for (word_t reg=lsb(mask); mask!=0; mask>>=lsb(mask)+1,reg+=lsb(mask)+1,num++)
    {
        TRACE_CTRLXFER_DETAILS( "\t (f%06d/%06d/%8s->m%06d): %08x", 
                                reg, hwreg[reg], ctrlxfer_get_hwregname(id, reg), 
                                dst_mr, ((word_t*)frame)[hwreg[reg]]);
        tcb_set_mr (dst, (*dst_mr)++, ((word_t*)frame)[hwreg[reg]]);
    }
    return num;
}

word_t arch_ktcb_set_powerpc_frameregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *src, word_t *src_mr)
{
    /* transfer from src to frame */
    const word_t *hwreg = ctrlxfer_hwregs[id];
    except_regs_t *frame = get_user_except_regs(addr_to_tcb(self));
    word_t num = 0;
    
    for (word_t reg=lsb(mask); mask!=0; mask>>=lsb(mask)+1,reg+=lsb(mask)+1,num++)
    {
        TRACE_CTRLXFER_DETAILS( "\t (m%06d->f%06d/%06d/%8s): %08x", 
                                src_mr, reg, hwreg[reg], ctrlxfer_get_hwregname(id, reg),
                                tcb_get_mr (src, *src_mr));
        
        ((word_t*)frame)[hwreg[reg]] = tcb_get_mr (src, (*src_mr)++);
    }
    return num;
}

word_t arch_ktcb_powerpc_ctrlxfer_fpu (arch_ktcb_t *self, tcb_t *dst)
{
    /* FPU has special handling--transfer always happens
     * from/to floating point regs */
    tcb_t *fp_tcb = get_fp_lazy_tcb();
    tcb_t *src = (tcb_t *) addr_to_tcb (self);
    
    /* XXX: this only works on the same CPU */
    if (tcb_get_cpu (src) == tcb_get_cpu (dst))
    {
        /* spill if neither source nor dest */
        if (fp_tcb != src && fp_tcb != dst && fp_tcb)
            tcb_resources_spill_fpu (&fp_tcb->resources, fp_tcb);
        
        /* if it wasn't the source then load into FPU */
        if (fp_tcb != src)
            tcb_resources_restore_fpu (&src->resources, src);
        
        /* swing ownership to dst */
        tcb_resources_reown_fpu (&src->resources, src, dst);
    }
    else
    {
        /* spill and memcpy to target tcb */
        UNIMPLEMENTED();
    }
    return  0;
}


word_t arch_ktcb_set_powerpc_fpuregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *src, word_t *src_mr)
{ return arch_ktcb_powerpc_ctrlxfer_fpu (&src->arch, addr_to_tcb(self)); }

word_t arch_ktcb_get_powerpc_fpuregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr)
{ return arch_ktcb_powerpc_ctrlxfer_fpu (self, dst); }


#ifdef CONFIG_X_PPC_SOFTHVM
word_t arch_ktcb_get_powerpc_vmregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr)
{
    /* transfer from frame to dst */
    const word_t * const hwreg = ctrlxfer_hwregs[id];
    word_t num = 0;
    
    for (word_t reg=lsb(mask); mask!=0; mask>>=lsb(mask)+1,reg+=lsb(mask)+1,num++)
    {
        TRACE_CTRLXFER_DETAILS( "\t (f%06d/%06d/%8s->m%06d): %08x", 
                                reg, hwreg[reg], ctrlxfer_get_hwregname(id, reg), 
                                dst_mr, ((word_t*)self->vm)[hwreg[reg]]);
        tcb_set_mr (dst, (*dst_mr)++, ((word_t*)self->vm)[hwreg[reg]]);
    }
    return num;

}

word_t arch_ktcb_set_powerpc_vmregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *src, word_t *src_mr)
{
    /* transfer from src to frame */
    const word_t * const hwreg = ctrlxfer_hwregs[id];
    word_t num = 0;
    
    for (word_t reg=lsb(mask); mask!=0; mask>>=lsb(mask)+1,reg+=lsb(mask)+1,num++)
    {
        TRACE_CTRLXFER_DETAILS( "\t (m%06d->f%06d/%06d/%8s): %08x", 
                                src_mr, reg, hwreg[reg], ctrlxfer_get_hwregname(id, reg),
                                tcb_get_mr (src, *src_mr));
        ((word_t*)self->vm)[hwreg[reg]] = tcb_get_mr (src, (*src_mr)++);
    }
    return num;

}

word_t arch_ktcb_get_powerpc_tlbregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr)
{
    /* transfer from frame to dst */
    word_t num = 0;

    for (word_t reg=lsb(mask); mask!=0; mask>>=lsb(mask)+1,reg+=lsb(mask)+1,num++)
    {
        word_t hwreg = (id - id_tlb0) * 4 + reg / 4;
        word_t val = ppc_hvm_tlb_ctrlxfer_get (&self->vm->tlb[hwreg], reg % 4);
        
        TRACE_CTRLXFER_DETAILS( "\t (f%06d/%06d/%8s->m%06d): %08x", 
                                reg, hwreg, ctrlxfer_get_hwregname(id, reg), 
                                dst_mr, val);
        
        tcb_set_mr (dst, (*dst_mr)++, val);
    }
    return num;

}

word_t arch_ktcb_set_powerpc_tlbregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *src, word_t *src_mr)
{
    /* transfer from src to frame */
    word_t num = 0;

    
    for (word_t reg=lsb(mask); mask!=0; mask>>=lsb(mask)+1,reg+=lsb(mask)+1,num++)
    {
        word_t hwreg = (id - id_tlb0) * 4 + reg / 4;
        ppc_hvm_tlb_ctrlxfer_set (&self->vm->tlb[hwreg], reg % 4, tcb_get_mr (src, (*src_mr)++));
        
        TRACE_CTRLXFER_DETAILS( "\t (m%06d->f%06d/%06d/%8s): %08x", 
                                src_mr, reg, hwreg, ctrlxfer_get_hwregname(id, reg),
                                tcb_get_mr (src, *src_mr));
        
    }
    return num;

}
#endif


get_ctrlxfer_regs_t get_ctrlxfer_regs[id_max] = 
{ 
  /* gpregs0 */	  &arch_ktcb_get_powerpc_frameregs,
  /* gpregs1 */	  &arch_ktcb_get_powerpc_frameregs,
  /* gpregsx */	  &arch_ktcb_get_powerpc_frameregs,
  /* fpuregs */   &arch_ktcb_get_powerpc_fpuregs,                 
#ifdef CONFIG_X_PPC_SOFTHVM             
  /* mmu */ 	  &arch_ktcb_get_powerpc_vmregs,
  /* except */	  &arch_ktcb_get_powerpc_vmregs,
  /* ivor */	  &arch_ktcb_get_powerpc_vmregs,
  /* timer */  	  &arch_ktcb_get_powerpc_vmregs,
  /* config */	  &arch_ktcb_get_powerpc_vmregs,
  /* debug */	  &arch_ktcb_get_powerpc_vmregs, 
  /* icache */    &arch_ktcb_get_powerpc_vmregs, 
  /* dcache */	  &arch_ktcb_get_powerpc_vmregs, 
  /* shadow_tlb */&arch_ktcb_get_powerpc_vmregs,            
  /* tlb0 */      &arch_ktcb_get_powerpc_tlbregs,	                
  /* tlb1 */      &arch_ktcb_get_powerpc_tlbregs,                
  /* tlb2 */      &arch_ktcb_get_powerpc_tlbregs,                
  /* tlb3 */      &arch_ktcb_get_powerpc_tlbregs,                
  /* tlb4 */	  &arch_ktcb_get_powerpc_tlbregs,              
  /* tlb5 */      &arch_ktcb_get_powerpc_tlbregs,                
  /* tlb6 */      &arch_ktcb_get_powerpc_tlbregs,                
  /* tlb7 */      &arch_ktcb_get_powerpc_tlbregs,                
  /* tlb8 */	  &arch_ktcb_get_powerpc_tlbregs,              
  /* tlb9 */      &arch_ktcb_get_powerpc_tlbregs,                
  /* tlb10 */     &arch_ktcb_get_powerpc_tlbregs,                
  /* tlb11 */     &arch_ktcb_get_powerpc_tlbregs,                
  /* tlb12 */	  &arch_ktcb_get_powerpc_tlbregs,              
  /* tlb13 */     &arch_ktcb_get_powerpc_tlbregs,                
  /* tlb14 */     &arch_ktcb_get_powerpc_tlbregs,                
  /* tlb15 */     &arch_ktcb_get_powerpc_tlbregs,                
#endif
};    

set_ctrlxfer_regs_t set_ctrlxfer_regs[id_max] = 
{ 
  /* gpregs0 */	  &arch_ktcb_set_powerpc_frameregs,
  /* gpregs1 */	  &arch_ktcb_set_powerpc_frameregs,
  /* gpregsx */	  &arch_ktcb_set_powerpc_frameregs,
  /* fpuregs */   &arch_ktcb_set_powerpc_fpuregs,                 
#ifdef CONFIG_X_PPC_SOFTHVM             
  /* mmu */ 	  &arch_ktcb_set_powerpc_vmregs,
  /* except */	  &arch_ktcb_set_powerpc_vmregs,
  /* ivor */	  &arch_ktcb_set_powerpc_vmregs,
  /* timer */  	  &arch_ktcb_set_powerpc_vmregs,
  /* config */	  &arch_ktcb_set_powerpc_vmregs,
  /* debug */	  &arch_ktcb_set_powerpc_vmregs, 
  /* icache */    &arch_ktcb_set_powerpc_vmregs, 
  /* dcache */	  &arch_ktcb_set_powerpc_vmregs, 
  /* shadow_tlb */&arch_ktcb_set_powerpc_vmregs,            
  /* tlb0 */      &arch_ktcb_set_powerpc_tlbregs,	                
  /* tlb1 */      &arch_ktcb_set_powerpc_tlbregs,                
  /* tlb2 */      &arch_ktcb_set_powerpc_tlbregs,                
  /* tlb3 */      &arch_ktcb_set_powerpc_tlbregs,                
  /* tlb4 */	  &arch_ktcb_set_powerpc_tlbregs,              
  /* tlb5 */      &arch_ktcb_set_powerpc_tlbregs,                
  /* tlb6 */      &arch_ktcb_set_powerpc_tlbregs,                
  /* tlb7 */      &arch_ktcb_set_powerpc_tlbregs,                
  /* tlb8 */	  &arch_ktcb_set_powerpc_tlbregs,              
  /* tlb9 */      &arch_ktcb_set_powerpc_tlbregs,                
  /* tlb10 */     &arch_ktcb_set_powerpc_tlbregs,                
  /* tlb11 */     &arch_ktcb_set_powerpc_tlbregs,                
  /* tlb12 */	  &arch_ktcb_set_powerpc_tlbregs,              
  /* tlb13 */     &arch_ktcb_set_powerpc_tlbregs,                
  /* tlb14 */     &arch_ktcb_set_powerpc_tlbregs,                
  /* tlb15 */     &arch_ktcb_set_powerpc_tlbregs,                
#endif
};    

#endif  /* defined(CONFIG_X_CTRLXFER_MSG) */


/**********************************************************************
 *
 *   tcb_t operations.  These are declared extern in api/v4/tcb.h, so they
 *   cannot be INLINE definitions in glue/v4-powerpc/tcb.h -- C rejects a
 *   static-inline definition of a name already declared extern.  They live
 *   here for the same reason, and in the same place, as on glue/v4-x86.
 *
 **********************************************************************/

/**********************************************************************
 *
 *                  copy-area related functions
 *
 **********************************************************************/

void tcb_adjust_for_copy_area (tcb_t *self, tcb_t * dst, addr_t * s, addr_t * d)
{
    tcb_resources_setup_copy_area (&self->resources, self, s, dst, d);
    tcb_resources_enable_copy_area (&self->resources, self);
}

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

addr_t tcb_copy_area_real_address (tcb_t *self, addr_t addr)
{
    return tcb_resources_copy_area_real_address (&self->resources, self, addr);
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
    ASSERT(count > 0);

    asm volatile (
	    "mtctr	%0 ;"	/* Initialize the count register. */
	    "1:"
	    "lwzu	%0, 4 (%1) ;"	/* Load from src utcb. */
	    "stwu	%0, 4 (%2) ;"	/* Store to dest utcb. */
	    "bdnz	1b ;"	/* Decrement ctr and branch if not zero. */
	    : /* outputs */
	      "+r" (count)
	    : /* inputs */
	      /* Handle pre-increment with -1 offset. */
	      "r" (&self->utcb->mr[start-1]), 
	      "r" (&dest->utcb->mr[start-1])
	    : /* clobbers */
	      "ctr"
	    );
}

/**
 * tcb_do_ipc: invokes an in-kernel IPC
 * @param to_tid destination thread id
 * @param from_tid from specifier
 * @param timeout IPC timeout
 * @return IPC message tag (MR0)
 */
msg_tag_t tcb_do_ipc (tcb_t *self, threadid_t to_tid, threadid_t from_tid, timeout_t timeout)
{
    tcb_resources_set_kernel_ipc (&self->resources, self);

    register word_t r3 asm("r3") = threadid_get_raw (&to_tid);
    register word_t r4 asm("r4") = threadid_get_raw (&from_tid);
    register word_t r5 asm("r5") = timeout.raw;

    /* ABI stack */
    asm volatile (
	    "addi %%r1, %%r1, -16 ;"	/* Allocate stack space for r30 and r31,
					   and the ABI stack space for calling
					   a function. */
	    "stw %%r30, 8(%%r1) ;"	/* Preserve r30. */
	    "stw %%r31, 12(%%r1) ;"	/* Preserve r31. */
	    "bl sys_ipc ;"		/* Call sys_ipc(). */
	    "lwz %%r31, 12(%%r1) ;"	/* Restore r31. */
	    "lwz %%r30, 8(%%r1) ;"	/* Restore r30. */
	    "addi %%r1, %%r1, 16 ;"	/* Clean-up the stack. */
	    : "+r" (r3), "+r" (r4), "+r" (r5)
	    : 
	    : "r0", "r2", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "r13", 
	      "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21", 
	      "r22", "r23", "r24", "r25", "r26", "r27", "r28", "r29", 
	      "ctr", "lr", "cr0", "cr1", "cr2", "cr3", "cr4", "cr5", 
	      "cr6", "cr7", "memory"
#if (__GNUC__ >= 3)
	      , "xer"
#endif
	    );

    tcb_resources_clr_kernel_ipc (&self->resources, self);

    msg_tag_t tag;
    tag.raw = tcb_get_mr (self, 0);
    return tag;
}

/**
 * tcb_get_br: returns value of buffer register
 * @index: number of buffer register
 */
word_t tcb_get_br (tcb_t *self, word_t index)
{
    return self->utcb->br[32-index];
}

/**
 * tcb_get_mr: returns value of message register
 * @index: number of message register
 */
word_t tcb_get_mr (tcb_t *self, word_t index)
{
    return self->utcb->mr[index];
}

word_t * tcb_get_stack_top (tcb_t *self)
{
    word_t stack;
    /* The powerpc eabi stack must be 8-byte aligned. */
    stack = ((word_t)self + TOTAL_TCB_SIZE) & ~(8-1);
    return (word_t *)stack;
}

word_t tcb_get_user_flags (tcb_t *self)
{
    return get_user_syscall_regs(self)->srr1_flags & MSR_USER_MASK;
}

/* 
 * access functions for ex-regs'able registers
 */
addr_t tcb_get_user_ip (tcb_t *self)
{
    return (addr_t) get_user_syscall_regs(self)->srr0_ip;
}

addr_t tcb_get_user_sp (tcb_t *self)
{
    return (addr_t) get_user_syscall_regs(self)->r1_stack;
}

word_t tcb_get_utcb_location (tcb_t *self)
{
    utcb_t *dummy = (utcb_t *)NULL;
    return threadid_get_raw (&self->myself_local) - (word_t)dummy->mr;
}

void tcb_init_stack (tcb_t *self)
{
    self->stack = tcb_get_stack_top(self);
    TRACE_TCB( "stack = %p, tcb bottom = %p, tcb size = %d\n", 
	       self->stack, self, sizeof(tcb_t) );
}

/**********************************************************************
 *
 *                        notification functions
 *
 **********************************************************************/
void tcb_notify (tcb_t *self, void (*func)(void))
{
    tcb_notify_word2 (self, (void (*)(word_t, word_t))func, 0, 0);
}

void tcb_notify_word (tcb_t *self, void (*func)(word_t), word_t arg1)
{
    tcb_notify_word2 (self, (void (*)(word_t, word_t))func, arg1, 0);
}

/**********************************************************************
 *
 *                        in-kernel IPC invocation
 *
 **********************************************************************/






void tcb_release_copy_area (tcb_t *self)
{
    tcb_resources_disable_copy_area (&self->resources, self);
}

void tcb_return_from_ipc (tcb_t *self)
{
    return_ipc_abort();
}

void tcb_return_from_user_interruption (tcb_t *self)
{
    word_t return_stack;
    extern word_t _except_return_shortcircuit[];

    // We want to short-circuit the return trip, to the exception
    // exit path.  So we jump to the point in assembler code which
    // starts restoring the user's full exception context.

    return_stack = (word_t) tcb_get_stack_top (self) - 
	(sizeof(except_regs_t) + EABI_STACK_SIZE);

    // Install the stack, and jump to the context store code.
    asm volatile (
	    "mtlr %0 ;"
	    "mr %%r1, %1 ;"
	    "blr ;"
	    :
	    : "r" ((word_t)_except_return_shortcircuit), "r" (return_stack)
	    : "r0"
	    );
}

/**
 * tcb_set_br: sets the value of a buffer register
 * @index: number of buffer register
 * @value: value to set
 */
void tcb_set_br (tcb_t *self, word_t index, word_t value)
{
    self->utcb->br[32-index] = value;
}

void tcb_set_cpu (tcb_t *self, cpuid_t cpu)
{
    self->cpu = cpu;
    self->utcb->processor_no = cpu;
#if defined(CONFIG_PPC_MMU_TLB)
    if (tcb_get_space(self) != get_kernel_space())
	self->pdir_cache = (word_t) space_get_asid_cpu (self->space, cpu);
#endif
}

/**
 * tcb_set_mr: sets the value of a message register
 * @index: number of message register
 * @value: value to set
 */
void tcb_set_mr (tcb_t *self, word_t index, word_t value)
{
    self->utcb->mr[index] = value;
}

void tcb_set_space (tcb_t *self, space_t * space)
{
    self->space = space;

    if (!space)
	return;

    if( EXPECT_FALSE(space == get_kernel_space()) )
    {
	/* Thread switch expects pdir_cache to be 0 for kernel threads.
	 */
	self->pdir_cache = 0;
	tcb_resources_set_kernel_thread (&self->resources, self);
	return;
    }

#ifdef CONFIG_PPC_MMU_SEGMENT
    self->pdir_cache = (word_t)space->get_segment_id().raw;
    TRACE_TCB("set_space(), space 0x%p, tcb 0x%p, kernel_space 0x%p\n", 
	      space, self, get_kernel_space() );

    space->sync_kernel_space( self );	/* Map self tcb into the space. */
    space->handle_hash_miss( self );	/* Install self tcb into the pg hash. */
    space->handle_hash_miss( space );	/* TODO: is self the solution? */
#endif
}

void tcb_set_user_flags (tcb_t *self, const word_t flags)
{
    get_user_syscall_regs(self)->srr1_flags = 
	(flags & MSR_USER_MASK) | MSR_USER;
}

void tcb_set_user_ip (tcb_t *self, addr_t ip)
{
    get_user_syscall_regs(self)->srr0_ip = (word_t) ip;
}

void tcb_set_user_sp (tcb_t *self, addr_t sp)
{
    get_user_syscall_regs(self)->r1_stack = (word_t) sp;
}

/********************************************************************** 
 *
 *                      tcb methods
 *
 **********************************************************************/

void tcb_set_utcb_location (tcb_t *self, word_t utcb_location)
{
    utcb_t *dummy = (utcb_t *)NULL;
    threadid_set_raw (&self->myself_local, utcb_location + (word_t)dummy->mr);
}

/**
 * tcb_switch_to: switches to specified tcb
 */
void tcb_switch_to (tcb_t *self, tcb_t * dest)
{
    ASSERT(dest->stack);
    ASSERT(get_current_cpu() == tcb_get_cpu (dest));
    ASSERT(dest != self);

    // TODO: adjust the thread switch return address to load 
    // resources.  Thus the common path need not check for a load.
    if( EXPECT_FALSE(resource_bits_have_resources (&self->resource_bits)) )
	tcb_resources_save (&self->resources, self);

#ifdef CONFIG_PPC_MMU_SEGMENTS
    /* NOTE: pdir_cache holds the segment ID. */
    if ( (dest->pdir_cache != current->pdir_cache) && (dest->pdir_cache != 0) )
    {
	word_t dummy;
	asm volatile (
#if defined(CONFIG_PPC_SEGMENT_LOOP)
	    /* Here is a loop to set the segment registers.  It is 4 cycles
	     * slower than the nonlooped version.  But it has 17 fewer
	     * instructions totalling 68 bytes, and thus saves 
	     * over 2 cache lines.
	     */
	    "li %%r3, 12 ;"	// Init the loop count.
	    "mtctr %%r3 ;"	// Load the loop count.
	    "li %%r3, 0 ;"	// Init the segment register index.
	    "99:" 
	    "mtsrin %0, %%r3 ;"	// Set the segment register.
	    "addi %0, %0, 1 ;"	// Increment the segment ID.
	    "extlwi %%r3, %0, 4, 28; "	// Extract the segment register index.
	    "bdnz 99b ;"	// Loop.
#else
	    "mtsr 0, %0 ; addi %0, %0, 1 ;"
	    "mtsr 1, %0 ; addi %0, %0, 1 ;"
	    "mtsr 2, %0 ; addi %0, %0, 1 ;"
	    "mtsr 3, %0 ; addi %0, %0, 1 ;"
	    "mtsr 4, %0 ; addi %0, %0, 1 ;"
	    "mtsr 5, %0 ; addi %0, %0, 1 ;"
	    "mtsr 6, %0 ; addi %0, %0, 1 ;"
	    "mtsr 7, %0 ; addi %0, %0, 1 ;"
	    "mtsr 8, %0 ; addi %0, %0, 1 ;"
	    "mtsr 9, %0 ; addi %0, %0, 1 ;"
	    "mtsr 10, %0 ; addi %0, %0, 1 ;"
	    "mtsr 11, %0 ;"
#endif
	    "isync ;"
	    : "=r"(dummy)
	    : "0"(dest->pdir_cache));
    }
#elif defined(CONFIG_PPC_MMU_TLB)
    if (dest->pdir_cache)
    {
	asid_t *asid = (asid_t*)dest->pdir_cache;
	word_t current_pid = ppc_get_pid();
	if (asid_get (asid) != current_pid)
	{
	    word_t dest_pid;
	    // AS switch...
	    if ( EXPECT_FALSE(!asid_is_valid (asid)) )
		space_allocate_asid (tcb_get_space (dest));
	    dest_pid = asid_manager_reference (get_asid_manager(), asid);
	    ASSERT(dest_pid);
	    ppc_set_pid(dest_pid);
	}
    }
#endif

    register word_t dummy0 asm("r27");
    register word_t dummy1 asm("r28");
    register word_t dummy2 asm("r29");

    asm volatile (
	    "stw %%r30, -4(%%r1) ;"		/* Preserve r30 on the stack. */
	    "stw %%r31, -8(%%r1) ;"		/* Preserve r31 on the stack. */
	    "mtsprg " MKSTR(SPRG_CURRENT_TCB) ", %[dest_tcb] ;"	/* Save the tcb pointer in sprg1. */
	    "lis %[dest_tcb], 1f@ha ;"		/* Grab the return address. */
	    "la  %[dest_tcb], 1f@l(%[dest_tcb]) ;"	/* Put the return address in %3. */
	    "stwu %[dest_tcb], -16(%%r1) ;"	/* Store (with update) the return address on the
						   current stack. */
	    "stw %%r1, 0(%[this_sp]) ;"		/* Save the current stack in old_stack. */
	    "addi %%r1, %[dest_sp], 16 ;"	/* Install the new stack. */
	    "lwz %%r3, -16(%%r1) ;"		/* Grab the new thread's address. */
	    "lwz %%r30, -4(%%r1) ;"		/* Restore r30. */
	    "lwz %%r31, -8(%%r1) ;"		/* Restore r31. */
	    "mtctr %%r3 ;"			/* Prepare to jump. */
	    "bctr ;"				/* Jump to the new thread. */
	    "1:"				/* The return address. */
	    : "=b" (dummy0), "=b" (dummy1), "=b" (dummy2)
	    : [dest_sp] "0" (dest->stack),
	      [this_sp] "1" (&self->stack),
	      [dest_tcb] "2" (dest)
	    : "memory", "r0", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", 
	      "r10", "r11", "r12", "r13", "r14", "r15", "r16", "r17", "r18",
	      "r19", "r20", "r21", "r22", "r23", "r24", "r25", "r26",
	      "ctr", "lr", 
	      "cr0", "cr1", "cr2", "cr3", "cr4", "cr5", "cr6", "cr7", "xer"
        );

    if( EXPECT_FALSE(resource_bits_have_resources (&self->resource_bits)) )
	tcb_resources_load (&self->resources, self);
}


/**********************************************************************
 *
 *   Remaining C entry points declared in api/v4/tcb.h.  As with the space
 *   predicates, the C++ class declared these and nothing defined them.
 *
 **********************************************************************/

/* tcb_set_saved_partner/_state are INLINE in api/v4/tcb.h, beside their getters. */

#if !defined(CONFIG_STATIC_TCBS)
/* Dynamic KTCBs: nothing to do.  The CONFIG_STATIC_TCBS form lives in
   api/v4/thread.c, next to the tcb_array it initialises. */
void tcb_init_tcbs (void)			{ /* Nothing to do (CONFIG_STATIC_TCBS off). */ }
#endif

/* Switch to the initial thread: install its stack and return into it.  The
   powerpc thread-switch record puts the resume address at the top of the
   stack, which is what tcb_switch_to's epilogue also relies on. */
void initial_switch_to_c (tcb_t *tcb)
{
    __asm__ __volatile__ (
	"mr	%%r1, %0 ;"		/* install the new stack */
	"lwz	%%r3, 0(%%r1) ;"	/* resume address */
	"mtctr	%%r3 ;"
	"bctr ;"
	:
	: "b" (tcb->stack)
	: "r3", "ctr");
    while (1);
}

#if !defined(CONFIG_SMP)
/* api/v4/thread.c defines this only under CONFIG_SMP. */
bool tcb_migrate_to_processor (tcb_t *self, cpuid_t processor)
{ (void) self; (void) processor; return false; }
#endif
