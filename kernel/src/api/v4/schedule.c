/*********************************************************************
 *
 * Copyright (C) 2002-2010,  Karlsruhe University
 *
 * File path:     api/v4/schedule.c
 * Description:   scheduling functions
 *
 * @LICENSE@
 *
 * $Id$
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

#include <kdb/tracepoints.h>

/* TRACEF's active form (CONFIG_KDB_NO_ASSERTS) evaluates its args, which here
   are C++ getters in rare/error paths; no-op it in this C file (as in ipcx.c). */
#undef TRACEF
#define TRACEF(x...)

/* initial_switch_to is a C++-only arch inline; C form in glue thread.cc. */
void initial_switch_to_c (tcb_t *tcb);

/* global idle thread, we allocate a utcb to make accessing MRs etc easier */
whole_tcb_t __whole_idle_tcb UNIT("cpulocal") __attribute__((aligned(sizeof(whole_tcb_t))));
utcb_t	    __idle_utcb UNIT("cpulocal") __attribute__((aligned(sizeof(utcb_t))));
tcb_t *__idle_tcb = (tcb_t *) &__whole_idle_tcb;

/* global scheduler object */
scheduler_t scheduler UNIT("cpulocal");
/* was scheduler_t::schedule_request_queue (a static member); the C++ ctor only
   zeroed it, so C zero-init (BSS) is equivalent. */
schedule_request_queue_t schedule_request_queue[CONFIG_SMP_MAX_CPUS];


DECLARE_TRACEPOINT(SYSCALL_THREAD_SWITCH);
DECLARE_TRACEPOINT(SYSCALL_SCHEDULE);
DECLARE_TRACEPOINT(SCHEDULE_IDLE);
DECLARE_TRACEPOINT_DETAIL(SCHEDULE_DETAILS);
EXTERN_TRACEPOINT(INTERRUPT_DETAILS);


/* file-internal (were scheduler_t methods; no external callers). */
static void process_schedule_requests (void);
static word_t add_schedule_request (schedule_req_t *req);

/* asm-named tcb_t method (its C++ decl in tcb.h is invisible to C). */
void tcb_create_kernel_thread (tcb_t *, threadid_t, utcb_t *, sktcb_type_e);


void SECTION(".init") init_all_threads(void)
{
    init_interrupt_threads();
    init_kernel_threads();
    init_root_servers();

#if defined(CONFIG_KDB_ON_STARTUP)
    enter_kdebug ("System started (press 'g' to continue)");
#endif
}

#if defined(CONFIG_SMP)
void do_xcpu_send_irq(cpu_mb_entry_t * entry)
{
    tcb_t * handler_tcb = entry->tcb;
    word_t irq = entry->param[0];
    threadid_t irq_tid = threadid_irqthread (irq);
    tcb_t * irq_tcb = tcb_get_tcb (irq_tid);

    if (!tcb_is_local_cpu (handler_tcb))
    {
	TRACEF("xcpu-IRQ forward (%d->%t %d)", irq, handler_tcb, tcb_get_cpu (handler_tcb));
	enter_kdebug("Untested");
	xcpu_request_c (tcb_get_cpu (handler_tcb), do_xcpu_send_irq, handler_tcb, irq);
	return;
    }

    threadid_t handler_partner = tcb_get_partner (handler_tcb);
    if ((thread_state_is_waiting (&handler_tcb->thread_state) ||
	 thread_state_is_locked_waiting (&handler_tcb->thread_state)) &&
	(threadid_is_anythread (&handler_partner) ||
	 threadid_equals (&handler_partner, &irq_tid)))
    {
	// ok, thread is waiting -- deliver IRQ
	TRACE_IRQ_DETAILS("irq %d xcpu delivery (%d->%t)", irq, handler_tcb);
	tcb_set_tag (handler_tcb, msg_tag_irq_tag ());
	tcb_set_partner (handler_tcb, threadid_irqthread (irq));
	tcb_set_state (handler_tcb, THREAD_STATE_RUNNING);
	sched_schedule (handler_tcb, sched_current);
    }
    else
    {
	TRACEF("irq %d xcpu handler not ready %t s=%s",
	       irq, handler_tcb, thread_state_string (tcb_get_state (handler_tcb)));
	enter_kdebug("UNTESTED");
	tcb_set_tag (irq_tcb, msg_tag_irq_tag ());
	tcb_set_partner (irq_tcb, tcb_get_global_id (handler_tcb));
	tcb_set_state (irq_tcb, THREAD_STATE_POLLING);
	tcb_lock (handler_tcb);
	tcb_enqueue_send (irq_tcb, handler_tcb);
	tcb_unlock (handler_tcb);
    }
}
#endif

