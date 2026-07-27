/*********************************************************************
 *
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, Jan Stoess, IBM Corporation
 *
 * File path:     api/v4/ipc.c
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

#define HANDLE_LOCAL_IDS

#include INC_API(tcb.h)
#include INC_API(schedule.h)
#include INC_API(ipc.h)
#include INC_API(interrupt.h)
#include INC_API(syscalls.h)
#include INC_API(smp.h)
#include INC_GLUE(syscalls.h)


DECLARE_TRACEPOINT(SYSCALL_IPC);
DECLARE_TRACEPOINT_DETAIL(IPC_TRANSFER);

DECLARE_TRACEPOINT_DETAIL(IPC_DETAILS);
DECLARE_TRACEPOINT_DETAIL(IPC_ERROR);
DECLARE_TRACEPOINT_DETAIL(IPC_XCPU_DETAILS);

INLINE bool transfer_message(tcb_t * src, tcb_t * dst, msg_tag_t tag)
{
    ASSERT(src);
    ASSERT(dst);
    TRACEPOINT (IPC_TRANSFER, "IPC transfer message: src=%t, dst=%t\n", src, dst);

    // clear all flags except propagation
    msg_tag_clear_receive_flags (&tag);

    /* VU: this copy loop is safe - untyped items can never
     * exceed the total number of message registers */
    if (msg_tag_get_untyped (&tag))
	tcb_copy_mrs (src, dst, 1, msg_tag_get_untyped (&tag));

    if (EXPECT_TRUE( !msg_tag_get_typed (&tag) ))
    {
	// If we have only untyped we know there will be no error.
	// Allow for some gcc optimizations here.
	tcb_set_tag (dst, tag);

	if (EXPECT_FALSE (msg_tag_is_propagated (&tag)))
	{
	    // If propagated message transfer was successful we can
	    // set the ActualSender field of the destination, and also
	    // redirect the partner of the virtual sender.  However,
	    // we must make sure that we revalidate the VirtualSender
	    // value since this is a variable that is fetched from the
	    // UTCB (and might therefore have been changed in the
	    // mean time).

	fixup_propagation:
	    {
	    tcb_t * virt_sender = tcb_get_tcb (tcb_get_virtual_sender (src));
	    threadid_t vsend = tcb_get_virtual_sender (src);
	    threadid_t vgid  = tcb_get_global_id (virt_sender);
	    threadid_t vpart = tcb_get_partner (virt_sender);
	    threadid_t sgid  = tcb_get_global_id (src);

	    if (threadid_equals (&vsend, &vgid)
		&& (tcb_get_space (src) == tcb_get_space (virt_sender) ||
		    tcb_get_space (src) == tcb_get_space (dst))
		&& thread_state_is_waiting (&virt_sender->thread_state)
		&& threadid_equals (&vpart, &sgid))
	    {
		TRACE_IPC_DETAILS("redirect virtual sender %t to partner %t \n",
				  virt_sender, dst);
		tcb_set_partner (virt_sender, tcb_get_global_id (dst));
	    }
	    tcb_set_actual_sender (dst, tcb_get_global_id (src));
	    }
	}

	return true;
    }
    else
    {
	tag = extended_transfer(src, dst, tag);
	tcb_set_tag (dst, tag);

	if (EXPECT_FALSE (msg_tag_is_propagated (&tag)))
	{
	    if (EXPECT_TRUE (! msg_tag_is_error (&tag)))
		goto fixup_propagation;
	    tcb_set_actual_sender (dst, tcb_get_global_id (src));
	}
	return (! msg_tag_is_error (&tag));
    }
}

#if defined(CONFIG_SMP)

/**********************************************************************
 *                          SMP handlers
 **********************************************************************/

