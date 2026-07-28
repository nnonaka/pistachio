/*********************************************************************
 *                
 * Copyright (C) 2007-2010,  Karlsruhe University
 *                
 * File path:     api/v4/sched-rr/schedule.c
 * Description:   
 *                
 * @LICENSE@
 *                
 * $Id:$
 *                
 ********************************************************************/
#include <debug.h>
#include INC_API(tcb.h)
#include INC_API(schedule.h)
#include INC_API(interrupt.h)
#include INC_API(queueing.h)
#include INC_API(syscalls.h)
#include INC_API(smp.h)
#include INC_API(cpu.h)
#include INC_API(kernelinterface.h)
#include INC_GLUE(syscalls.h)
#include INC_GLUE(config.h)
#include INC_GLUE(schedule.h)

#define TOTAL_QUANTUM_EXPIRED (~0ULL)
/* Defined in api/v4/schedule.c; the EXTERN_TRACEPOINT that used to cover this
   was in sched-rr/schedule_functions.h, now removed (see notes §106). */
EXTERN_TRACEPOINT(SCHEDULE_IDLE);
DECLARE_TRACEPOINT(TOTAL_QUANTUM_EXPIRED);
DECLARE_TRACEPOINT(SCHEDULE_WAKEUP_TIMEOUT);
DECLARE_TRACEPOINT(TIMESLICE_EXPIRED);
DECLARE_TRACEPOINT(SCHEDULE_PM_DELAYED);
DECLARE_TRACEPOINT(SCHEDULE_PM_DELAY_OVERRULED);
DECLARE_TRACEPOINT(SCHEDULE_PM_FAULT);
DECLARE_TRACEPOINT(SCHEDULE_PM_DELAY_REFRESH);

/* local C forms of helpers whose header inlines are not visible to C here */
extern scheduler_t scheduler;
INLINE scheduler_t * cur_sched (void)		{ return &scheduler; }
INLINE tcb_t * to_tcb (void *addr)		{ return (tcb_t *) ((word_t) addr & KTCB_MASK); }
INLINE utcb_t * tcb_utcb (tcb_t *self)		{ return self->utcb; }

/* was rr_scheduler_t::current_time (a static member) */
volatile u64_t rr_sched_current_time = 0;

#if defined(CONFIG_SMP)
/* was rr_scheduler_t::smp_requeue_lists (a static member) */
static smp_requeue_t smp_requeue_lists[CONFIG_SMP_MAX_CPUS];
#endif

/**********************************************************************
 *
 *  helpers that were inline methods in the headers; they need scheduler_t /
 *  tcb_t to be complete, which they only are here.
 *
 **********************************************************************/

INLINE prio_queue_t * sched_prio_queue (scheduler_t *self)
{ return &self->__base.root_prio_queue; }

INLINE void prio_queue_enqueue (prio_queue_t *self, tcb_t *tcb, bool head)
{
    ASSERT (tcb);
    ASSERT (tcb != get_idle_tcb_c ());

    if (queue_state_is_set (&tcb->queue_state, QUEUE_STATE_READY))
	return;

    prio_t prio = rr_sched_get_priority (&tcb->sched_state.base);

    if (head)
	ENQUEUE_LIST_HEAD (self->prio_queue[prio], tcb, sched_state.base.ready_list);
    else
	ENQUEUE_LIST_TAIL (self->prio_queue[prio], tcb, sched_state.base.ready_list);

    queue_state_set (&tcb->queue_state, QUEUE_STATE_READY);
    if ((s16_t) prio > self->max_prio)
	self->max_prio = (s16_t) prio;
}

INLINE void prio_queue_dequeue (prio_queue_t *self, tcb_t *tcb)
{
    ASSERT (tcb);
    ASSERT (tcb != get_idle_tcb_c ());
    if (!queue_state_is_set (&tcb->queue_state, QUEUE_STATE_READY))
	return;

    prio_t prio = rr_sched_get_priority (&tcb->sched_state.base);
    DEQUEUE_LIST (self->prio_queue[prio], tcb, sched_state.base.ready_list);
    queue_state_clear (&tcb->queue_state, QUEUE_STATE_READY);
}

INLINE void sched_enqueue_ready (scheduler_t *self, tcb_t *tcb, bool head)
{
    ASSERT (tcb);
    ASSERT (tcb_is_local_cpu (tcb));
    prio_queue_enqueue (sched_prio_queue (self), tcb, head);
}

INLINE void sched_dequeue_ready (scheduler_t *self, tcb_t *tcb)
{
    ASSERT (tcb);
    prio_queue_dequeue (sched_prio_queue (self), tcb);
}

/* rr_sched_ktcb_t::delay_preemption */
INLINE bool rr_delay_preemption (rr_sched_ktcb_t *self, tcb_t *tcb)
{
    /* we always allow ourself to delay our preemption */
    if (to_tcb (self) == tcb)
	return true;
    else if (rr_sched_get_sensitive_prio (self) < rr_sched_get_priority (&tcb->sched_state.base))
	return false;
    return (rr_sched_get_maximum_delay (self) > 0);
}

