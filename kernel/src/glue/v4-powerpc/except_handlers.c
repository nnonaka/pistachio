/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     glue/v4-powerpc/except_handlers.cc
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

#include <debug.h>
#include <kdb/tracepoints.h>

#include INC_ARCH(phys.h)
#include INC_ARCH(except.h)
#include INC_ARCH(msr.h)
#include INC_ARCH(frame.h)
#include INC_ARCH(ppc44x.h)

#include INC_API(tcb.h)
#include INC_API(schedule.h)
#include INC_API(interrupt.h)
#include INC_API(kernelinterface.h)

#include INC_GLUE(exception.h)
#include INC_GLUE(memcfg.h)


DECLARE_TRACEPOINT(PPC_EXCEPT_PROG);
DECLARE_TRACEPOINT(PPC_EXCEPT_DECR);
DECLARE_TRACEPOINT(PPC_EXCEPT_EXTINT);
DECLARE_TRACEPOINT(PPC_EXCEPT_FPU);

INLINE void halt_user_thread( void )
{
    tcb_t *current = get_current_tcb();

    tcb_set_state (current,  THREAD_STATE_HALTED );
    scheduler_schedule (get_current_scheduler(), get_idle_tcb(), sched_dest);
}

static bool send_exception_ipc( word_t exc_no, word_t exc_code )
{
    tcb_t *current = get_current_tcb();
    threadid_t handler = tcb_get_exception_handler (current);
    threadid_t local_id;
    word_t local_id_raw;

    if( threadid_is_nilthread (&handler) )
    {
	printf( "Unable to deliver user exception: no exception handler.\n" );
	return false;
    }

    // Setup exception IPC.
#define EXC_IPC_SAVED_REGISTERS (GENERIC_EXC_MR_MAX+1)
    word_t saved_mr[EXC_IPC_SAVED_REGISTERS];
    msg_tag_t tag;

    // Save message registers.
    for( int i = 0; i < EXC_IPC_SAVED_REGISTERS; i++ )
	saved_mr[i] = tcb_get_mr (current, i);
    tcb_set_saved_partner (current,  tcb_get_partner (current) );
    tcb_set_saved_state (current,  tcb_get_state (current) );

    // Create the message tag.
    msg_tag_set (&tag, 0, GENERIC_EXC_MR_MAX, (word_t)GENERIC_EXC_LABEL );
    tcb_set_tag (current,  tag );

    // Create the message.
    local_id = tcb_get_local_id (current);
    local_id_raw = threadid_get_raw (&local_id);
    tcb_set_mr (current,  GENERIC_EXC_MR_UIP,      (word_t)tcb_get_user_ip (current) );
    tcb_set_mr (current,  GENERIC_EXC_MR_USP,      (word_t)tcb_get_user_sp (current) );
    tcb_set_mr (current,  GENERIC_EXC_MR_UFLAGS,   (word_t)tcb_get_user_flags (current) );
    tcb_set_mr (current,  GENERIC_EXC_MR_NO,       exc_no );
    tcb_set_mr (current,  GENERIC_EXC_MR_CODE,     exc_code );
    tcb_set_mr (current,  GENERIC_EXC_MR_LOCAL_ID, local_id_raw );

    // Deliver the exception IPC.
    tag = tcb_do_ipc (current,  tcb_get_exception_handler (current),
	    tcb_get_exception_handler (current), timeout_never() );

    // Alter the user context if necessary.
    if( !msg_tag_is_error (&tag) )
    {
	tcb_set_user_ip (current,  (addr_t)tcb_get_mr (current, GENERIC_EXC_MR_UIP) );
	tcb_set_user_sp (current,  (addr_t)tcb_get_mr (current, GENERIC_EXC_MR_USP) );
	tcb_set_user_flags (current,  tcb_get_mr (current, GENERIC_EXC_MR_UFLAGS) );
    }
    else
	printf( "Unable to deliver user exception: IPC error.\n" );

    // Clean-up.
    for( int i = 0; i < EXC_IPC_SAVED_REGISTERS; i++ )
	tcb_set_mr (current,  i, saved_mr[i] );
    tcb_set_partner (current,  tcb_get_saved_partner (current) );
    tcb_set_saved_partner (current,  NILTHREAD );
    tcb_set_state (current,  tcb_get_saved_state (current) );
    tcb_set_saved_state (current,  THREAD_STATE_ABORTED );

    return !msg_tag_is_error (&tag);
}

// XXX switch to kdebug thread model
tcb_t *get_kdebug_tcb() { return (tcb_t*)~0; }

