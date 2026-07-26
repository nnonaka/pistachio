/*********************************************************************
 *
 * Copyright (C) 2002,  Karlsruhe University
 *
 * File path:    api/v4/interrupt.c
 * Description:  Interrupt handling
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
 * $Id: interrupt.cc,v 1.24 2006/06/12 15:22:03 stoess Exp $
 *
 *********************************************************************/

#include <debug.h>
#include <kdb/tracepoints.h>

#include INC_API(tcb.h)
#include INC_API(space.h)
#include INC_API(interrupt.h)
#include INC_API(schedule.h)
#include INC_API(kernelinterface.h)
#include INC_API(smp.h)
#include INC_API(cpu.h)

/* tcb.h defines TID(x) as the C++ (x).get_raw(); in C it lives only in
   dead (compiled-out) trace args, but redefine it to the C accessor to keep
   the file self-consistent. */
#undef TID
#define TID(x)	threadid_get_raw (&(x))


DECLARE_TRACEPOINT(INTERRUPT);
DECLARE_TRACEPOINT_DETAIL(INTERRUPT_DETAILS);
DECLARE_TRACEPOINT(SYSCALL_THREAD_CONTROL_IRQ);

static utcb_t *irq_utcb;
static word_t irq_utcb_count;

/* handler for dedicated irq threads (forward decl for notify() below) */
static void irq_thread(void);

#if defined(CONFIG_SMP)
/* defined in api/v4/schedule.cc with C linkage; the C-linkage decl in
   schedule.h sits inside its __cplusplus guard, so declare it for C here. */
void do_xcpu_send_irq (cpu_mb_entry_t * entry);
#endif

#if defined(CONFIG_SMP)
/* for IRQ forwarding */
static void do_xcpu_interrupt(cpu_mb_entry_t * entry)
{
    word_t irq = entry->param[0];
    handle_interrupt(irq);
}
#endif


// handler for dedicated irq threads
static void irq_thread(void)
{
    tcb_t * current = get_current_tcb();
    threadid_t cur_gid = tcb_get_global_id (current);
    word_t irq = threadid_get_irqno (&cur_gid);

    while(1)
    {
	tcb_t * handler_tcb = tcb_get_tcb (tcb_get_irq_handler (current));

	/* VU: when we are in the send queue the IRQ thread was busy
	 * and the IRQ could not be delivered. Hence, we actually
	 * have to send the IPC. Otherwise, the message was delivered
	 * on the fast path and we got activated by an ack IPC */
	if (queue_state_is_set (&current->queue_state, QUEUE_STATE_SEND))
	{
	    // got activated -- do send operation
	    ASSERT(tcb_is_local_cpu (handler_tcb));
	    ASSERT(tcb_is_local_cpu (current));
	    TRACE_IRQ_DETAILS("irq %d deliver message", irq);

	    tcb_lock (handler_tcb);
	    tcb_dequeue_send (current, handler_tcb);
	    tcb_set_tag (handler_tcb, msg_tag_irq_tag ());
	    tcb_set_partner (handler_tcb, tcb_get_global_id (current));
	    tcb_unlock (handler_tcb);

#if defined(CONFIG_SMP)
	    if (!tcb_is_local_cpu (handler_tcb))
	    {
		TRACE_IRQ_DETAILS("irq %d xcpu deliver IRQ message", irq);
		xcpu_request_c (tcb_get_cpu (handler_tcb), do_xcpu_send_irq, handler_tcb, irq);
		sched_schedule (get_idle_tcb_c (), sched_handoff);
	    }
	    else
#endif
	    {
		tcb_set_partner (current, tcb_get_irq_handler (current));
		tcb_set_state (current, THREAD_STATE_WAITING_FOREVER);
		sched_schedule (handler_tcb, sched_handoff);
	    }
	}

	// got re-activated due to send operation to IRQ thread...
	TRACE_IRQ(irq, "irq %d ACK handler %t", irq, handler_tcb);
	tcb_set_state (current, THREAD_STATE_HALTED);

	// if an interrupt was already pending deliver it
	if (intctrl_unmask (irq))
	    handle_interrupt(irq);

	sched_schedule (get_idle_tcb_c (), sched_handoff);
    }
}


/**
 * interrupt handler, delivers interrupt IPC if thread is waiting
 * @param irq interrupt number
 */