/* tcb_t::get/set_preempt_flags (the utcb field is C-visible) */
INLINE preempt_flags_t tcb_preempt_flags (tcb_t *self)
{ preempt_flags_t f; f.raw = self->utcb->preempt_flags; return f; }
INLINE void tcb_preempt_flags_set (tcb_t *self, preempt_flags_t f)
{ self->utcb->preempt_flags = f.raw; }

/* scheduler_t::check_dispatch_thread */
INLINE bool check_dispatch_thread (tcb_t *tcb, tcb_t *dest)
{
    preempt_flags_t pf = tcb_preempt_flags (tcb);
    if (EXPECT_FALSE (preempt_flags_is_delayed (&pf) &&
		      rr_sched_get_maximum_delay (&tcb->sched_state.base)))
	return !rr_delay_preemption (&tcb->sched_state.base, dest);
    return (rr_sched_get_priority (&tcb->sched_state.base) <
	    rr_sched_get_priority (&dest->sched_state.base));
}

INLINE void sched_set_accounted (scheduler_t *self, tcb_t *tcb)
{ prio_queue_set_timeslice_tcb (sched_prio_queue (self), tcb); }

INLINE tcb_t * sched_get_accounted (scheduler_t *self)
{ return prio_queue_get_timeslice_tcb (sched_prio_queue (self)); }

/* rr_scheduler_t::enqueue_timeout / dequeue_timeout are used via schedule.c */

static tcb_t * sched_find_next_thread (scheduler_t *self);
static void rr_smp_requeue (scheduler_t *self, bool holdlock);
static void rr_end_of_timeslice (scheduler_t *self, tcb_t *tcb);
static tcb_t * rr_parse_wakeup_queues (scheduler_t *self, tcb_t *current);

/**********************************************************************
 *
 *  scheduler_t methods (were INLINEs in sched-rr/schedule_functions.h)
 *
 **********************************************************************/

static bool do_schedule_current (void)
{
    scheduler_t *self = cur_sched ();
    tcb_t *tcb = sched_find_next_thread (self);
    tcb_t *current = get_current_tcb ();

    ASSERT (tcb);
    ASSERT (current);

    /* the newly selected thread gets accounted */
    sched_set_accounted (self, tcb);

    /* do not switch to ourself */
    if (tcb == current)
	return false;

    if (current != get_idle_tcb_c ())
	sched_enqueue_ready (self, current, false);

    tcb_switch_to (current, tcb);

    return true;
}

static bool do_schedule (tcb_t *dest, word_t flags)
{
    scheduler_t *self = cur_sched ();
    tcb_t *current = get_current_tcb ();

    ASSERT (tcb_get_cpu (current) == tcb_get_cpu (dest));
    ASSERT (FLAG_IS_SET (flags, sched_chk_flag) ||
	    FLAG_IS_SET (flags, sched_ds2_flag) ||
	    FLAG_IS_SET (flags, sched_ds1_flag));

    if (FLAG_IS_SET (flags, sched_ds2_flag) ||
	(FLAG_IS_SET (flags, sched_chk_flag) && check_dispatch_thread (current, dest)))
    {
	ASSERT (current != dest);

	/* during IPC, perform TS donation and lazy destination queueing */
	if (!FLAG_IS_SET (flags, rr_tsdonate_flag))
	    sched_set_accounted (self, dest);

	/* make sure we are in the ready queue */
	if (FLAG_IS_SET (flags, sched_c2r_flag) && current != get_idle_tcb_c ())
	    sched_enqueue_ready (self, current, true);

	tcb_switch_to (current, dest);
	return true;
    }
    else
    {
	/* according to the scheduler the current thread should remain active,
	 * so simply activate the other guy and return */
	if (dest != get_idle_tcb_c ())
	    sched_enqueue_ready (self, dest, false);

	if (FLAG_IS_SET (flags, sched_timeout_flag))
	    sched_ktcb_cancel_timeout (&dest->sched_state);

	return false;
    }
}

static bool do_schedule_two (tcb_t *dest1, tcb_t *dest2, word_t flags)
{
    scheduler_t *self = cur_sched ();
    tcb_t *current = get_current_tcb ();
    tcb_t *dest;
    bool  ret;

    ASSERT (FLAG_IS_SET (flags, sched_chk_flag) ||
	    FLAG_IS_SET (flags, sched_ds1_flag) ||
	    FLAG_IS_SET (flags, sched_ds2_flag));
    ASSERT (current != dest1 && current != dest2 && dest1 != dest2);
    ASSERT (tcb_get_cpu (current) == tcb_get_cpu (dest1));

    if (FLAG_IS_SET (flags, sched_ds2_flag) ||
	(FLAG_IS_SET (flags, sched_chk_flag) && check_dispatch_thread (dest1, dest2)))
    {
	if (FLAG_IS_SET (flags, sched_timeout_flag))
	    sched_ktcb_cancel_timeout (&dest1->sched_state);
	sched_enqueue_ready (self, dest1, false);
	dest = dest2;
	ret = false;
    }
    else
    {
	sched_enqueue_ready (self, dest2, false);
	dest = dest1;
	ret = true;
    }

    /* during IPC, perform TS donation */
    if (!FLAG_IS_SET (flags, rr_tsdonate_flag))
	sched_set_accounted (self, dest);

    /* make sure we are in the ready queue */
    if (FLAG_IS_SET (flags, sched_c2r_flag) && current != get_idle_tcb_c ())
	sched_enqueue_ready (self, current, true);

    tcb_switch_to (current, dest);
    return ret;
}