INLINE void try_to_debug( except_regs_t *regs, word_t exc_no, word_t dar, word_t dsisr )
{
    if( EXPECT_TRUE(get_kip()->kdebug_entry == NULL) )
	return;

    debug_param_t param;

    param.exception = exc_no;
    param.frame = regs;
    param.tcb = get_current_tcb();
    param.space = (get_current_space() ? get_current_space() : get_kernel_space());
    param.dar = dar;
    param.dsisr = dsisr;

    get_kip()->kdebug_entry( (void *)&param );
#ifdef CONFIG_KDB_BREAKIN
    kdebug_check_breakin();
#endif
}

static void dispatch_exception( except_regs_t *regs, word_t exc_no )
{
    if( get_kip()->kdebug_entry )
    {
	// If the debugger exists, let it have the first try at handling
	// the exception.
	word_t start_ip = regs->srr0_ip;
	word_t start_flags = regs->srr1_flags;

	try_to_debug( regs, exc_no, 0, 0 );

	if( (regs->srr0_ip != start_ip) || (regs->srr1_flags != start_flags) )
	    return;	// The kernel debugger handled the exception.
    }

    if( ppc_is_kernel_mode(regs->srr1_flags) )
	panic( "exception %x in kernel thread.\n", exc_no );

    // Try to send the exception to the user's exception handler.
    if( !send_exception_ipc(exc_no, regs->srr1_flags) )
    {
	enter_kdebug( "unhandled user exception, halting thread" );
	halt_user_thread();
    }
}

static bool emulate_instruction(word_t opcode, except_regs_t *regs)
{
#if defined(CONFIG_PPC_BOOKE)
    ppc_instr_t instr = PPC_INSTR(opcode);
    switch(ppc_instr_get_primary (instr))
    {
    case 31:
	switch(ppc_instr_get_secondary (instr))
	{
	case 259: // mfdcrx
	    except_regs_set_register (regs, ppc_instr_rt (instr), ppc_get_dcrx(except_regs_get_register (regs, ppc_instr_ra (instr))));
	    regs->srr0_ip += 4;
	    return true;

	case 323: // mfdcr
	    except_regs_set_register (regs, ppc_instr_rt (instr), ppc_get_dcrx(ppc_instr_rf (instr)));
	    regs->srr0_ip += 4;
	    return true;

	case 387: // mtdcrx
	    ppc_set_dcrx(except_regs_get_register (regs, ppc_instr_ra (instr)), except_regs_get_register (regs, ppc_instr_rt (instr)));
	    regs->srr0_ip += 4;
	    return true;

	case 451: // mtdcr
	    ppc_set_dcrx(ppc_instr_rf (instr), except_regs_get_register (regs, ppc_instr_rt (instr)));
	    regs->srr0_ip += 4;
	    return true;
	}
    }
#endif
    return false;
}


EXCDEF( unknown_handler )
{
    TRACEF("unknown handler\n");
    try_to_debug( frame, 0, 0, 0 );
    return_except();
}

EXCDEF( sys_reset_handler )
{
    try_to_debug( frame, EXCEPT_ID(SYSTEM_RESET), 0, 0 );
    return_except();
}

EXCDEF( machine_check_handler )
{
    panic("machine check\n");
    try_to_debug( frame, EXCEPT_ID(MACHINE_CHECK), 0, 0 );
    return_except();
}


EXCDEF( extern_int_handler )
{
    TRACEPOINT(PPC_EXCEPT_EXTINT,
	       "EXC ExtInt Handler: IP=%08x, MSR=%08x\n", srr0, srr1);

    if( EXPECT_FALSE(ppc_is_kernel_mode(srr1)) ) {
	srr1 = processor_wake( srr1 );
	frame->srr1_flags = srr1;
    }

    intctrl_handle_irq (get_current_cpu());

    return_except();
}

EXCDEF( alignment_handler )
{
    dispatch_exception( frame, EXCEPT_ID(ALIGNMENT) );
    return_except();
}

