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
{ return powerpc_ctrlxfer_fpu(dst); }


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
        word_t val = self->vm->tlb[hwreg].ctrlxfer_get(reg % 4);
        
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
        self->vm->tlb[hwreg].ctrlxfer_set(reg % 4, tcb_get_mr (src, (*src_mr)++));
        
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