static bool do_schedule_interrupt (tcb_t *irq, tcb_t *handler)
{
    tcb_set_tag (irq, msg_tag_irq_tag ());
    tcb_set_partner (irq, tcb_get_global_id (handler));
    tcb_set_state (irq, THREAD_STATE_POLLING);
    tcb_lock (handler);
    tcb_enqueue_send (irq, handler);
    tcb_unlock (handler);
    return true;
}

void sched_deschedule (tcb_t *tcb)
{
    sched_dequeue_ready (cur_sched (), tcb);
}

bool sched_is_scheduler (tcb_t *tcb, tcb_t *dest_tcb)
{
    ASSERT (tcb_exists (tcb));
    ASSERT (dest_tcb);

    if (is_privileged_space_c (tcb_get_space (tcb)))
	return true;

    /* are we in the same address space as the scheduler of the thread? */
    threadid_t scheduler_tid = sched_ktcb_get_scheduler (&dest_tcb->sched_state);
    tcb_t *scheduler_tcb = tcb_get_tcb (scheduler_tid);

    threadid_t gid = tcb_get_global_id (tcb);
    if (!threadid_equals (&gid, &scheduler_tid) ||
	(tcb_get_space (tcb) != tcb_get_space (scheduler_tcb)))
	return false;

    return true;
}

u64_t sched_get_current_time (void)
{
    return rr_sched_current_time;
}

tcb_t * sched_get_accounted_tcb (void)
{ return sched_get_accounted (cur_sched ()); }

void sched_set_accounted_tcb (tcb_t *tcb)
{ sched_set_accounted (cur_sched (), tcb); }

bool sched_idle_hlt (void)
{
    TRACEPOINT (SCHEDULE_IDLE, "idle loop by user hlt");
    return false;
}

word_t sched_check_schedule_parameters (tcb_t *scheduler, schedule_req_t *req)
{
    if (req->prio_control.raw != 0 &&
	req->prio_control.prio > rr_sched_get_priority (&scheduler->sched_state.base) &&
	!is_privileged_space_c (tcb_get_space (scheduler)))
	return ENO_PRIVILEGE;

    if (req->time_control.raw != 0 &&
	(!(req->time_control.total_quantum.time.type == 0) ||
	 !(req->time_control.timeslice.time.type == 0)))
	return EINVALID_THREAD;

    /* only set sensitive prio if _at most_ equal to the scheduler's prio */
    if (req->preemption_control.raw != 0 &&
	(prio_t) req->prio_control.sensitive_prio > rr_sched_get_priority (&scheduler->sched_state.base))
	return ENO_PRIVILEGE;

    return EOK;
}

void sched_commit_schedule_parameters (schedule_req_t *req)
{
    if (req->prio_control.raw != 0)
    {
	sched_deschedule (req->tcb);

	if ((word_t) req->prio_control.prio <= MAX_PRIORITY &&
	    req->prio_control.prio != rr_sched_get_priority (&req->tcb->sched_state.base))
	    rr_sched_set_priority (&req->tcb->sched_state.base, (prio_t) req->prio_control.prio);

	do_schedule (req->tcb, sched_current);
    }

    if (req->preemption_control.raw != 0)
    {
	rr_sched_init_maximum_delay (&req->tcb->sched_state.base,
				     (u16_t) req->preemption_control.max_delay);

	/* only set sensitive prio if _at most_ equal to current prio */
	if ((prio_t) req->preemption_control.sensitive_prio >=
	    rr_sched_get_priority (&req->tcb->sched_state.base))
	    rr_sched_set_sensitive_prio (&req->tcb->sched_state.base,
					 (prio_t) req->preemption_control.sensitive_prio);
    }

    if (req->processor_control.raw != 0)
	tcb_migrate_to_processor (req->tcb, req->processor_control.processor);

    if (req->time_control.raw != 0)
    {
	rr_sched_init_timeslice (&req->tcb->sched_state.base, req->time_control.timeslice);
	rr_sched_set_total_quantum (&req->tcb->sched_state.base,
				    time_get_microseconds (&req->time_control.total_quantum));
    }
}