static void idle_thread(void)
{
    sched_set_accounted_tcb (get_current_tcb());
#if defined(CONFIG_X_EVT_LOGGING)
    get_idle_tcb_c()->sched_state.logid = IDLE_LOGID;
#endif
    sched_idle ();
}

SYS_THREAD_SWITCH (threadid_t dest)
{
    /* Make sure we are in the ready queue to
     * find at least ourself and ensure that the thread
     * is rescheduled */
    tcb_t * current = get_current_tcb();

    TRACEPOINT( SYSCALL_THREAD_SWITCH, "SYS_THREAD_SWITCH current=%t, dest=%t\n",
		current, TID(dest));

    /* explicit timeslice donation */
    if (!threadid_is_nilthread (&dest))
    {
	tcb_t * dest_tcb = tcb_get_tcb (dest);

	if ( dest_tcb == current )
	    return_thread_switch();

	if ( thread_state_is_runnable (&dest_tcb->thread_state) &&
	     threadid_equals (&dest_tcb->myself_global, &dest) &&
	     tcb_is_local_cpu (dest_tcb) )
	{
	    sched_schedule (dest_tcb, sched_ds2_flag + rr_tsdonate_flag);
	    return_thread_switch();
	}
    }

    sched_ktcb_sys_thread_switch (&current->sched_state);
    return_thread_switch();
}


#if defined(CONFIG_SMP)
static void do_xcpu_schedule(cpu_mb_entry_t * entry)
{
    process_schedule_requests();
}
#endif

static word_t add_schedule_request(schedule_req_t *req)
{
    ASSERT(req->tcb);

    cpuid_t cpu = get_current_cpu();
    cpuid_t reqcpu = tcb_get_cpu (req->tcb);
    schedule_req_t *qreq = (schedule_req_t *) 0;


    if (tcb_flags_is_set (req->tcb, TCB_FLAG_SCHEDULE_IN_PROGRESS))
    {
	TRACE_SCHEDULE_DETAILS("schedule %t on cpu %d still in progress, skip\n", req->tcb, reqcpu, req->tcb);
	return EINVALID_PARAM;
    }

    do
    {
	if ((qreq = schedule_request_queue_reserve_request (&schedule_request_queue[reqcpu])))
	    break;

	TRACE_SCHEDULE_DETAILS("schedule request queue cpu %d full, empty it\n", reqcpu);

	if (reqcpu == cpu)
	    process_schedule_requests();
#if defined(CONFIG_SMP)
	else
	{
	    xcpu_request_c (reqcpu, do_xcpu_schedule, (tcb_t *) 0, 0);
	    while (sched_schedule_requests_pending (reqcpu))
		; /* spin */
	}
#endif

	TRACE_SCHEDULE_DETAILS("schedule request queue cpu %d empty again, retry reservation\n", reqcpu);

    } while(!qreq);


    TRACE_SCHEDULE_DETAILS("add %t cpu %d to requeust queue on cpu %d", req->tcb, tcb_get_cpu (req->tcb), reqcpu);
    qreq->tcb = req->tcb;
    tcb_flags_add (qreq->tcb, TCB_FLAG_SCHEDULE_IN_PROGRESS);
    qreq->prio_control = req->prio_control;
    qreq->time_control = req->time_control;
    qreq->preemption_control = req->preemption_control;
    qreq->processor_control = req->processor_control;
    qreq->valid = 1;

    schedule_request_queue_commit_request (&schedule_request_queue[reqcpu]);

    return EOK;

}


static void process_schedule_requests(void)
{
    /* local part of schedule */

    cpuid_t cpu = get_current_cpu();

    TRACE_SCHEDULE_DETAILS("process schedule requests");


    while (!schedule_request_queue_is_empty (&schedule_request_queue[cpu]))
    {
	schedule_req_t req = schedule_request_queue_process_request (&schedule_request_queue[cpu]);

	if (!req.valid)
	    continue;

	tcb_t *dest_tcb = req.tcb;

	TRACE_SCHEDULE_DETAILS("process_request: %t time=%x, prio=%x proc=%x, preempt=%x",
		   dest_tcb, req.time_control.raw, req.prio_control.raw,
		   req.processor_control.raw, req.preemption_control.raw);

	if (tcb_get_cpu (dest_tcb) != cpu)
	{
	    TRACEF(" wrong cpu %t cpu %d time=%x, prio=%x proc=%x, preempt=%x",
		   dest_tcb, tcb_get_cpu (dest_tcb),
		   req.time_control.raw, req.prio_control.raw,
		   req.processor_control.raw, req.preemption_control.raw);
	    enter_kdebug("SCHEDULE BUG");
	}

	ASSERT(tcb_get_cpu (dest_tcb) == cpu);
	sched_commit_schedule_parameters (&req);
	tcb_flags_remove (dest_tcb, TCB_FLAG_SCHEDULE_IN_PROGRESS);
    }

}