// handler functions to kick remote transfers
static void do_xcpu_receive(cpu_mb_entry_t * entry)
{
    tcb_t * from_tcb = entry->tcb;
    tcb_t * to_tcb = (tcb_t*)entry->param[0];

    TRACE_XIPC_DETAILS("ipc xcpu %s from_tcb: %t (s=%s) to_tcb: %t (s=%s)",
	       __func__, from_tcb, thread_state_string (tcb_get_state (from_tcb)),
	       to_tcb, thread_state_string (tcb_get_state (to_tcb)));

    // did the sender migrate meanwhile?
    if (!tcb_is_local_cpu (from_tcb))
	UNIMPLEMENTED();

    threadid_t from_partner = tcb_get_partner (from_tcb);
    threadid_t to_gid = tcb_get_global_id (to_tcb);
    threadid_t to_partner = tcb_get_partner (to_tcb);
    threadid_t from_gid = tcb_get_global_id (from_tcb);

    // still waiting for the IPC?
    if ( (thread_state_is_polling (&from_tcb->thread_state) && threadid_equals (&from_partner, &to_gid) ) ||
	 (thread_state_is_locked_waiting (&to_tcb->thread_state) && threadid_equals (&to_partner, &from_gid)) )
    {
	// everything is fine -- now kick the thread
	tcb_set_state (from_tcb, THREAD_STATE_LOCKED_RUNNING);
	sched_schedule (from_tcb, sched_default);
    }
    else
	UNIMPLEMENTED();
}

// reply function
static void do_xcpu_send_reply(cpu_mb_entry_t * entry)
{
    // the send operation can start now
    tcb_t * from_tcb = (tcb_t*)entry->tcb;
    TRACE_XIPC_DETAILS("ipc xcpu %s from_tcb: %t (s=%s), result %x",
	       __func__, entry->tcb, thread_state_string (tcb_get_state (from_tcb)),
	       entry->param[0]);

    // we can let the thread run
    if (!tcb_is_local_cpu (from_tcb))
    {
	TRACE_XIPC_DETAILS("ipc xcpu %s from_tcb: %t (%s) migrated to cpu %d",
		   __func__, entry->tcb, thread_state_string (tcb_get_state (from_tcb)), tcb_get_cpu (from_tcb));

	// Forward request
	xcpu_request_c (tcb_get_cpu (from_tcb), do_xcpu_send_reply, from_tcb, 0);
	return;
    }

    // store status of XCPU request in TCB
    from_tcb->xcpu_status = entry->param[0];

    // and re-activate the thread
    tcb_set_state (from_tcb, THREAD_STATE_LOCKED_RUNNING);
    sched_schedule (from_tcb, sched_default);
}

static void do_xcpu_send(cpu_mb_entry_t * entry)
{
    tcb_t * to_tcb = entry->tcb;
    tcb_t * from_tcb = (tcb_t*)entry->param[0];
    threadid_t sender_id;
    threadid_set_raw (&sender_id, entry->param[1]);

    ASSERT(to_tcb);
    ASSERT(from_tcb);

    TRACE_XIPC_DETAILS("ipc xcpu %s to_tcb: %t (%s), from_tcb: %t (%s)",
	       __func__, to_tcb, thread_state_string (tcb_get_state (to_tcb)),
	       from_tcb, thread_state_string (tcb_get_state (from_tcb)));

    // did the receiver migrate meanwhile?
    if (!tcb_is_local_cpu (to_tcb))
    {
	TRACE_XIPC_DETAILS("ipc xcpu %s to_tcb: %t migrated to cpu %d",
		   __func__, to_tcb, tcb_get_cpu (to_tcb));
	xcpu_request_c (tcb_get_cpu (from_tcb), do_xcpu_send_reply, from_tcb, 1);
	return;
    }

    threadid_t to_partner = tcb_get_partner (to_tcb);

    if ( thread_state_is_waiting (&to_tcb->thread_state) &&
	 ( threadid_equals (&to_partner, &sender_id) ||
	   threadid_is_anythread (&to_partner) ))
    {
	// ok, still waiting --> everything is fine
	sched_ktcb_cancel_timeout (&to_tcb->sched_state);
	tcb_set_state (to_tcb, THREAD_STATE_LOCKED_WAITING);
	tcb_set_partner (to_tcb, sender_id);

	// now let the other thread run again
	xcpu_request_c (tcb_get_cpu (from_tcb), do_xcpu_send_reply, from_tcb, 0);
    }
    else if (thread_state_is_locked_waiting (&to_tcb->thread_state) &&
	     threadid_equals (&to_partner, &sender_id))
    {
	// ok, we are locked_waiting -- means we already issued
	// a request packet (do_xcpu_receive) -- so don't bother
	TRACE_XIPC_DETAILS("ipc xcpu %s %t is locked_waiting for %t",
		   __func__, to_tcb, TID(sender_id));
    }
    else
    {
	TRACE_XIPC_DETAILS("ipc xcpu %s (not waiting) to_tcb: %t (%s), from_tcb: %t",
		   __func__, to_tcb, thread_state_string (tcb_get_state (to_tcb)), from_tcb);
	xcpu_request_c (tcb_get_cpu (from_tcb), do_xcpu_send_reply, from_tcb, 1);
    }
}