word_t sched_return_schedule_parameter (word_t num, schedule_req_t *req)
{
    if (!req->tcb) return 0;

    if (num == 0)
    {
	thread_state_t state; state.state = tcb_get_state (req->tcb);
	return (state.state == THREAD_STATE_ABORTED		? 1 :
		thread_state_is_halted (&state)			? 2 :
		thread_state_is_running (&state)		? 3 :
		thread_state_is_polling (&state)		? 4 :
		thread_state_is_sending (&state)		? 5 :
		thread_state_is_waiting (&state)		? 6 :
		thread_state_is_receiving (&state)		? 7 :
		thread_state_is_xcpu_waiting (&state)		? 6 :
		({ WARNING("invalid state  tcb %t (%x)\n", req->tcb, (word_t) state.state); (word_t) 0;}));
    }
    else
    {
	word_t rem_ts = (word_t) rr_sched_get_timeslice (&req->tcb->sched_state.base);
	word_t rem_tq = (word_t) rr_sched_get_total_quantum (&req->tcb->sched_state.base);
	return (rem_ts << 16) | (rem_tq & 0xffff);
    }
}

/**********************************************************************
 *
 *  the policy methods (were out-of-line in this file)
 *
 **********************************************************************/

/**
 * the idle thread checks for runnable threads in the run queue
 * and performs a thread switch if possible. Otherwise, it
 * invokes a system sleep function (which should normally result in
 * a processor halt)
 */
void sched_idle (void)
{
    TRACE_INIT("\tIdle thread started on CPU %d\n", get_current_cpu());

    while (1)
    {
	if (!do_schedule_current ())
	{
	    spin (78, get_current_cpu ());
	    processor_sleep ();
	}
    }
}

/**
 * find_next_thread: selects the next tcb to be dispatched
 */
static tcb_t * sched_find_next_thread (scheduler_t *self)
{
    prio_queue_t *prio_queue = sched_prio_queue (self);
    ASSERT (prio_queue);

#if defined(CONFIG_SMP)
    /* requeue threads which got activated by other CPUs */
    rr_smp_requeue (self, false);
#endif

    ASSERT (prio_queue->max_prio <= MAX_PRIORITY);

    for (s16_t prio = prio_queue->max_prio; prio >= 0; prio--)
    {
	tcb_t *tcb = prio_queue_get (prio_queue, (prio_t) prio);
	while (tcb)
	{
	    ASSERT (queue_state_is_set (&tcb->queue_state, QUEUE_STATE_READY));

	    thread_state_t st; st.state = tcb_get_state (tcb);
	    if (thread_state_is_runnable (&st))
	    {
		prio_queue_set (prio_queue, (prio_t) prio, tcb);
		prio_queue->max_prio = prio;
		return tcb;
	    }
	    else
	    {
		/* dequeue non-runnable thread */
		prio_queue_dequeue (prio_queue, tcb);
		tcb = prio_queue_get (prio_queue, (prio_t) prio);
	    }
	}
    }
    /* if we can't find a schedulable thread - switch to idle */
    prio_queue->max_prio = -1;
    return get_idle_tcb_c ();
}

/**
 * sends preemption IPC to the scheduler thread that the total quantum
 * has expired
 */
static void rr_total_quantum_expired (tcb_t *tcb)
{
    TRACEPOINT (TOTAL_QUANTUM_EXPIRED, "total quantum expired for %t\n", tcb);

    enter_kdebug ("total quantum IPC unimplemented");
    UNIMPLEMENTED ();
}

static tcb_t * rr_parse_wakeup_queues (scheduler_t *self, tcb_t *current)
{
    if (!self->__base.wakeup_list)
	return current;

    /* use thread owning the current timeslice */
    tcb_t *highest_wakeup = prio_queue_get_timeslice_tcb (sched_prio_queue (self));
    tcb_t *tcb = self->__base.wakeup_list;

    ASSERT (highest_wakeup);

    /* check if the current timeslice holder is higher prio than timeslice owner */
    if (!check_dispatch_thread (current, highest_wakeup))
	highest_wakeup = current;

    bool list_head_changed = false;

    do
    {
	sched_ktcb_t *sched_state = &tcb->sched_state;
	list_head_changed = false;
	if (rr_sched_has_timeout_expired (&sched_state->base, rr_sched_current_time))
	{
	    /*
	     * We might try to wake up a thread which is waiting forever.
	     * (see the original comment: lazy dequeueing after a fast-path
	     * reply, or an xfer timeout which should be let trigger.)
	     */
	    thread_state_t st; st.state = tcb_get_state (tcb);
	    if (thread_state_is_waiting_forever (&st) &&
		!tcb_flags_is_set (tcb, TCB_FLAG_HAS_XFER_TIMEOUT))
	    {
		tcb_t *tmp = tcb->sched_state.base.wait_list.next;
		sched_ktcb_cancel_timeout (sched_state);
		list_head_changed = true;
		tcb = tmp;
		continue;
	    }

	    TRACEPOINT (SCHEDULE_WAKEUP_TIMEOUT, "wakeup timeout wu=%p to=%ld time=%ld\n",
			tcb, rr_sched_get_timeout (&sched_state->base),
			(word_t) rr_sched_current_time);

	    /* we have to wakeup the guy */
	    if (check_dispatch_thread (highest_wakeup, tcb))
		highest_wakeup = tcb;

	    tcb_t *tmp = tcb->sched_state.base.wait_list.next;
	    sched_ktcb_cancel_timeout (sched_state);
	    list_head_changed = true;

	    tcb_flags_remove (tcb, TCB_FLAG_HAS_XFER_TIMEOUT);

	    thread_state_t st2; st2.state = tcb_get_state (tcb);
	    if (thread_state_is_sending (&st2) || thread_state_is_receiving (&st2))
	    {
		/*
		 * The thread must invoke the function which handles the IPC
		 * timeout.  The handler returns directly to user level with
		 * an error code.
		 */
		tcb_notify_word (tcb, handle_ipc_timeout, (word_t) st2.state);
	    }

	    /* set it running and enqueue into ready-queue */
	    tcb_set_state (tcb, THREAD_STATE_RUNNING);
	    sched_enqueue_ready (self, tcb, false);

	    tcb = tmp;
	}
	else
	    tcb = tcb->sched_state.base.wait_list.next;
    } while (self->__base.wakeup_list && (tcb != self->__base.wakeup_list || list_head_changed));

    if (highest_wakeup == prio_queue_get_timeslice_tcb (sched_prio_queue (self)))
	highest_wakeup = current;

    return highest_wakeup;
}

