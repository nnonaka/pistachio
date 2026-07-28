/*********************************************************************
 *
 * Copyright (C) 2003-2004, 2006-2008, 2010,  Karlsruhe University
 *
 * File path:     api/v4/exregs.c
 * Description:   Iplementation of ExchangeRegisters()
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
 * $Id: exregs.cc,v 1.12 2006/12/05 16:33:37 skoglund Exp $
 *
 ********************************************************************/
#include INC_GLUE(syscalls.h)
#include INC_API(smp.h)
#include INC_API(schedule.h)

#include <kdb/tracepoints.h>

/* tcb.h defines TID(x) as the C++ (x).get_raw(); in C it lives only in dead
   (compiled-out) trace args, but redefine it to the C accessor for safety. */
#undef TID
#define TID(x)	threadid_get_raw (&(x))

DECLARE_TRACEPOINT (SYSCALL_EXCHANGE_REGISTERS);
#if defined(CONFIG_X_CTRLXFER_MSG)
EXTERN_TRACEPOINT(IPC_CTRLXFER_ITEM);
#endif

void handle_ipc_error (void);
void thread_return (void);
/* asm-named tcb_t method (C++ decl invisible to C); real C symbol tcb_unwind. */
void tcb_unwind (tcb_t *self, word_t reason);

static bool perform_exregs (tcb_t *src, tcb_t * dst, exregs_ctrl_t * control, word_t * usp,
			    word_t * uip, word_t * uflags, threadid_t * pager,
			    word_t * uhandle);

#if defined(CONFIG_SMP)

/**
 * Handler invoked to pass the return values of an ExchangeRegisters()
 * back to the invoker of the syscall.
 */
static void do_xcpu_exregs_reply (cpu_mb_entry_t * entry)
{
    tcb_t * tcb = entry->tcb;

    //TRACEF ("current=%t, tcb=%t\n", get_current_tcb (), tcb);

    if (EXPECT_FALSE (! tcb_is_local_cpu (tcb)))
    {
	// Forward request.
	xcpu_request7 (tcb_get_cpu (tcb), do_xcpu_exregs_reply, tcb,
		      entry->param[0], entry->param[1], entry->param[2],
		      entry->param[3], entry->param[4], entry->param[5], 0);
	return;
    }

    if (EXPECT_FALSE
	(tcb_get_state (tcb) != THREAD_STATE_XCPU_WAITING_EXREGS))
    {
	// Thread killed before exregs was completed.  Just ignore.
	return;
    }

    // Store exregs return values into TCB.
    threadid_t pager_tid;
    threadid_set_raw (&pager_tid, entry->param[4]);

    tcb->misc.exregs.control =	entry->param[0];
    tcb->misc.exregs.sp =	entry->param[1];
    tcb->misc.exregs.ip	=	entry->param[2];
    tcb->misc.exregs.flags =	entry->param[3];
    tcb->misc.exregs.pager =	pager_tid;
    tcb->misc.exregs.user_handle = entry->param[5];

    // Reactivate thread.
    tcb_set_state (tcb, THREAD_STATE_RUNNING);
    sched_schedule (tcb, sched_default);
}


/**
 * Handler invoked to perform ExchangeRegisers() on the remote CPU.
 */
static void do_xcpu_exregs (cpu_mb_entry_t * entry)
{
    tcb_t * dst = entry->tcb;
    tcb_t * from = (tcb_t *) entry->param[0];

    //TRACEF ("%t %t\n", get_current_tcb(), dst);

    if (EXPECT_FALSE (! tcb_is_local_cpu (dst)))
    {
	// Forward request.
	xcpu_request7 (tcb_get_cpu (dst), do_xcpu_exregs, dst, entry->param[0],
		      entry->param[1], entry->param[2], entry->param[3],
		      entry->param[4], entry->param[5], entry->param[6]);
	return;
    }

    threadid_t pager_tid;
    threadid_set_raw (&pager_tid, entry->param[5]);
    exregs_ctrl_t ctrl;
    ctrl.raw = entry->param[1];

    bool reschedule = perform_exregs (from, dst,
				      &ctrl,
				      &entry->param[2],
				      &entry->param[3],
				      &entry->param[4],
				      &pager_tid,
				      &entry->param[6]
				      );


    // Pass return values back to invoker thread.
    xcpu_request7 (tcb_get_cpu (from), do_xcpu_exregs_reply, from,
		  ctrl.raw, entry->param[2], entry->param[3],
		  entry->param[4], threadid_get_raw (&pager_tid), entry->param[6], 0);

    if (reschedule)
	sched_schedule_current ();
}