EXCDEF( program_handler )
{
    TRACEPOINT(PPC_EXCEPT_PROG,
	       "EXC Program Handler: IP=%p\n", srr0);

    space_t *space = get_current_space();
    if( EXPECT_FALSE(space == NULL) )
	space = get_kernel_space();

    word_t instr = space_get_from_user (space,  (addr_t)srr0 );
    if( instr == KIP_EXCEPT_INSTR ) {
	fpage_t kip_area = space_get_kip_page_area (space);

	frame->r3 = (word_t) fpage_get_base (&kip_area);
	/* Reaching the descriptor through kernel_desc_ptr is what
	   glue/v4-x86/exception.c does since get_kernel_descriptor went. */
	kernel_interface_page_t *kip = get_kip();
	kernel_descriptor_t *kdesc =
	    (kernel_descriptor_t *) ((addr_word_t) kip + kip->kernel_desc_ptr);

	frame->r4 = api_version_to_word (&kip->api_version);
	frame->r5 = api_flags_to_word (&kip->api_flags);
	frame->r6 = kernel_id_get_raw (&kdesc->kernel_id);
	frame->srr0_ip += 4;
	return_except();
    }

    if (! (is_privileged_space(space) && emulate_instruction(instr, frame)) )
	dispatch_exception( frame, EXCEPT_ID(PROGRAM) );

    return_except();
}

EXCDEF( fp_unavail_handler )
{
    tcb_t *current_tcb = get_current_tcb();
    TRACEPOINT(PPC_EXCEPT_FPU,   
	       "FPU unavail IP %p, MSR %08x (curr=%p, FPU owner=%p)\n", 
	       srr0, srr1, current_tcb, get_fp_lazy_tcb());

    tcb_resources_fpu_unavail_exception (&current_tcb->resources, current_tcb);

    return_except();
}

EXCDEF( decrementer_handler )
{
    /* Don't go back to sleep if the thread was in power savings mode.
     * We will only see timer interrupts from user mode, or from the sleep
     * function in kernel mode.
     */
    if( EXPECT_FALSE(ppc_is_kernel_mode(srr1)) ) {
	srr1 = processor_wake( srr1 );
	frame->srr1_flags = srr1;
    }

    TRACEPOINT(PPC_EXCEPT_DECR, 
	       "Decrementer Intr: IP=%p, MSR %08x", srr0, srr1);

#ifdef CONFIG_PPC_BOOKE
    // BookE uses auto-reload decrementer; just ack
    {
	ppc_tsr_t tsr = ppc_tsr_dec_irq ();
	ppc_tsr_write (&tsr);
    }
#else
    extern word_t decrementer_interval;
    ppc_set_dec( decrementer_interval );
#endif

    scheduler_handle_timer_interrupt (get_current_scheduler());
    return_except();
}

EXCDEF( trace_handler )
{
    dispatch_exception( frame, EXCEPT_ID(TRACE) );
    return_except();
}

EXCDEF( fp_assist_handler )
{
    dispatch_exception( frame, EXCEPT_ID(FP_ASSIST) );
    return_except();
}

#ifdef CONFIG_PPC_BOOKE
EXCDEF( debug_handler )
{
    UNIMPLEMENTED();
}

SECTION(".init") void install_exception_handlers( cpuid_t cpu )
{
    ppc_set_spr(SPR_IVOR(0), EXCEPT_OFFSET_CRITICAL_INPUT);
    ppc_set_spr(SPR_IVOR(1), EXCEPT_OFFSET_MACHINE_CHECK);
    ppc_set_spr(SPR_IVOR(2), EXCEPT_OFFSET_DSI);
    ppc_set_spr(SPR_IVOR(3), EXCEPT_OFFSET_ISI);
    ppc_set_spr(SPR_IVOR(4), EXCEPT_OFFSET_EXTERNAL_INT);
    ppc_set_spr(SPR_IVOR(5), EXCEPT_OFFSET_ALIGNMENT);
    ppc_set_spr(SPR_IVOR(6), EXCEPT_OFFSET_PROGRAM);
    ppc_set_spr(SPR_IVOR(7), EXCEPT_OFFSET_FP_UNAVAILABLE);
    ppc_set_spr(SPR_IVOR(8), EXCEPT_OFFSET_SYSCALL);
    ppc_set_spr(SPR_IVOR(9), EXCEPT_OFFSET_AUX_UNAVAILABLE);
    ppc_set_spr(SPR_IVOR(10), EXCEPT_OFFSET_DECREMENTER);
    ppc_set_spr(SPR_IVOR(11), EXCEPT_OFFSET_INTERVAL_TIMER);
    ppc_set_spr(SPR_IVOR(12), EXCEPT_OFFSET_WATCHDOG);
    ppc_set_spr(SPR_IVOR(13), EXCEPT_OFFSET_DTLB);
    ppc_set_spr(SPR_IVOR(14), EXCEPT_OFFSET_ITLB);
    ppc_set_spr(SPR_IVOR(15), EXCEPT_OFFSET_DEBUG);

    ppc_set_spr(SPR_IVPR, (word_t)memcfg_start_except() & 0xffff0000);
}
#endif