/**
 * selects the next runnable thread in a round-robin fashion
 */
static void rr_end_of_timeslice (scheduler_t *self, tcb_t *tcb)
{
    spin (74, get_current_cpu ());
    ASSERT (tcb);
    ASSERT (tcb != get_idle_tcb_c ());   /* the idler never yields */

    prio_queue_t *prio_queue = sched_prio_queue (self);
    ASSERT (prio_queue);

    tcb_t *timeslice_tcb = prio_queue_get_timeslice_tcb (prio_queue);
    ASSERT (timeslice_tcb);

    /*
     * if the timeslice TCB is in the prio queue perform RR scheduling,
     * otherwise the thread gets enqueued later on
     */
    sched_ktcb_t *tsched_state = &timeslice_tcb->sched_state;

    if (queue_state_is_set (&timeslice_tcb->queue_state, QUEUE_STATE_READY))
	prio_queue_set (prio_queue, rr_sched_get_priority (&tsched_state->base),
			tsched_state->base.ready_list.next);

    /*
     * make sure we are in the ready list, enqueue at tail to give others a
     * chance to run; if still in the ready list the position is maintained
     */
    sched_enqueue_ready (self, tcb, false);

    /* renew timeslice of accounted TCB */
    rr_sched_renew_timeslice (&tsched_state->base,
			      (u32_t) rr_sched_get_timeslice_length (&tsched_state->base));

    /* clear the accounted TCB */
    prio_queue_set_timeslice_tcb (prio_queue, NULL);
}

#if defined(CONFIG_SMP)
static void rr_smp_requeue (scheduler_t *self, bool holdlock)
{
    smp_requeue_t *rq = &smp_requeue_lists[get_current_cpu ()];

    if (!smp_requeue_is_empty (rq) || holdlock)
    {
	spinlock_lock (&rq->lock);
	tcb_t *tcb = NULL;

	while (!smp_requeue_is_empty (rq))
	{
	    tcb = smp_requeue_dequeue_head (rq);
	    ASSERT (tcb_get_cpu (tcb) == get_current_cpu ());

	    if (tcb->sched_state.base.requeue_callback)
	    {
		tcb->sched_state.base.requeue_callback (tcb);
		tcb->sched_state.base.requeue_callback = NULL;
	    }
	    else
	    {
		sched_ktcb_cancel_timeout (&tcb->sched_state);
		thread_state_t st; st.state = tcb_get_state (tcb);
		if (thread_state_is_runnable (&st))
		    sched_enqueue_ready (self, tcb, false);
	    }
	}
	if (!holdlock)
	    spinlock_unlock (&rq->lock);

	do_schedule_current ();
    }
}

void sched_remote_schedule (tcb_t *tcb)
{
    if (!tcb->sched_state.base.requeue)
    {
	cpuid_t cpu = tcb_get_cpu (tcb);
	smp_requeue_t *rq = &smp_requeue_lists[cpu];
	spinlock_lock (&rq->lock);
	if (tcb_get_cpu (tcb) != cpu)
	{
	    /* thread may have migrated meanwhile */
	    UNIMPLEMENTED ();
	}
	smp_requeue_enqueue_head (rq, tcb);
	spinlock_unlock (&rq->lock);
	smp_xcpu_trigger (cpu);
    }
}

/**
 * re-integrates a thread into the CPUs queues etc.
 */