/**
 * Peform ExchangeRegisters() on a remote CPU.
 */
static void remote_exregs (tcb_t *current, tcb_t * dst, word_t * control,
			   word_t * usp, word_t * uip, word_t * uflags,
			   threadid_t * pager, word_t * uhandle)
{
    //TRACEF ("current=%t tcb=%t\n", current, dst);

    // Pass exregs request to remote CPU.
    xcpu_request7 (tcb_get_cpu (dst), do_xcpu_exregs, dst, (word_t) current,
		  *control, *usp, *uip, *uflags, threadid_get_raw (pager),
		  *uhandle);

    // Now wait for operation to complete.
    tcb_set_state (current, THREAD_STATE_XCPU_WAITING_EXREGS);
    sched_schedule (get_idle_tcb_c (), sched_handoff);

    // Grab exregs return values from tcb.
    *control =	current->misc.exregs.control;
    *usp = 	current->misc.exregs.sp;
    *uip = 	current->misc.exregs.ip;
    *uflags = 	current->misc.exregs.flags;
    *pager =	current->misc.exregs.pager;
    *uhandle =	current->misc.exregs.user_handle;

    // Reinitialize state
    tcb_init_saved_state (current);
}

#endif /* CONFIG_SMP */


/**
 * Do the actual ExhangeRegisters() syscall.  Separated into a
 * separate function so that it can be invoked on a remote CPU.
 *
 * @return if destination thread should be scheduled
 * @param tcb		destination tcb
 * @param control	control parameter (in/out)
 * @param usp		stack pointer (in/out)
 * @param uip		instrunction pointer (in/out)
 * @param uflags	flags (in/out)
 * @param pager		pager (in/out)
 * @param uhandler	user defined handle (in/out)
 */