static void do_xcpu_send_done(cpu_mb_entry_t * entry)
{
    tcb_t * to_tcb = entry->tcb;
    threadid_t sender_id;
    threadid_set_raw (&sender_id, entry->param[0]);

    TRACE_XIPC_DETAILS("ipc xcpu %s to_tcb: %t (%s) partner %t sender %t",
	       __func__, to_tcb, thread_state_string (tcb_get_state (to_tcb)),
	       TID(tcb_get_partner (to_tcb)), TID(sender_id));

    // did the receiver migrate meanwhile?
    if (!tcb_is_local_cpu (to_tcb))
    {
	TRACE_XIPC_DETAILS("ipc xcpu %s to_tcb: %t migrated to cpu %d",
		   __func__, to_tcb, tcb_get_cpu (to_tcb));
	// Forward request
	xcpu_request_c (tcb_get_cpu (to_tcb), do_xcpu_send_done, to_tcb, threadid_get_raw (&sender_id));
	return;
    }

    threadid_t to_partner = tcb_get_partner (to_tcb);

    if ( thread_state_is_locked_waiting (&to_tcb->thread_state) &&
	 threadid_equals (&to_partner, &sender_id) )
    {
	msg_tag_t tag = tcb_get_tag (to_tcb);
	msg_tag_set_xcpu (&tag);
	tcb_set_tag (to_tcb, tag);
	tcb_set_state (to_tcb, THREAD_STATE_RUNNING);
	sched_schedule (to_tcb, sched_default);
    }
    else
    {
	UNIMPLEMENTED();
    }
}

#endif /* CONFIG_SMP */


/**********************************************************************
 *
 *                          IPC syscall
 *
 **********************************************************************/