static void xcpu_integrate_thread (tcb_t *tcb)
{
    ASSERT (!queue_state_is_set (&tcb->queue_state, QUEUE_STATE_WAKEUP));
    ASSERT (tcb != get_current_tcb ());

    tcb_lock (tcb);

    /* interrupt threads are handled specially */
    if (tcb_is_interrupt_thread (tcb))
    {
	migrate_interrupt_end (tcb);
	tcb_unlock (tcb);
	return;
    }

    tcb_unlock (tcb);

    sched_ktcb_t *sched_state = &tcb->sched_state;
    /* the thread may have received an IPC meanwhile, so check whether it is
     * already running again */
    thread_state_t st; st.state = tcb_get_state (tcb);
    if (thread_state_is_runnable (&st))
	do_schedule (tcb, sched_default);
    else if (rr_sched_get_timeout (&sched_state->base) &&
	     thread_state_is_waiting_with_timeout (&st))
	sched_ktcb_set_timeout_abs (sched_state,
				    sched_get_current_time () +
				    rr_sched_get_timeout (&sched_state->base), true);
}

void sched_move_tcb (tcb_t *tcb, cpuid_t cpu)
{
    scheduler_t *self = cur_sched ();

    tcb_lock (tcb);

    /* it is only necessary to notify the other CPU if the thread is in one of
     * the scheduling queues (wakeup, ready) or is an interrupt thread */
    thread_state_t st; st.state = tcb_get_state (tcb);
    bool need_xcpu = thread_state_is_runnable (&st) ||
	(rr_sched_get_timeout (&tcb->sched_state.base) != 0) ||
	tcb_is_interrupt_thread (tcb);

    rr_smp_requeue (self, true);
    ASSERT (tcb->sched_state.base.requeue == NULL);

    if (tcb_get_space (tcb))
	space_move_tcb (tcb_get_space (tcb), tcb, get_current_cpu (), cpu);

    tcb_set_cpu (tcb, cpu);
    spinlock_unlock (&smp_requeue_lists[get_current_cpu ()].lock);

    if (need_xcpu)
    {
	tcb->sched_state.base.requeue_callback = xcpu_integrate_thread;
	sched_remote_schedule (tcb);
    }
    tcb_unlock (tcb);
}
#endif /* CONFIG_SMP */

void sched_handle_timer_interrupt (void)
{
    scheduler_t *self = cur_sched ();

    spin (77, get_current_cpu ());

#if defined(CONFIG_DEBUG)
    if (kdebug_check_interrupt ())
	return;
#endif

    if (get_current_cpu () == 0)
    {
	/* update the global time */
	rr_sched_current_time += get_timer_tick_length ();
    }

#if defined(CONFIG_SMP)
    process_xcpu_mailbox ();
    rr_smp_requeue (self, false);
#endif

    tcb_t *current = get_current_tcb ();
    tcb_t *wakeup = rr_parse_wakeup_queues (self, current);

    /* the idle thread schedules itself so no point to do it here. */
    if (current == get_idle_tcb_c ())
	return;

    /* tick timeslice */
    bool reschedule = false;

    tcb_t *timeslice_tcb = prio_queue_get_timeslice_tcb (sched_prio_queue (self));
    ASSERT (timeslice_tcb);

    sched_ktcb_t *tsched_state = &timeslice_tcb->sched_state;
    sched_ktcb_t *csched_state = &current->sched_state;

    /* Check for not infinite timeslice and expired */
    if (EXPECT_TRUE (rr_sched_get_timeslice_length (&tsched_state->base) != 0) &&
	EXPECT_FALSE (rr_sched_account_timeslice (&tsched_state->base,
						  (u32_t) get_timer_tick_length ()) <= 0))
    {
	ASSERT (tcb_utcb (current));

	preempt_flags_t pf = tcb_preempt_flags (current);
	if (preempt_flags_is_delayed (&pf) &&
	    rr_delay_preemption (&csched_state->base, wakeup))
	{
	    rr_sched_renew_timeslice (&tsched_state->base,
				      rr_sched_get_maximum_delay (&csched_state->base));
	    rr_sched_set_maximum_delay (&csched_state->base, 0);
	    preempt_flags_t npf = tcb_preempt_flags (current);
	    tcb_preempt_flags_set (current, preempt_flags_set_pending (&npf));
	}
	else
	{
	    /* We have end-of-timeslice. */
	    TRACEPOINT (TIMESLICE_EXPIRED, "timeslice expired for %t\n", current);

	    rr_end_of_timeslice (self, current);
	    reschedule = true;
	}
    }

    /* a higher priority thread was woken up - switch to him.
     * Note: wakeup respects delayed preemption flags */
    if (!reschedule)
    {
	if (wakeup == current)
	    return;

	ASSERT (wakeup);
	do_schedule (wakeup, sched_dest);
	return;
    }

    /* time slice expired */
    if (EXPECT_FALSE (rr_sched_get_total_quantum (&tsched_state->base) != 0))
    {
	/* we have a total quantum - so do some book-keeping */
	if (rr_sched_get_total_quantum (&tsched_state->base) == TOTAL_QUANTUM_EXPIRED)
	{
	    rr_total_quantum_expired (timeslice_tcb);
	}
	else if (rr_sched_get_total_quantum (&tsched_state->base) <=
		 rr_sched_get_timeslice_length (&tsched_state->base))
	{
	    /* we are getting close... */
	    rr_sched_renew_timeslice (&tsched_state->base,
				      (u32_t) rr_sched_get_total_quantum (&tsched_state->base));
	    rr_sched_set_total_quantum (&tsched_state->base, TOTAL_QUANTUM_EXPIRED);
	}
	else
	{
	    /* account this time slice */
	    rr_sched_account_quantum (&tsched_state->base,
				      (u32_t) rr_sched_get_timeslice_length (&tsched_state->base));
	}
    }

    /* schedule the next thread */
    do_schedule_current ();
}