static bool perform_exregs (tcb_t *src, tcb_t * dst, exregs_ctrl_t * control, word_t * usp,
			    word_t * uip, word_t * uflags, threadid_t * pager,
			    word_t * uhandle)
{
    //TRACEF("perform_exregs %x\n", dest);
    exregs_ctrl_t ctrl = *control;

    // Load return values before they are clobbered.
    word_t old_usp = (word_t) tcb_get_user_sp (dst);
    word_t old_uip = (word_t) tcb_get_user_ip (dst);
    word_t old_uhandle = tcb_get_user_handle (dst);
    word_t old_uflags = tcb_get_user_flags (dst);
    threadid_t old_pager = tcb_get_pager (dst);
    exregs_ctrl_t old_control;
    old_control.raw = 0;

    bool reschedule = false;

    UNUSED word_t src_idx = 1;

#if defined(CONFIG_X_CTRLXFER_MSG)
    word_t items = 0;
    msg_item_t src_item;
    acceptor_t acceptor;

    acceptor.raw = tcb_get_br (dst, 0);

    if (exregs_ctrl_is_set (&ctrl, EXREGS_CTRL_CTRLXFER_CONF_FLAG))
    {
	do
	{
	    src_item.raw = tcb_get_mr (src, src_idx++);

	    if (!msg_item_is_ctrlxfer_item (&src_item))
		break;

	    TRACEPOINT(IPC_CTRLXFER_ITEM, "ctrlxfer item: conf %t->%t fault=%d, id_mask=%x",
		       src, dst, msg_item_get_ctrlxfer_id (&src_item),
		       msg_item_get_ctrlxfer_mask (&src_item));

	    {
		ctrlxfer_mask_t mask;
		mask.maskvalue = msg_item_get_ctrlxfer_mask (&src_item);
		tcb_set_fault_ctrlxfer_items (dst, msg_item_get_ctrlxfer_id (&src_item), mask);
	    }

	} while (msg_item_more_ctrlxfer_items (&src_item));

    }
    if (exregs_ctrl_is_set (&ctrl, EXREGS_CTRL_CTRLXFER_READ_FLAG))
    {
	do
	{
	    src_item.raw = tcb_get_mr (src, src_idx);

	    if (!msg_item_is_ctrlxfer_item (&src_item) || !acceptor_accept_ctrlxfer (&acceptor))
		break;

    	    TRACEPOINT(IPC_CTRLXFER_ITEM,
		       "ctrlxfer item: read %t->%t id=%d, mask=%x (m->%c)",
		       src, dst,
		       msg_item_get_ctrlxfer_id (&src_item), msg_item_get_ctrlxfer_mask (&src_item),
		       acceptor_accept_ctrlxfer (&acceptor) ? 'f' : 'm');

	    if( (items = tcb_ctrlxfer (dst, src, src_item, 0, src_idx, false, true)) == 0)
		break;

	    src_idx += items;

	} while (msg_item_more_ctrlxfer_items (&src_item));

    }
    if (exregs_ctrl_is_set (&ctrl, EXREGS_CTRL_CTRLXFER_WRITE_FLAG))
    {
	do
	{
	    src_item.raw = tcb_get_mr (src, src_idx);

	    if (!msg_item_is_ctrlxfer_item (&src_item) || !acceptor_accept_ctrlxfer (&acceptor))
		break;

    	    TRACEPOINT(IPC_CTRLXFER_ITEM,
		       "ctrlxfer item: write %t->%t id=%d, mask=%x (m->%c)",
		       src, dst,
		       msg_item_get_ctrlxfer_id (&src_item), msg_item_get_ctrlxfer_mask (&src_item),
		       acceptor_accept_ctrlxfer (&acceptor) ? 'f' : 'm');

	    if( (items = tcb_ctrlxfer (src, dst, src_item, src_idx, 0, true, false)) == 0)
		break;

	    src_idx += items;

	} while (msg_item_more_ctrlxfer_items (&src_item));

    }

#endif

    if (exregs_ctrl_is_set (&ctrl, EXREGS_CTRL_SP_FLAG))
	tcb_set_user_sp (dst, (addr_t) *usp);

    if (exregs_ctrl_is_set (&ctrl, EXREGS_CTRL_IP_FLAG))
	tcb_set_user_ip (dst, (addr_t) *uip);

    if (exregs_ctrl_is_set (&ctrl, EXREGS_CTRL_FLAGS_FLAG))
	tcb_set_user_flags (dst, *uflags);

    if (exregs_ctrl_is_set (&ctrl, EXREGS_CTRL_PAGER_FLAG))
	tcb_set_pager (dst, *pager);

    if (exregs_ctrl_is_set (&ctrl, EXREGS_CTRL_UHANDLE_FLAG))
	tcb_set_user_handle (dst, *uhandle);


    // Check if thread was IPCing
    if (thread_state_is_sending (&dst->thread_state))
    {
	exregs_ctrl_set (&old_control, EXREGS_CTRL_SEND_FLAG);
	if (exregs_ctrl_is_set (&ctrl, EXREGS_CTRL_SEND_FLAG))
	{
	    tcb_unwind (dst, TCB_UNWIND_ABORT);
	    tcb_set_state (dst, THREAD_STATE_RUNNING);
	    tcb_notify (dst, handle_ipc_error);
	    sched_schedule (dst, sched_current);
	    reschedule = true;
	}
    }
    else if (thread_state_is_receiving (&dst->thread_state))
    {
	exregs_ctrl_set (&old_control, EXREGS_CTRL_RECV_FLAG);
	if (exregs_ctrl_is_set (&ctrl, EXREGS_CTRL_RECV_FLAG))
	{
	    tcb_unwind (dst, TCB_UNWIND_ABORT);
	    tcb_set_state (dst, THREAD_STATE_RUNNING);
	    tcb_notify (dst, handle_ipc_error);
	    sched_schedule (dst, sched_current);
	    reschedule = true;
	}
    }

    // Check if we should resume the thread.
    if (thread_state_is_halted (&dst->thread_state))
    {
	exregs_ctrl_set (&old_control, EXREGS_CTRL_HALT_FLAG);

	// If thread is halted - resume it.
	if ((exregs_ctrl_is_set (&ctrl, EXREGS_CTRL_HALTFLAG_FLAG)) && !(exregs_ctrl_is_set (&ctrl, EXREGS_CTRL_HALT_FLAG)))
	{
	    tcb_set_state (dst, THREAD_STATE_RUNNING);
	    sched_schedule (dst, sched_current);
	    reschedule = true;
	}
    }

    // Check if we should halt the thread.
    else if ((exregs_ctrl_is_set (&ctrl, EXREGS_CTRL_HALTFLAG_FLAG)) && (exregs_ctrl_is_set (&ctrl, EXREGS_CTRL_HALT_FLAG)))
    {
	if (thread_state_is_running (&dst->thread_state))
	{
	    // Halt a running thread
	    tcb_set_state (dst, THREAD_STATE_HALTED);
	    reschedule = true;
	}
	else
	{
	    printf("Halting a thread with ongoing kernel operations is not supported");
	    enter_kdebug("UNIMPLEMENTED");
	}
	// Need to halt the thread after any ongoing IPC or other
	// kernel operations are finished.  We can not let the thread
	// return to user level.
    }


    // Load up return values.
    *control =	old_control;
    *usp =	old_usp;
    *uip =	old_uip;
    *uflags =	old_uflags;
    *pager =	old_pager;
    *uhandle =	old_uhandle;

    return reschedule;

}



