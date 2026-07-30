/****************************************************************************
 *
 * Copyright (C) 2008,  Karlsruhe University
 * 
 * File path:	glue/v4-x86/thread.cc
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
 ***************************************************************************/
#include <debug.h>
#include INC_GLUE(config.h)
#include INC_API(thread.h)
#include INC_API(tcb.h)
#include INC_API(schedule.h)
#include INC_ARCH_SA(tss.h)

#if defined(CONFIG_X_CTRLXFER_MSG) || defined(CONFIG_X_X86_HVM)
#include INC_GLUE(ipc.h)

/*
 * The user exception frame lives at the top of the thread's kernel stack; see
 * the comment on the declaration in glue/v4-x86/ipc.h.  Not an INLINE there
 * because tcb_get_stack_top is declared after api/v4/tcb.h includes the glue
 * headers.  Gated because no other configuration has a caller.
 */
x86_exceptionframe_t *get_user_frame (tcb_t *tcb)
{
    return (x86_exceptionframe_t *) tcb_get_stack_top (tcb) - 1;
}
#endif

#if defined(CONFIG_X_CTRLXFER_MSG)
EXTERN_TRACEPOINT(IPC_CTRLXFER_ITEM_DETAILS);

word_t arch_ktcb_get_x86_gpregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr)
{
    /* transfer from frame to dst */
    const word_t *hwreg = ctrlxfer_hwregs[id];
    word_t *frame = (word_t *) get_user_frame (addr_to_tcb (self));
    word_t num = 0;
    
    for (word_t reg=(word_t)lsb(mask); mask!=0; mask>>=(word_t)lsb(mask)+1,reg+=(word_t)lsb(mask)+1,num++)
    {
        TRACE_CTRLXFER_DETAILS( "\t (f%06d/%06d/%8s->m%06d): %08x", 
                                reg, hwreg[reg], ctrlxfer_get_hwregname(id, reg), 
                                *dst_mr, frame[hwreg[reg]]);
        tcb_set_mr (dst, (*dst_mr)++, frame[hwreg[reg]]);
	
    }
    return num;
}

word_t arch_ktcb_set_x86_gpregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *src, word_t *src_mr)
{
    /* transfer from src to frame */
    const word_t *hwreg = ctrlxfer_hwregs[id];
    word_t *frame = (word_t *) get_user_frame (addr_to_tcb (self));
    word_t eflmask = X86_CTRLXFER_FLAGMASK;

    word_t num = 0;
    
    for (word_t reg=(word_t)lsb(mask); mask!=0; mask>>=(word_t)lsb(mask)+1,reg+=(word_t)lsb(mask)+1,num++)
    {
        TRACE_CTRLXFER_DETAILS( "\t (m%06d->f%06d/%06d/%8s): %08x", 
                                *src_mr, reg, hwreg[reg], ctrlxfer_get_hwregname(id, reg),
                                tcb_get_mr (src, *src_mr));
        
	if ((reg == gpreg_eflags))
	    frame[hwreg[reg]] = (frame[hwreg[reg]] & ~eflmask) | (tcb_get_mr (src, (*src_mr)++) & eflmask);
	else
	    frame[hwreg[reg]] = tcb_get_mr (src, (*src_mr)++);
    }
    return num;
}

/* Were UNIMPLEMENTED() one-liners in the class body. */
word_t arch_ktcb_get_x86_fpuregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr)
{ (void) self; (void) id; (void) mask; (void) dst; (void) dst_mr; UNIMPLEMENTED(); return 0; }

word_t arch_ktcb_set_x86_fpuregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *src, word_t *src_mr)
{ (void) self; (void) id; (void) mask; (void) src; (void) src_mr; UNIMPLEMENTED(); return 0; }


get_ctrlxfer_regs_t get_ctrlxfer_regs[id_max] = 
{ 
	&arch_ktcb_get_x86_gpregs,
 	&arch_ktcb_get_x86_fpuregs,
#if defined(CONFIG_X_X86_HVM)
 	&arch_ktcb_get_x86_hvm_cregs,
 	&arch_ktcb_get_x86_hvm_dregs,
 	&arch_ktcb_get_x86_hvm_segregs,
 	&arch_ktcb_get_x86_hvm_segregs,
 	&arch_ktcb_get_x86_hvm_segregs,
 	&arch_ktcb_get_x86_hvm_segregs,
 	&arch_ktcb_get_x86_hvm_segregs,
 	&arch_ktcb_get_x86_hvm_segregs,
 	&arch_ktcb_get_x86_hvm_segregs,
 	&arch_ktcb_get_x86_hvm_segregs,
 	&arch_ktcb_get_x86_hvm_segregs,
 	&arch_ktcb_get_x86_hvm_segregs,
	&arch_ktcb_get_x86_hvm_nonregexc,
	&arch_ktcb_get_x86_hvm_execctrl,
	&arch_ktcb_get_x86_hvm_otherregs,
#endif

};


set_ctrlxfer_regs_t set_ctrlxfer_regs[id_max] = 
{ 
	&arch_ktcb_set_x86_gpregs,
 	&arch_ktcb_set_x86_fpuregs,
#if defined(CONFIG_X_X86_HVM)
 	&arch_ktcb_set_x86_hvm_cregs,
 	&arch_ktcb_set_x86_hvm_dregs,
 	&arch_ktcb_set_x86_hvm_segregs,
 	&arch_ktcb_set_x86_hvm_segregs,
 	&arch_ktcb_set_x86_hvm_segregs,
 	&arch_ktcb_set_x86_hvm_segregs,
 	&arch_ktcb_set_x86_hvm_segregs,
 	&arch_ktcb_set_x86_hvm_segregs,
 	&arch_ktcb_set_x86_hvm_segregs,
 	&arch_ktcb_set_x86_hvm_segregs,
 	&arch_ktcb_set_x86_hvm_segregs,
 	&arch_ktcb_set_x86_hvm_segregs,
	&arch_ktcb_set_x86_hvm_nonregexc,
	&arch_ktcb_set_x86_hvm_execctrl,
	&arch_ktcb_set_x86_hvm_otherregs,
#endif

};

#endif /* defined(CONFIG_X_CTRLXFER_MSG) */