/**********************************************************************
 *
 *  remaining C entry points (were the extern "C" wrapper blocks)
 *
 **********************************************************************/

bool sched_schedule_requests_pending (cpuid_t cpu)
{
    return !schedule_request_queue_is_empty (&schedule_request_queue[cpu]);
}

/* xcpu_handler_t only exists under CONFIG_SMP, and every caller of this is
   itself inside a CONFIG_SMP block. */
#if defined(CONFIG_SMP)
void xcpu_request_c (cpuid_t dstcpu, xcpu_handler_t handler, tcb_t *tcb, word_t param0)
{
    xcpu_request_many (dstcpu, handler, tcb, param0, 0, 0, 0);
}
#endif

void sched_ktcb_sys_thread_switch (sched_ktcb_t *self)
{
    (void) self;
    do_schedule_current ();
}


/* public (void) entry points declared in api/v4/schedule.h */
void sched_schedule_current (void)			{ do_schedule_current (); }
void sched_schedule (tcb_t *dest, word_t flags)		{ do_schedule (dest, flags); }
void sched_schedule_two (tcb_t *dest1, tcb_t *dest2, word_t flags)
							{ do_schedule_two (dest1, dest2, flags); }
void sched_schedule_interrupt (tcb_t *irq, tcb_t *handler) { do_schedule_interrupt (irq, handler); }


/**********************************************************************
 *
 *  sched_ktcb_t entry points (were C++ wrappers over the class methods)
 *
 **********************************************************************/

/* rr_scheduler_t::enqueue_timeout / dequeue_timeout */
INLINE void sched_enqueue_timeout (scheduler_t *self, tcb_t *tcb)
{
    ASSERT (tcb);
    ASSERT (tcb_get_cpu (tcb) == get_current_cpu ());
    ENQUEUE_LIST_TAIL (self->__base.wakeup_list, tcb, sched_state.base.wait_list);
    queue_state_set (&tcb->queue_state, QUEUE_STATE_WAKEUP);
}

INLINE void sched_dequeue_timeout (scheduler_t *self, tcb_t *tcb)
{
    ASSERT (tcb);
    DEQUEUE_LIST (self->__base.wakeup_list, tcb, sched_state.base.wait_list);
    queue_state_clear (&tcb->queue_state, QUEUE_STATE_WAKEUP);
}

void sched_ktcb_set_timeout_abs (sched_ktcb_t *self, u64_t absolute_time, bool enqueue)
{
    /* a thread should not be in the wakeup queue */
    self->base.absolute_timeout = absolute_time;

    if (enqueue)
	sched_enqueue_timeout (cur_sched (), to_tcb (self));
}

void sched_ktcb_cancel_timeout (sched_ktcb_t *self)
{
    tcb_t *tcb = to_tcb (self);
    if (EXPECT_TRUE (!queue_state_is_set (&tcb->queue_state, QUEUE_STATE_WAKEUP)))
	return;
    sched_dequeue_timeout (cur_sched (), tcb);
}

void sched_ktcb_init (sched_ktcb_t *self, sktcb_type_e type)
{
    rr_sched_init_timeslice (&self->base, rr_default_timeslice ());

    /* init_total_quantum(DEFAULT_TOTAL_QUANTUM): never -> 0, fresh timeslice */
    self->base.total_quantum = 0;
    self->base.current_timeslice = (s64_t) self->base.timeslice_length;

    switch (type)
    {
    case sktcb_user:
	rr_sched_set_priority (&self->base, DEFAULT_PRIORITY);
	break;
    case sktcb_root:
	rr_sched_set_priority (&self->base, ROOT_PRIORITY);
	break;
    case sktcb_irq:
    case sktcb_hi:
	rr_sched_set_priority (&self->base, MAX_PRIORITY);
	break;
    case sktcb_lo:
	rr_sched_set_priority (&self->base, 0);
	break;
    }
    /* defacto disable delayed preemptions */
    rr_sched_set_sensitive_prio (&self->base, rr_sched_get_priority (&self->base));
    rr_sched_set_maximum_delay (&self->base, 0);
#if defined(CONFIG_SMP)
    self->base.requeue = NULL;
#endif
}

void sched_ktcb_set_scheduler (sched_ktcb_t *self, threadid_t tid)
{ self->scheduler = tid; }