void handle_interrupt(word_t irq)
{

    spin(76, get_current_cpu());

    threadid_t irq_tid = threadid_irqthread (irq);
    tcb_t * irq_tcb = tcb_get_tcb (irq_tid);

    // some sanity checks
    ASSERT(threadid_get_irqno (&irq_tid) < thread_info_get_system_base (&get_kip()->thread_info));

    TRACE_IRQ(irq, "IRQ %d (%s)", irq, irq_tcb->get_state().string());

    /* VU: we can get a spurious interrupt if we de-attached the IRQ
     * meanwhile in this case we simply ignore the IRQ */
    if ( tcb_get_state (irq_tcb) == THREAD_STATE_ABORTED )
    {
	TRACE("CPU%d spurious IRQ%d?", get_current_cpu(), irq);
	return;
    }

    // we should only receive irqs if the thread is halted
    if (!thread_state_is_halted (&irq_tcb->thread_state))
	return;

#if defined(CONFIG_SMP)
    /* while migrating it may happen that we get an IRQ -> forward to
     * other CPU */
    if ( !tcb_is_local_cpu (irq_tcb) )
    {
	xcpu_request_c (tcb_get_cpu (irq_tcb), do_xcpu_interrupt, NULL, irq );
	return;
    }
#endif

    // get the handler thread id
    threadid_t handler_tid = tcb_get_irq_handler (irq_tcb);
    tcb_t * handler_tcb = tcb_get_tcb (handler_tid);

    // if the handler TID is not valid -- abort the IRQ thread
    threadid_t handler_gid = tcb_get_global_id (handler_tcb);
    if (EXPECT_FALSE( !threadid_equals (&handler_gid, &handler_tid) ))
    {
	TRACE("IRQ handler TID is invalid -- halting IRQ%d", irq);
	tcb_set_state (irq_tcb, THREAD_STATE_ABORTED);
	return;
    }

    TRACE_IRQ_DETAILS("irq %d handler %t (s=%s)",  irq, handler_tcb,
		      (handler_tcb ? handler_tcb->get_state().string() : "UNDEF"));

    if (EXPECT_TRUE( thread_state_is_waiting (&handler_tcb->thread_state) ))
    {
	// thread is waiting for IPC -- we use a shortcut
	threadid_t partner = tcb_get_partner (handler_tcb);

	if (EXPECT_TRUE( threadid_equals (&partner, &irq_tid) || threadid_is_anythread (&partner) ))
	{
	    // set IRQ thread to be waiting for the ack IPC
	    tcb_set_partner (irq_tcb, handler_tid);
	    tcb_set_state (irq_tcb, THREAD_STATE_WAITING_FOREVER);

#if defined(CONFIG_SMP)
	    if (!tcb_is_local_cpu (handler_tcb))
	    {
		TRACE_IRQ_DETAILS("irq %d xcpu forward IRQ tcb creation", irq);
		xcpu_request_c (tcb_get_cpu (handler_tcb), do_xcpu_send_irq,
			      handler_tcb, irq);
		return;
	    }
#endif

	    TRACE_IRQ_DETAILS("irq %d deliver message", irq);

	    // deliver IPC
	    tcb_set_tag (handler_tcb, msg_tag_irq_tag ());
	    tcb_set_partner (handler_tcb, irq_tid);

	    tcb_set_state (handler_tcb, THREAD_STATE_RUNNING);
	    /* we enter this path only if the handler is waiting -- so
	     * we are not currently execuing on its TCB */
	    sched_schedule (handler_tcb, sched_default);
	    return;
	}
    }

    TRACE_IRQ_DETAILS("irq %d schedule interrupt %t", irq, irq_tcb);

    /* the thread is not waiting for IPC -- so generate nice msg and
     * enqueue into send queue */
    sched_schedule_interrupt (irq_tcb, handler_tcb);
}

#if defined(CONFIG_SMP)
static void do_xcpu_thread_control_interrupt(cpu_mb_entry_t * entry)
{
    threadid_t handler_tid;
    threadid_set_raw (&handler_tid, entry->param[0]);
    thread_control_interrupt(tcb_get_global_id (entry->tcb), handler_tid);
}
#endif

/**
 * implements the sys_thread_control-handling for interrupt threads
 * @param irq_tid interrupt thread id
 * @param handler_tid thread id of the handler thread
 * @return true if operation was successful
 */