SYS_SCHEDULE (threadid_t dest_tid, word_t time_control,
	      word_t processor_control, word_t prio_control,
	      word_t preemption_control )
{

    tcb_t * current = get_current_tcb();
    tcb_t * dest_tcb = tcb_get_tcb (dest_tid);

    TRACEPOINT(SYSCALL_SCHEDULE,
	       "SYS_SCHEDULE: curr=%t, dest=%t, time_ctrl=%x, "
	       "proc_ctrl=%x, prio_ctrl=%x, preemption_ctrl=%x\n",
	       current, TID(dest_tid), time_control,
	       processor_control, prio_control, preemption_control);

    schedule_req_t req;
    req.tcb = dest_tcb;
    req.time_control.raw = time_control;
    req.prio_control.raw = prio_control;
    req.preemption_control.raw = preemption_control;
    req.processor_control.raw = processor_control;
    req.valid = false;

    word_t err, ret0 = 0 , ret1 = 0;

    threadid_t nil = threadid_nilthread ();
    if (!threadid_equals (&dest_tid, &nil))
    {
        // make sure the thread id is valid
	threadid_t dest_gid = tcb_get_global_id (dest_tcb);
	if (!threadid_equals (&dest_gid, &dest_tid))
	{
	    tcb_set_error_code (get_current_tcb (), EINVALID_THREAD);
	    return_schedule(0, 0);
	}

	if (!sched_is_scheduler (current, dest_tcb))
	{
	    tcb_set_error_code (get_current_tcb (), ENO_PRIVILEGE);
	    return_schedule(0, 0);
	}

	err = sched_check_schedule_parameters (current, &req);
	if (err != EOK)
	{
	    tcb_set_error_code (get_current_tcb (), err);
	    return_schedule (0, 0);
	}

        /* Calculate return values before operation */
        ret0 = sched_return_schedule_parameter (0, &req);
        ret1 = sched_return_schedule_parameter (1, &req);

	err = add_schedule_request (&req);
	if (err != EOK)
	{
	    tcb_set_error_code (get_current_tcb (), err);
	    return_schedule (0, 0);
	}

    }


    // Process our own requests
    process_schedule_requests();

#if defined(CONFIG_SMP)
    for (cpuid_t cpu = 0; cpu < cpu_count; cpu++)
    {
	if (sched_schedule_requests_pending (cpu))
	    xcpu_request_c (cpu, do_xcpu_schedule, (tcb_t *) 0, 0);
    }
#endif

    tcb_set_error_code (get_current_tcb (), EOK);
    return_schedule(ret0, ret1);
}

/**********************************************************************
 *
 *                     Initialization
 *
 **********************************************************************/

void SECTION(".init") scheduler_start(scheduler_t *self, cpuid_t cpuid)
{
    (void) self;
    TRACE_INIT ("\tSwitching to idle thread (CPU %d)\n", cpuid);
    tcb_set_cpu (get_idle_tcb_c (), cpuid);

    initial_switch_to_c(get_idle_tcb_c ());
}

void SECTION(".init") scheduler_init(scheduler_t *self, bool bootcpu)
{

    TRACE_INIT ("\tInitializing threading (CPU %d)\n", get_current_cpu());
    /* Was protected scheduler_t::policy_scheduler_init().  This used to be
       inlined here, which silently hard-coded the round-robin policy's idea of
       "empty scheduler" into shared code: sched-hs additionally has to set up
       the root queue's domain tcb and the scheduled_queue/scheduled_tcb pair,
       and without them its first enqueue walks a NULL domain tcb.  Each policy
       supplies its own. */
    policy_scheduler_init (self);


    /* set idle-magic */
    tcb_create_kernel_thread (get_idle_tcb_c (), NILTHREAD, &__idle_utcb, sktcb_lo);
    tcb_set_space (get_idle_tcb_c (), get_kernel_space_c ());
    threadid_set (&get_idle_tcb_c ()->myself_global, IDLETHREAD);
    tcb_create_startup_stack (get_idle_tcb_c (), idle_thread);

    if( bootcpu )
    	tcb_notify (get_idle_tcb_c (), init_all_threads);


    return;
}