#if defined(CONFIG_X_PAGER_EXREGS)
FEATURESTRING ("pagerexregs");
#endif
#if defined(CONFIG_X_CTRLXFER_MSG)
FEATURESTRING ("ctrlxfer");
#endif

static inline bool has_exregs_perms(tcb_t * dst, threadid_t dst_tid)
{
    space_t * space = get_current_space_c ();
    // correct tid?
    if (!threadid_equals (&dst->myself_global, &dst_tid))
	return false;
    if (tcb_get_space (dst) == space)
	return true;

#if defined(CONFIG_X_PAGER_EXREGS)
    // all threads in pager address space can ex-regs
    threadid_t pager_tid = tcb_get_pager (dst); // make copy
    tcb_t * pager = tcb_get_tcb (pager_tid);

    if ( threadid_equals (&pager->myself_global, &pager_tid) &&
	 tcb_get_space (pager) == space )
	return true;
#endif
#if defined(CONFIG_X_CTRLXFER_MSG)
    if ( is_privileged_space(space))
	return true;
#endif
    return false;
}


SYS_EXCHANGE_REGISTERS (threadid_t dst_tid, word_t control,
			word_t usp, word_t uip, word_t uflags,
			word_t uhandle, threadid_t pager_tid,
			bool is_local)
{

    tcb_t *current = get_current_tcb();
    exregs_ctrl_t ctrl;
    ctrl.raw = control;

    TRACEPOINT (SYSCALL_EXCHANGE_REGISTERS,
		"SYS_EXCHANGE_REGISTERS: current %t, dst=%t [%s], control=0x%x [%s]"
		", usp=%p, uip=%p, uflags=%p, pager=%t, uhandle=%x\n",
		current, TID(dst_tid), is_local ? "local" : "global",
		ctrl.raw, exregs_ctrl_string (&ctrl), usp, uip, uflags, TID(pager_tid), uhandle);

    // Upon entry a local dst_tid will be converted into a global
    // thread ID before kernel entry.  If user somehow tricked kernel
    // entry with a local ID this will be handled in the test case
    // below.
    tcb_t * dst = tcb_get_tcb (dst_tid);

    // Only allow exregs on:
    //  - active threads
    //  - in the same address space
    //  - with a valid thread ID.

    if ((! tcb_is_activated (dst)) || (! has_exregs_perms(dst, dst_tid)) )
    {
	threadid_t nil = threadid_nilthread ();
	tcb_set_error_code (current, EINVALID_THREAD);
	return_exchange_registers (threadid_get_raw (&nil), 0, 0, 0, 0,
				   nil, 0);
    }

#if defined(CONFIG_SMP)
    if (! tcb_is_local_cpu (dst))
    {
	// Destination thread on remote CPU.  Must perform operation
	// remotely.
	remote_exregs (current, dst, &ctrl.raw, &usp, &uip, &uflags,
		       &pager_tid, &uhandle);
    }
    else
#endif
    {
	// Dstination thread on same CPU.  Perform operation immediately.
	if (perform_exregs (current, dst, &ctrl, &usp, &uip, &uflags,
			    &pager_tid, &uhandle))
	    sched_schedule_current ();

    }

    threadid_t ret_id = is_local ? tcb_get_global_id (dst) : tcb_get_local_id (dst);
    return_exchange_registers
	(threadid_get_raw (&ret_id),
	 ctrl.raw, usp, uip, uflags, pager_tid, uhandle);

}