bool thread_control_interrupt(threadid_t irq_tid, threadid_t handler_tid)
{
    // interrupt thread
    tcb_t * irq_tcb = tcb_get_tcb (irq_tid);
    word_t irq = threadid_get_irqno (&irq_tid);

    TRACEPOINT (SYSCALL_THREAD_CONTROL_IRQ, "SYS_THREAD_CONTROL for IRQ%d (IRQ=%t, handler=%t)",
		irq, TID(irq_tid), TID(handler_tid));

    // check for valid handler tid
    tcb_t * handler_tcb = tcb_get_tcb (handler_tid);
    threadid_t handler_gid = tcb_get_global_id (handler_tcb);
    if ( !threadid_equals (&handler_gid, &handler_tid) )
	return false;

    // check whether this is a valid/available IRQ
    if (!threadid_is_interrupt (&irq_tid) ||
	(irq < intctrl_get_number_irqs () && !intctrl_is_irq_available (irq)))
	return false;

    // allocate before usage to avoid remapping...
    irq_tcb = tcb_allocate (irq_tid);

    /* VU: IRQ TCBs always "exist" -- so we check whether they
     * already have a UTCB */
    if (tcb_is_activated (irq_tcb))
    {
	// check whether we really change something
	threadid_t cur_handler = tcb_get_irq_handler (irq_tcb);
	if (threadid_equals (&cur_handler, &handler_tid))
	    return true;

#if defined(CONFIG_SMP)
	if (!tcb_is_local_cpu (irq_tcb))
	{
	    xcpu_request_c (tcb_get_cpu (irq_tcb), do_xcpu_thread_control_interrupt,
			 irq_tcb, threadid_get_raw (&handler_tid));
	    return true;
	}
#endif
	// dequeue from handler's send queue if necessary
	if (thread_state_is_polling (&irq_tcb->thread_state))
	{
	    tcb_t * handler_tcb = tcb_get_partner_tcb (irq_tcb);
	    tcb_lock (handler_tcb);
	    tcb_dequeue_send (irq_tcb, handler_tcb);
	    tcb_unlock (handler_tcb);
	}
    }
    else
    {

	if (irq_utcb_count++ % (KMEM_CHUNKSIZE / sizeof(utcb_t)) == 0)
	    irq_utcb = (utcb_t *) kmem_alloc(&kmem, kmem_utcb, KMEM_CHUNKSIZE);
	else
	    irq_utcb++;

	/*
	 * Set the priority of the kernel interrupt thread to the
	 * maximum priority.  Note that this is not related to the
	 * priority of the actual interrupt the API is concerned with.
	 * Instead it only makes sure that, given the current
	 * implementation of the interrupt protocol, acknowledge
	 * messages are immediately acted upon.  This is necessary to
	 * match more closely the API idea of interrupts being
	 * represented as threads but having no execution time.
	 */

	tcb_create_kernel_thread (irq_tcb, irq_tid, irq_utcb, sktcb_irq);
	tcb_notify (irq_tcb, irq_thread);
    }

    // at this point we must be on the local CPU
    ASSERT(tcb_is_local_cpu (irq_tcb));

    tcb_set_irq_handler (irq_tcb, handler_tid);

    if (threadid_equals (&irq_tid, &handler_tid))
    {
	TRACE_IRQ_DETAILS("disable irq %d", irq);
	intctrl_disable (irq);
	tcb_set_state (irq_tcb, THREAD_STATE_ABORTED);
    }
    else
    {
	TRACE_IRQ_DETAILS("enable irq %d cpu %d", irq, tcb_get_cpu (irq_tcb));
	tcb_set_state (irq_tcb, THREAD_STATE_HALTED);
	intctrl_set_cpu (irq, tcb_get_cpu (irq_tcb));
	intctrl_enable (irq);
    }
    return true;
}

#if defined(CONFIG_SMP)
void migrate_interrupt_start (tcb_t * tcb)
{
    ASSERT(tcb_is_interrupt_thread (tcb));
    threadid_t gid = tcb_get_global_id (tcb);
    word_t irq = threadid_get_irqno (&gid);
    TRACE_IRQ_DETAILS("migrate irq %d start", irq);

    if (thread_state_is_halted (&tcb->thread_state))
	intctrl_mask (irq);
}

void migrate_interrupt_end (tcb_t * tcb)
{
    ASSERT(tcb_is_interrupt_thread (tcb));
    threadid_t gid = tcb_get_global_id (tcb);
    word_t irq = threadid_get_irqno (&gid);
    TRACE_IRQ_DETAILS("migrate irq %d end", irq);

    intctrl_set_cpu (irq, get_current_cpu());

    /*
     * js: If we got an interrupt during migration, it will be forwarded from
     * the old CPU; in this case, the irq mask must stay masked, since we
     * otherwise may get a second interrupt before having processed the
     * forwarded one. We check that case with the pending flag
     */
    if (!intctrl_is_pending (irq) &&
	thread_state_is_halted (&tcb->thread_state))
	intctrl_unmask (irq);
}
#endif

void SECTION(".init") init_interrupt_threads(void)
{
    /* initialize KIP */
    word_t num_irqs = intctrl_get_number_irqs ();

    TRACE_INIT("System has %d hardware interrupts\n", num_irqs);
    thread_info_set_system_base (&get_kip()->thread_info, num_irqs);
}