SYS_IPC (threadid_t to_tid, threadid_t from_tid, timeout_t timeout)
{
#ifdef SYS_IPC_PREAMBLE
    SYS_IPC_PREAMBLE
#endif
    tcb_t * to_tcb = NULL;
    tcb_t * from_tcb;
    tcb_t * current = get_current_tcb();
    msg_tag_t tag = tcb_get_tag (current);

    //ENABLE_TRACE_XIPC_DETAILS(~0, 0);
    TRACEPOINT (SYSCALL_IPC,
		"SYS_IPC: %t->%t (<-%t), to: %x, t: %x (l=0x%x, u=%d, t=%d)",
		current, TID(to_tid), TID(from_tid), timeout.raw,
		tcb_get_tag (current).raw, tcb_get_tag (current).x.label,
		tcb_get_tag (current).x.untyped, tcb_get_tag (current).x.typed);

    /* --- send phase --------------------------------------------------- */
#if defined(CONFIG_SMP)
send_path:
#endif

    if (! EXPECT_FALSE( threadid_is_nilthread (&to_tid) ))
    {
	to_tcb = tcb_get_tcb (to_tid);
	TRACE_IPC_DETAILS("ipc send phase curr=%t, to=%t", current, TID(to_tid));

	threadid_t to_gid = tcb_get_global_id (to_tcb);
	if (EXPECT_FALSE( !threadid_equals (&to_gid, &to_tid) ))
	{
	    /* specified thread id invalid */
	    TRACE_IPC_ERROR("ipc invalid send tid, wanted %t, but have %t", to_tid.raw, to_tcb);
	    tcb_set_error_code (current, IPC_SND_ERROR(ERR_IPC_NON_EXISTING));
	    tcb_set_tag (current, msg_tag_error_tag ());
	    return_ipc(NILTHREAD);
	}

	threadid_t sender_id = tcb_get_global_id (current);

	if (EXPECT_FALSE( msg_tag_is_propagated (&tag) ))
	{
	    tcb_t * virt_sender = tcb_get_tcb (tcb_get_virtual_sender (current));
	    threadid_t vsend = tcb_get_virtual_sender (current);
	    threadid_t vgid  = tcb_get_global_id (virt_sender);

	    // propagation only allowed within same address space
	    if ((threadid_equals (&vsend, &vgid)
		&& (tcb_get_space (current) == tcb_get_space (virt_sender) ||
		    tcb_get_space (current) == tcb_get_space (to_tcb))))
	    {
		sender_id = tcb_get_virtual_sender (current);
	    }
	    else
	    {
		msg_tag_set_propagated (&tag, false);
		tcb_set_tag (current, tag);
	    }
	}

#if defined(CONFIG_SMP)
	/* VU: set the thread state before actually checking
	 * the partner. Allows for concurrent checks on SMP */
	tcb_set_partner (current, to_tid);
	tcb_set_state (current, THREAD_STATE_POLLING);

        /* VU: add smp_memory_barrier() */
	if ( lock_state_is_active (&to_tcb->lock_state) )
	    tcb_lock (to_tcb);
#endif

	// not waiting || (not waiting for me && not waiting for any && not waiting for anylocal)
	// optimized for receive and wait any
	{
	threadid_t to_partner = tcb_get_partner (to_tcb);
	threadid_t curr_gid = tcb_get_global_id (current);
	if (EXPECT_FALSE(
                ((!thread_state_is_waiting (&to_tcb->thread_state))  ||
                 (   // Not waiting for sender (may be virtual sender)?
                     !threadid_equals (&to_partner, &sender_id) &&
                     // Not open wait?
                     !threadid_is_anythread (&to_partner) &&
                     // Not open local wait?
                     !(threadid_is_anylocalthread (&to_partner) &&
                       tcb_get_space (to_tcb) == tcb_get_space (current)) &&
                     // Not waiting for actual sender (if propagating IPC)?
                     !threadid_equals (&to_partner, &curr_gid)   ))
#if defined(CONFIG_SMP)
                && (!thread_state_is_locked_waiting (&to_tcb->thread_state) || (!threadid_equals (&to_partner, &curr_gid)))
#endif
                ))
	{
	    TRACE_IPC_DETAILS("ipc blocking send (curr=%t, to=%t s=%s)",
		       current, TID(to_tid), thread_state_string (tcb_get_state (to_tcb)));

	    /* thread is not receiving */
	    time_t snd_to = timeout_get_snd (&timeout);
	    if (EXPECT_FALSE( !time_is_never (&snd_to) ))
	    {
		if (time_is_zero (&snd_to))
		{
		    TRACE_IPC_ERROR("ipc zero send timeout (curr=%t, to=%t)", current, TID(to_tid));
		    /* VU: set thread state to running - in case we
		     * had a long IPC. Not on the critical path */
		    tcb_set_state (current, THREAD_STATE_RUNNING);
		    tcb_set_tag (current, msg_tag_error_tag ());
		    tcb_set_error_code (current, IPC_SND_ERROR(ERR_IPC_TIMEOUT));
		    tcb_unlock (to_tcb);
		    return_ipc(NILTHREAD);
		}
		{
		    time_t __snd = timeout_get_snd (&timeout);
		    TRACE_IPC_DETAILS("ipc setting timeout %dus current time %ld",
			   (word_t) time_get_microseconds (&__snd),
			   (word_t) sched_get_current_time ());
		}
		tcb_sched_set_timeout (current, timeout_get_snd (&timeout));

	    }
#if defined(CONFIG_SMP)
	    if (!lock_state_is_active (&to_tcb->lock_state))
		tcb_lock (to_tcb);
#endif

	    tcb_enqueue_send (current, to_tcb);
	    tcb_unlock (to_tcb);
	    tcb_set_partner (current, to_tid);
	    tcb_set_state (current, THREAD_STATE_POLLING);
	    sched_schedule (get_idle_tcb_c (), sched_ipcblk);

	    // got re-activated -- start IPC now
	    // make sure we dequeue ourselfs from the wakeup list
	    sched_ktcb_cancel_timeout (&current->sched_state);
	    tcb_lock (to_tcb);
	    tcb_dequeue_send (current, to_tcb);
	    // we re-acquire the lock and need to release for sure
	}
#if defined(CONFIG_SMP)
	else if (EXPECT_FALSE( !tcb_is_local_cpu (to_tcb) && !lock_state_is_enabled (&to_tcb->lock_state) ))
	{

	    TRACE_XIPC_DETAILS("ipc xcpu send %t:%d (%s) -> %t:%d (%s)",
		       current, tcb_get_cpu (current), thread_state_string (tcb_get_state (current)),
		       to_tcb, tcb_get_cpu (to_tcb), thread_state_string (tcb_get_state (to_tcb)));

	    // receiver seems to be waiting -- try to send
	    xcpu_request_many ( tcb_get_cpu (to_tcb), do_xcpu_send,
			  to_tcb, (word_t)current, threadid_get_raw (&sender_id), 0, 0);

	    // at this stage we are already polling...
	    sched_schedule (get_idle_tcb_c (), sched_ipcblk);

	    // re-activated?
	    TRACE_XIPC_DETAILS("ipc xcpu got reactivated after waiting to send %t:%d (%s) -> %t:%d (%s) result %d",
		       current, tcb_get_cpu (current), thread_state_string (tcb_get_state (current)),
		       to_tcb, tcb_get_cpu (to_tcb), thread_state_string (tcb_get_state (to_tcb)), current->xcpu_status);

	    // something happened -- retry sending
	    if (current->xcpu_status != 0)
	    {
#if 0
		// zero-timeout XCPU don't retry -- should be more generic
		// use absolute timeout on start of first round and check when
		// coming back
		if ( timeout_get_snd (&timeout).is_zero() )
		{
		    current->set_state(thread_state_t::running);
		    current->set_tag(msg_tag_t::error_tag());
		    current->set_error_code(IPC_SND_ERROR(ERR_IPC_TIMEOUT));
		    return_ipc(NILTHREAD);
		}
#endif
		TRACE_XIPC_DETAILS("ipc xcpu send failed, retry to send %t:%d (%s) -> %t:%d (%s)",
			   current, tcb_get_cpu (current), thread_state_string (tcb_get_state (current)),
			   to_tcb, tcb_get_cpu (to_tcb), thread_state_string (tcb_get_state (to_tcb)));


		goto send_path;
	    }
	    // ... fall through and perform send
	}
#endif

	// The partner must be told who the IPC originated from.
	tcb_set_partner (to_tcb, sender_id);

	if (EXPECT_FALSE( !transfer_message(current, to_tcb, tag) ))
	{
	    /* error on transfer - activate the partner and return */
	    tcb_set_tag (current, tcb_get_tag (to_tcb));
	    tcb_set_state (current, THREAD_STATE_RUNNING);
	    tcb_set_state (to_tcb, THREAD_STATE_RUNNING);
	    tcb_unlock (to_tcb);

	    if (EXPECT_TRUE( tcb_is_local_cpu (to_tcb) ))
		sched_schedule (to_tcb, sched_current);
#if defined(CONFIG_SMP)
	    else
		sched_remote_schedule (to_tcb);
#endif
	    return_ipc(to_tid);
	}

#if defined(CONFIG_SMP)
	if (EXPECT_FALSE( !tcb_is_local_cpu (to_tcb) ))
	{
	    if ( lock_state_is_enabled (&to_tcb->lock_state) )
	    {
		// lock-based remote IPC
		msg_tag_t xtag = tcb_get_tag (to_tcb);
		msg_tag_set_xcpu (&xtag);
		tcb_set_tag (to_tcb, xtag);

		threadid_t saved = tcb_get_saved_partner (to_tcb);
		if (threadid_is_nilthread (&saved))
		    tcb_set_state (to_tcb, THREAD_STATE_RUNNING);
		else
		    // Receiver had a nested IPC.
		    tcb_set_state (to_tcb, THREAD_STATE_LOCKED_RUNNING_IPC_DONE);
		sched_remote_schedule (to_tcb);
	    }
	    else
	    {
		// RPC-based remote IPC
		/* VU: kick receiver and forget about him
		 * we have to transmit the sender id since it is
		 * going to change in the receive path!!! */
		TRACE_XIPC_DETAILS("ipc xcpu notify on send done %t:%d (%s) -> %t:%d (%s)",
			   current, tcb_get_cpu (current), thread_state_string (tcb_get_state (current)),
			   to_tcb, tcb_get_cpu (to_tcb), thread_state_string (tcb_get_state (to_tcb)));

		//UNIMPLEMENTED();
		xcpu_request_c ( tcb_get_cpu (to_tcb), do_xcpu_send_done,
			      to_tcb, threadid_get_raw (&sender_id));
	    }
	    tcb_unlock (to_tcb);
	    // make sure we are running before potentially exiting to user
	    tcb_set_state (current, THREAD_STATE_RUNNING);
	    to_tcb = NULL;
	} else
	    tcb_unlock (to_tcb);
#endif
	}
    }

    /* --- send finished ------------------------------------------------ */
    TRACE_IPC_DETAILS("ipc send finished curr=%t to=%t from_tid %t", current, to_tcb, TID(from_tid));

    if (EXPECT_FALSE( threadid_is_nilthread (&from_tid) ))
    {
	/* this case is entered on:
	 *   - send-only case
	 *   - both descriptors set to nil id
	 * in the SMP case to_tcb is always NULL! */
	if (to_tcb != NULL)
	{
	    ASSERT(tcb_is_local_cpu (to_tcb));

	    threadid_t saved = tcb_get_saved_partner (to_tcb);
	    if (threadid_is_nilthread (&saved))
		tcb_set_state (to_tcb, THREAD_STATE_RUNNING);
	    else
		// Receiver had a nested IPC.
		tcb_set_state (to_tcb, THREAD_STATE_LOCKED_RUNNING_IPC_DONE);

	    tcb_set_state (current, THREAD_STATE_RUNNING);
	    sched_schedule (to_tcb, sched_sndonly);
	}

	return_ipc(from_tid);
    }
    /* --- receive phase ------------------------------------------------ */
    else /* ! from_tid.is_nilthread() */
    {
	TRACE_IPC_DETAILS("ipc receive phase curr=%t, from=%t", current, TID(from_tid));

#if defined(CONFIG_SMP)
        if (lock_state_is_active (&current->lock_state))
	    tcb_lock (current);
#endif

	/* VU: optimize for common case -- any, closed, anylocal */
	if (threadid_is_anythread (&from_tid))
	{
	    from_tcb = current->send_head;
	}
	else if (EXPECT_TRUE( !threadid_is_anylocalthread (&from_tid) ))
	{
	    /* closed wait */
	    ASSERT(threadid_is_global (&from_tid));
	    from_tcb = tcb_get_tcb (from_tid);


	    TRACE_IPC_DETAILS("ipc closed wait from %t, current=%t", TID(from_tid), current);

	    threadid_t from_gid = tcb_get_global_id (from_tcb);
	    threadid_t from_lid = tcb_get_local_id (from_tcb);
	    if (EXPECT_FALSE( !threadid_equals (&from_gid, &from_tid) &&
			      ( (tcb_get_space (from_tcb) != tcb_get_space (current)) ||
				(!threadid_equals (&from_lid, &from_tid)) ) ))
	    {
		/* wrong receiver id */
		TRACE_IPC_ERROR("ipc invalid receiver id (curr=%t, from=%t)", current, TID(from_tid));
		tcb_set_tag (current, msg_tag_error_tag ());
		tcb_set_error_code (current, IPC_RCV_ERROR(ERR_IPC_NON_EXISTING));
		ON_CONFIG_SMP(tcb_set_state (current, THREAD_STATE_RUNNING));
		tcb_unlock (current);
		return_ipc(NILTHREAD);
	    }
	}
	else
	{
	    /* anylocal */
	    tcb_t *head = current->send_head;
	    from_tcb = NULL;

#if defined(CONFIG_SMP)
	    TRACEF("from == anylocal requires SMP sanity check");
            UNTESTED();
#endif
	    if (head)
	    {
	        tcb_t *tcb = head;

	        do {
		    if (tcb_get_space (tcb) == tcb_get_space (current))
		    {
		        from_tcb = tcb;
			break;
		    }
		    tcb = tcb->send_list.next;
		} while (tcb != head);
	    }
	}

	/*
	 * no partner || partner is not polling ||
	 * partner doesn't poll on me
	 */
	threadid_t from_partner;
	threadid_t curr_gid = tcb_get_global_id (current);
	if (from_tcb) from_partner = tcb_get_partner (from_tcb);
	if( EXPECT_TRUE ( (from_tcb == NULL) ||
			  (!thread_state_is_polling (&from_tcb->thread_state)) ||
			  ( (!threadid_equals (&from_partner, &curr_gid)) &&
			    (!threadid_equals (&from_partner, &current->myself_local)) )) )
	{
	    TRACE_IPC_DETAILS("ipc blocking receive (curr=%t, from=%t)", current, TID(from_tid));

	    /* partner is not trying to send to me */
	    time_t rcv_to = timeout_get_rcv (&timeout);
	    if (EXPECT_FALSE( !time_is_never (&rcv_to) ))
	    {
		/* prepare the IPC error */
		tcb_set_error_code (current, IPC_RCV_ERROR(ERR_IPC_TIMEOUT));

		if ( time_is_zero (&rcv_to) )
		{
		    TRACE_IPC_ERROR("ipc receive error (curr=%t, from=%t)", current, TID(from_tid));
		    tcb_set_tag (current, msg_tag_error_tag ());
		    tcb_set_state (current, THREAD_STATE_RUNNING);
		    tcb_unlock (current);
		    /* zero timeout and partner not ready -->
		     * we have to perform timeslice donation */
		    if (to_tcb != NULL)
			sched_schedule (to_tcb, sched_rcverr);
		    return_ipc(NILTHREAD);
		}
		{
		    time_t __rcv = timeout_get_rcv (&timeout);
		    TRACE_IPC_DETAILS("ipc setting timeout %dus current time %ld",
			   (word_t) time_get_microseconds (&__rcv),
			   (word_t) sched_get_current_time ());
		}
		tcb_sched_set_timeout (current, timeout_get_rcv (&timeout));
		tcb_set_state (current, THREAD_STATE_WAITING_TIMEOUT);
	    }
	    else
		tcb_set_state (current, THREAD_STATE_WAITING_FOREVER);

	    /* VU: should we convert to a global id here??? */
	    tcb_set_partner (current, from_tid);

	    if (EXPECT_FALSE(to_tcb == NULL))
		to_tcb = get_idle_tcb_c ();
	    else
		tcb_set_state (to_tcb, THREAD_STATE_RUNNING);

#if defined(CONFIG_SMP)
	    ASSERT(tcb_is_local_cpu (to_tcb));
#endif
	    tcb_unlock (current);
	    sched_schedule (to_tcb, sched_ipcblk);

#if defined(HANDLE_LOCAL_IDS)
	    from_tcb = tcb_get_partner_tcb (current);
#endif

	    /* VU: if a timeout occurs the wakeup handling will set
	     * the error bit accordingly. Hence, we can directly
	     * return from the IPC without additional checking
	     * here. */

	    TRACE_IPC_DETAILS("ipc %t received msg from %t (virtual %t)",
			      current, tcb_get_partner_tcb (current),
			      TID(tcb_get_virtual_sender (current)));

	    /* XXX VU: restructure switching code so that dequeueing
	     * from wakeup is removed from critical path */
	    sched_ktcb_cancel_timeout (&current->sched_state);
	}
	else
	{

	    TRACE_IPC_DETAILS("ipc perform receive from %t",  from_tcb);

	    // both threads on the same CPU?
	    if (EXPECT_TRUE( tcb_is_local_cpu (from_tcb) ))
	    {
		/* partner is ready to send */
		tcb_set_state (from_tcb, THREAD_STATE_LOCKED_RUNNING);
		tcb_set_state (current, THREAD_STATE_LOCKED_WAITING);
		tcb_unlock (current);

		/* Switch to waiting partner.
		 * If we do not switch to woken up we have to dequeue him
		 * from the wakeup queue to make sure the IPC does not
		 * timeout meanwhile...
		 */
		if ( to_tcb != NULL)
		{
		    tcb_set_state (to_tcb, THREAD_STATE_RUNNING);
		    sched_schedule_two (from_tcb, to_tcb, sched_rplywt);
		}
		else
		    sched_schedule (from_tcb, sched_ipcblk);
	    }
#if defined(CONFIG_SMP)
	    else
	    {
		TRACE_XIPC_DETAILS("ipc xcpu receive curr=%t:%d -> from=%t:%d",
			   current, tcb_get_cpu (current), from_tcb, tcb_get_cpu (from_tcb));

                tcb_set_partner (current, from_tid);
		tcb_set_state (current, THREAD_STATE_LOCKED_WAITING);
		tcb_set_state (current, THREAD_STATE_LOCKED_WAITING);
		if (EXPECT_TRUE (lock_state_is_enabled (&current->lock_state)))
		{
		    tcb_set_state (from_tcb, THREAD_STATE_LOCKED_RUNNING);
		    // remote enqueue into ready
		    sched_remote_schedule (from_tcb);
		}
		else
		    xcpu_request_c (tcb_get_cpu (from_tcb), do_xcpu_receive, from_tcb, (word_t)current);

		tcb_unlock (current);

		if (!to_tcb) to_tcb = get_idle_tcb_c ();
		sched_schedule (to_tcb, sched_ipcblk);
		TRACE_XIPC_DETAILS("ipc xcpu receive done (from=%t, curr=%t)\n", from_tcb, current);
	    }
#endif
	}
	tcb_set_state (current, THREAD_STATE_RUNNING);
#if defined(HANDLE_LOCAL_IDS)
	if (tcb_get_space (current) == tcb_get_space (from_tcb))
	    return_ipc (tcb_get_local_id (from_tcb));
#endif
	return_ipc(tcb_get_partner (current));
    }

    // this case should never happen
    enter_kdebug("ipc fall-through - why?");
    spin_forever_c (0);
}