threadid_t sched_ktcb_get_scheduler (sched_ktcb_t *self)
{ return self->scheduler; }

void sched_ktcb_delete_tcb (sched_ktcb_t *self)
{
#if defined(CONFIG_SMP)
    if (self->base.requeue)
	rr_smp_requeue (cur_sched (), false);
#else
    (void) self;
#endif
}

/**********************************************************************
 *
 *  xcpu request entry points (were C++ wrappers over xcpu_request)
 *
 **********************************************************************/

#if defined(CONFIG_SMP)
static void do_xcpu_request (cpuid_t dstcpu, xcpu_handler_t handler, tcb_t *tcb,
			     word_t p0, word_t p1, word_t p2, word_t p3,
			     word_t p4, word_t p5, word_t p6, word_t p7)
{
    cpu_mb_t *mb = get_cpu_mailbox (dstcpu);
    cpu_mb_entry_t *entry = cpu_mb_alloc (mb);
    bool entered = (entry != NULL);

    if (entered)
    {
	cpu_mb_entry_set_many (entry, handler, tcb, p0, p1, p2, p3, p4, p5, p6, p7);
	cpu_mb_commit (mb, entry);
    }

    if (!entered)
    {
	printf ("CPU %d Failing XCPU requests are unimplemented cpu %d\n",
		get_current_cpu (), dstcpu);
	cpu_mb_dump_mailbox (mb, dstcpu);
	enter_kdebug ("BUG");
	spin_forever_c (0);
    }

#ifndef CONFIG_SMP_IDLE_POLL
    /* trigger an IPI */
    smp_xcpu_trigger (dstcpu);
#endif
}

void xcpu_request_many (cpuid_t dstcpu, xcpu_handler_t handler, tcb_t *tcb,
			word_t p0, word_t p1, word_t p2, word_t p3)
{ do_xcpu_request (dstcpu, handler, tcb, p0, p1, p2, p3, 0, 0, 0, 0); }

void xcpu_request7 (cpuid_t dstcpu, xcpu_handler_t handler, tcb_t *tcb,
		    word_t p0, word_t p1, word_t p2, word_t p3,
		    word_t p4, word_t p5, word_t p6)
{ do_xcpu_request (dstcpu, handler, tcb, p0, p1, p2, p3, p4, p5, p6, 0); }
#endif /* CONFIG_SMP */


/* scheduler init/start entry points (the class methods are asm-named
   scheduler_init/scheduler_start and defined in C in api/v4/schedule.c). */
void scheduler_init (scheduler_t *self, bool bootcpu);
void scheduler_start (scheduler_t *self, cpuid_t cpuid);

/* was scheduler_t::policy_scheduler_init; the body used to be inlined into
   api/v4/schedule.c's scheduler_init. */
void policy_scheduler_init (scheduler_t *self)
{
    self->__base.wakeup_list = (tcb_t *) 0;
    for (int i = 0; i <= MAX_PRIORITY; i++)
	self->__base.root_prio_queue.prio_queue[i] = (tcb_t *) 0;
    self->__base.root_prio_queue.max_prio = -1;
}

void sched_init (bool bootcpu)		{ scheduler_init (cur_sched (), bootcpu); }
void sched_start (cpuid_t cpu)		{ scheduler_start (cur_sched (), cpu); }

#if defined(CONFIG_DEBUG)
/*
 *  Debug dumps for the kdb showtcb commands.  These were INLINEs in the
 *  since-removed sched-rr/schedule_functions.h; their only caller
 *  (kdb/api/v4/tcb.c) is C, so the bodies live here.
 */

void sched_ktcb_dump_priority (sched_ktcb_t *self)
{
    printf("=== PRIO: %2d ===", self->base.priority);
#if defined(CONFIG_X_EVT_LOGGING)
    printf("= L: %2d =", self->logid);
#endif
}

void sched_ktcb_dump_list1 (sched_ktcb_t *self)
{
    printf("wait : %wt:%-wt   ", self->base.wait_list.next, self->base.wait_list.prev);
}

void sched_ktcb_dump_list2 (sched_ktcb_t *self)
{
    printf("ready: %wt:%-wt   ", self->base.ready_list.next, self->base.ready_list.prev);
}

void sched_ktcb_dump (sched_ktcb_t *self, u64_t current_time)
{
    printf("total quant:    %wdus, ts length  :       %wdus, curr ts: %wdus\n",
           (word_t)self->base.total_quantum, (word_t)self->base.timeslice_length,
           (word_t)self->base.current_timeslice);
    printf("abs timeout:    %wdus, rel timeout:       %wdus\n",
           (word_t)self->base.absolute_timeout,
           self->base.absolute_timeout == 0 ? 0 :
           (word_t)(self->base.absolute_timeout -  current_time));
    printf("sens prio: %d, delay: max=%dus, curr=%dus\n",
           self->base.sensitive_prio, self->base.max_delay, self->base.current_max_delay);
}
#endif /* CONFIG_DEBUG */
