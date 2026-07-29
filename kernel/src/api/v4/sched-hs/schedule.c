/*********************************************************************
 *
 * Copyright (C) 2007-2010,  Karlsruhe University
 *
 * File path:     api/v4/sched-hs/schedule.c
 * Description:   Hierarchical stride scheduling policy.
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

/* SCHEDULE_IDLE is defined in api/v4/schedule.c; the EXTERN_TRACEPOINT that
   used to cover it lived in sched-hs/schedule_functions.h, whose contents are
   folded into this file (see notes §103). */
EXTERN_TRACEPOINT(SCHEDULE_IDLE);
DECLARE_TRACEPOINT(TOTAL_QUANTUM_EXPIRED);
DECLARE_TRACEPOINT(SCHEDULE_WAKEUP_TIMEOUT);
DECLARE_TRACEPOINT(TIMESLICE_EXPIRED);
DECLARE_TRACEPOINT(SCHEDULE_PM_DELAYED);
DECLARE_TRACEPOINT(SCHEDULE_PM_DELAY_REFRESH);
DECLARE_TRACEPOINT(SCHEDULE_PRIO_DOMAIN);

DECLARE_KMEM_GROUP(kmem_sched);

FEATURESTRING ("hscheduling");

/* local C forms of helpers whose header inlines are not visible to C here */
extern scheduler_t scheduler;
INLINE scheduler_t * cur_sched (void)		{ return &scheduler; }
INLINE tcb_t * to_tcb (void *addr)		{ return (tcb_t *) ((word_t) addr & KTCB_MASK); }
INLINE utcb_t * tcb_utcb (tcb_t *self)		{ return self->utcb; }

/* threadid_t has no equality helper in C; compare the raw words. */
INLINE bool tid_is_idle (threadid_t tid)
{ threadid_t idle = IDLETHREAD; return threadid_get_raw (&tid) == threadid_get_raw (&idle); }

/* hs_sched_ktcb_t::flags is a bitmask_word_t; poke maskvalue directly, as
   api/v4/tcb.h does for tcb_t::flags. */
INLINE bool hs_flag_is_set (const hs_sched_ktcb_t *self, word_t bit)
{ return (self->flags.maskvalue & (1UL << bit)) != 0; }
INLINE void hs_flag_add (hs_sched_ktcb_t *self, word_t bit)
{ self->flags.maskvalue |= (1UL << bit); }

/* defined in api/v4/thread.c */
void tcb_create_inactive (tcb_t *, threadid_t, threadid_t, sktcb_type_e);

/* was hs_scheduler_t::current_time (a static member) */
volatile u64_t hs_sched_current_time = 0;

#if defined(CONFIG_SMP)
/* was hs_scheduler_t::smp_requeue_lists (a static member) */
static smp_requeue_t smp_requeue_lists[CONFIG_SMP_MAX_CPUS];
#endif

static void hs_smp_requeue (scheduler_t *self, bool holdlock);
static tcb_t * sched_find_next_thread (scheduler_t *self, prio_queue_t *prio_queue);
static void sched_enqueue_ready (scheduler_t *self, tcb_t *tcb, bool head);
static void sched_dequeue_ready (scheduler_t *self, tcb_t *tcb);
static bool do_schedule_current (void);
static bool do_schedule (tcb_t *dest, word_t flags);
static void hs_extended_schedule (scheduler_t *self, schedule_req_t *req);


/**********************************************************************
 *
 *  helpers that were inline methods in the headers; they need scheduler_t /
 *  tcb_t to be complete, which they only are here.
 *
 **********************************************************************/

INLINE prio_queue_t * sched_root_prio_queue (scheduler_t *self)
{ return &self->__base.root_prio_queue; }

/* prio_queue_t::enqueue */
static void prio_queue_enqueue (prio_queue_t *self, tcb_t *tcb, bool head)
{
    hs_sched_ktcb_t *sktcb;
    prio_t prio;
    word_t stride;

    ASSERT (tcb);
    ASSERT (tcb != get_idle_tcb_c ());

    sktcb = &tcb->sched_state.base;
    prio = hs_sched_get_priority (sktcb);
    stride = hs_sched_get_stride (sktcb);

    if (queue_state_is_set (&tcb->queue_state, QUEUE_STATE_READY))
    {
	if (hs_sched_get_pass (sktcb) < self->global_pass)
	    hs_sched_set_pass (sktcb, self->global_pass + stride);
	return;
    }

    hs_sched_set_pass (sktcb, self->global_pass + stride);

    if (head)
	ENQUEUE_LIST_HEAD (self->prio_queue[prio], tcb, sched_state.base.ready_list);
    else
	ENQUEUE_LIST_TAIL (self->prio_queue[prio], tcb, sched_state.base.ready_list);

    queue_state_set (&tcb->queue_state, QUEUE_STATE_READY);
    if ((s16_t) prio > self->max_prio)
	self->max_prio = (s16_t) prio;
    self->count++;
}

/* prio_queue_t::dequeue -- returns the remaining count, as the C++ did */
static bool prio_queue_dequeue (prio_queue_t *self, tcb_t *tcb)
{
    prio_t prio;

    ASSERT (tcb);
    ASSERT (tcb != get_idle_tcb_c ());

    prio = hs_sched_get_priority (&tcb->sched_state.base);

    if (queue_state_is_set (&tcb->queue_state, QUEUE_STATE_READY))
    {
	DEQUEUE_LIST (self->prio_queue[prio], tcb, sched_state.base.ready_list);
	queue_state_clear (&tcb->queue_state, QUEUE_STATE_READY);
	self->count--;
    }
    return self->count != 0;
}

/* prio_queue_t::init */
static void prio_queue_init (prio_queue_t *self, tcb_t *dtcb)
{
    int i;

    for (i = 0; i < MAX_PRIORITY; i++)
	self->prio_queue[i] = NULL;

    self->global_pass = 0;
    self->max_prio = -1;
    self->refcnt = 0;
    self->domain_tcb = dtcb;
    self->depth = 0;
    self->count = 0;

    ON_CONFIG_SMP (self->cpu_link = self->cpu_head = NULL);
    prio_queue_reset_period_cycles (self, get_cpu_cycles ());
}

/* hs_scheduler_t::enqueue_ready -- walks up the domain chain */
static void sched_enqueue_ready (scheduler_t *self, tcb_t *tcb, bool head)
{
    (void) self;
    while (!tid_is_idle (tcb_get_global_id (tcb)))
    {
	prio_queue_t *pq;

	ASSERT (tcb);
	pq = hs_sched_get_prio_queue (&tcb->sched_state.base);
	ASSERT (pq);

	prio_queue_enqueue (pq, tcb, head);
	tcb = prio_queue_get_domain_tcb (pq);
    }
}

/* hs_scheduler_t::dequeue_ready -- stops as soon as a queue is still occupied */
static void sched_dequeue_ready (scheduler_t *self, tcb_t *tcb)
{
    (void) self;
    ASSERT (tcb);
    while (!tid_is_idle (tcb_get_global_id (tcb)))
    {
	prio_queue_t *pq;

	ASSERT (tcb);
	pq = hs_sched_get_prio_queue (&tcb->sched_state.base);
	ASSERT (pq);

	if (prio_queue_dequeue (pq, tcb))
	    break;
	tcb = prio_queue_get_domain_tcb (pq);
    }
}

/* hs_scheduler_t::enqueue_timeout / dequeue_timeout */
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

/* tcb_t::get/set_preempt_flags (via the generic UTCB accessors) */
INLINE preempt_flags_t tcb_preempt_flags (tcb_t *self)
{ preempt_flags_t f; f.raw = utcb_get_preempt_flags (self->utcb); return f; }
INLINE void tcb_preempt_flags_set (tcb_t *self, preempt_flags_t f)
{ utcb_set_preempt_flags (self->utcb, f.raw); }

/*
 * Walk two threads up their domain chains until both sit in the same prio
 * queue, so that priorities and passes are compared within one domain.  Shared
 * by check_dispatch_thread and delay_preemption, which did it identically.
 */
static void hs_common_ancestor (hs_sched_ktcb_t **s, prio_queue_t **sq,
				hs_sched_ktcb_t **d, prio_queue_t **dq)
{
    while (*sq != *dq)
    {
	ASSERT (*sq && *dq);
	ASSERT (prio_queue_get_domain_tcb (*sq) && prio_queue_get_domain_tcb (*dq));

	if (prio_queue_get_depth (*sq) >= prio_queue_get_depth (*dq))
	{
	    *s = &prio_queue_get_domain_tcb (*sq)->sched_state.base;
	    *sq = hs_sched_get_prio_queue (*s);
	}
	if (prio_queue_get_depth (*sq) <= prio_queue_get_depth (*dq))
	{
	    *d = &prio_queue_get_domain_tcb (*dq)->sched_state.base;
	    *dq = hs_sched_get_prio_queue (*d);
	}
    }
}

/* hs_sched_ktcb_t::delay_preemption */
bool hs_sched_delay_preemption (hs_sched_ktcb_t *self, tcb_t *dtcb)
{
    tcb_t *stcb = to_tcb (self);
    hs_sched_ktcb_t *ssktcb, *dsktcb;
    prio_queue_t *sprio_queue, *dprio_queue;
    bool ret;

    if (stcb == dtcb)
	return true;

    ssktcb = &stcb->sched_state.base;
    dsktcb = &dtcb->sched_state.base;
    sprio_queue = hs_sched_get_prio_queue (ssktcb);
    dprio_queue = hs_sched_get_prio_queue (dsktcb);

    hs_common_ancestor (&ssktcb, &sprio_queue, &dsktcb, &dprio_queue);

    TRACEPOINT (SCHEDULE_DETAILS, "dpm %t (d %t prio %x pass %d) - %t (d %t prio %x pass %d)\n",
		stcb, to_tcb (ssktcb), hs_sched_get_priority (ssktcb), (word_t) hs_sched_get_pass (ssktcb),
		dtcb, to_tcb (dsktcb), hs_sched_get_priority (dsktcb), (word_t) hs_sched_get_pass (dsktcb));

    if (ssktcb->sensitive_prio < dsktcb->priority)
	ret = false;
    else
	ret = (self->current_max_delay > 0);

    TRACEPOINT (SCHEDULE_DETAILS, "dpm %ssuccessful %t (d %t send prio %x delay %dus) - %t (d %t prio %x)\n",
		(ret ? "un" : ""),
		stcb, to_tcb (ssktcb), ssktcb->sensitive_prio, self->current_max_delay,
		dtcb, to_tcb (dsktcb), hs_sched_get_priority (dsktcb));

    return ret;
}

/* hs_scheduler_t::check_dispatch_thread */
static bool check_dispatch_thread (tcb_t *stcb, tcb_t *dtcb)
{
    hs_sched_ktcb_t *ssktcb, *dsktcb;
    prio_queue_t *sprio_queue, *dprio_queue;
    preempt_flags_t pf;

    ASSERT (stcb != dtcb);

    ssktcb = &stcb->sched_state.base;
    dsktcb = &dtcb->sched_state.base;

    pf = tcb_preempt_flags (stcb);
    if (EXPECT_FALSE (preempt_flags_is_delayed (&pf) && hs_sched_get_maximum_delay (ssktcb)))
	return !hs_sched_delay_preemption (ssktcb, dtcb);

    sprio_queue = hs_sched_get_prio_queue (ssktcb);
    dprio_queue = hs_sched_get_prio_queue (dsktcb);
    ASSERT (sprio_queue && dprio_queue);

    hs_common_ancestor (&ssktcb, &sprio_queue, &dsktcb, &dprio_queue);

    TRACEPOINT (SCHEDULE_DETAILS, "cdt %t (d %t prio %x pass %d) - %t (d %t prio %x pass %d)\n",
		stcb, to_tcb (ssktcb), hs_sched_get_priority (ssktcb), (word_t) hs_sched_get_pass (ssktcb),
		dtcb, to_tcb (dsktcb), hs_sched_get_priority (dsktcb), (word_t) hs_sched_get_pass (dsktcb));

    if (hs_sched_get_priority (ssktcb) == hs_sched_get_priority (dsktcb))
	return hs_sched_get_pass (ssktcb) < hs_sched_get_pass (dsktcb);
    return hs_sched_get_priority (ssktcb) < hs_sched_get_priority (dsktcb);
}

/* hs_sched_ktcb_t::account_pass */
void hs_sched_account_pass (hs_sched_ktcb_t *self)
{
    tcb_t *tcb = to_tcb (self);

    while (!tid_is_idle (tcb_get_global_id (tcb)))
    {
	hs_sched_ktcb_t *sktcb = &tcb->sched_state.base;

	ASSERT (tcb_is_local_cpu (tcb));
	TRACEPOINT (SCHEDULE_DETAILS, "account pass %t %llu += %d\n",
		    tcb, hs_sched_get_pass (sktcb), hs_sched_get_stride (sktcb));

	hs_sched_set_pass (sktcb, hs_sched_get_pass (sktcb) + hs_sched_get_stride (sktcb));

	/* account for the parent queue too */
	tcb = prio_queue_get_domain_tcb (hs_sched_get_prio_queue (sktcb));
    }
}

/* hs_sched_ktcb_t::set_prio_queue */
void hs_sched_set_prio_queue (hs_sched_ktcb_t *self, prio_queue_t *q)
{
    /* TODO: delete priority subdomains when they become empty. */
    self->prio_queue = q;

    if (hs_flag_is_set (self, HS_SCHED_FLAG_IS_SCHEDULE_DOMAIN))
	prio_queue_set_depth (hs_sched_get_domain_prio_queue (self), self->prio_queue);
}

/* hs_sched_ktcb_t::migrate_prio_queue */
void hs_sched_migrate_prio_queue (hs_sched_ktcb_t *self, prio_queue_t *nq)
{
    tcb_t *tcb = to_tcb (self);
    scheduler_t *sched = cur_sched ();
    thread_state_t st;

    ASSERT (tcb_get_cpu (tcb) == get_current_cpu ());
    ASSERT (tcb_get_cpu (prio_queue_get_domain_tcb (nq)) == get_current_cpu ());

    sched_dequeue_ready (sched, tcb);
    hs_sched_set_prio_queue (self, nq);

    st.state = tcb_get_state (tcb);
    if (thread_state_is_runnable (&st))
	sched_enqueue_ready (sched, tcb, false);
}

/* prio_queue_t::prio_tickets.  The C++ computed this in float; a kernel has no
   FPU state to spare, so the ratios are accumulated in fixed point (1/1024) --
   see notes §104. */
static word_t prio_queue_prio_tickets (prio_queue_t *self, tcb_t *search,
				       word_t *search_tickets)
{
    tcb_t *start, *tcb;
    word_t stride_sum = 0;
    word_t tot_tickets = 0;

    ASSERT (search);
    start = prio_queue_get (self, hs_sched_get_priority (&search->sched_state.base));
    tcb = start;

    do {
	ASSERT (tcb);
	stride_sum += hs_sched_get_stride (&tcb->sched_state.base);
	tcb = tcb->sched_state.base.ready_list.next;
    } while (tcb != start);

    *search_tickets = 0;
    do {
	word_t stride = hs_sched_get_stride (&tcb->sched_state.base);
	/* tickets = stride_sum / stride, in 1/1024ths */
	word_t tickets = stride ? ((stride_sum << 10) / stride) : 0;
	tot_tickets += tickets;
	if (search == tcb)
	    *search_tickets = tickets >> 10;
	tcb = tcb->sched_state.base.ready_list.next;
    } while (tcb != start);

    return tot_tickets >> 10;
}


/**********************************************************************
 *
 *  prio_queue_t domain management (were out-of-line in schedule.cc)
 *
 **********************************************************************/

prio_queue_t * prio_queue_add_prio_domain (prio_queue_t *self, schedule_ctrl_t prio_control)
{
    word_t num_cpus = cpu_count;
    whole_tcb_t *domain_tcbs;
    cpuid_t cpu;

    ASSERT ((sizeof (tcb_t) + sizeof (prio_queue_t)) < sizeof (whole_tcb_t));
    ASSERT (prio_queue_get_depth (self) < sizeof (word_t));

    /* allocate dummy tcbs for the scheduling domain */
    domain_tcbs = (whole_tcb_t *) kmem_alloc (&kmem, kmem_sched,
					      sizeof (whole_tcb_t) * num_cpus);
    if (domain_tcbs == NULL)
	return NULL;

    /* initialize a dummy tcb for the domain, which serves as a schedulable entity */
    for (cpu = 0; cpu < num_cpus; cpu++)
    {
	tcb_t *domain_tcb = (tcb_t *) &domain_tcbs[cpu];
	prio_queue_t *domain_queue;

	tcb_create_inactive (domain_tcb, threadid_nilthread (), threadid_nilthread (),
			     sktcb_user);
	hs_flag_add (&domain_tcb->sched_state.base, HS_SCHED_FLAG_IS_SCHEDULE_DOMAIN);

	/* use the prio queue within the TCB's unused stack area */
	domain_queue = hs_sched_get_domain_prio_queue (&domain_tcb->sched_state.base);
	prio_queue_init (domain_queue, domain_tcb);

#if defined(CONFIG_SMP)
	/* link the domain queues for each CPU; point the head at CPU 0 */
	domain_queue->cpu_head =
	    hs_sched_get_domain_prio_queue (&((tcb_t *) domain_tcbs)->sched_state.base);
	/* point the link at the domain queue for the next CPU */
	domain_queue->cpu_link =
	    hs_sched_get_domain_prio_queue (&((tcb_t *) &domain_tcbs[(cpu + 1) % num_cpus])->sched_state.base);
#endif

	if (prio_control.raw != 0)
	{
	    /* set stride and priority of the domain */
	    if (prio_control.stride)
		hs_sched_set_stride (&domain_tcb->sched_state.base, (word_t) prio_control.stride);
	    /* prio is a signed 9 bit field filled from the schedule syscall's
	       message registers.  Without this check a negative value narrows
	       to prio_t and lands on ROOT_PRIORITY.  Same guard as the other
	       set_priority() call site, in sched_commit_schedule_parameters. */
	    if ((word_t) prio_control.prio <= MAX_PRIORITY)
		hs_sched_set_priority (&domain_tcb->sched_state.base, (prio_t) prio_control.prio);
#if defined(CONFIG_X_EVT_LOGGING)
	    if ((word_t) prio_control.logid > 0 &&
		(word_t) prio_control.logid < MAX_LOGIDS)
		domain_tcb->sched_state.logid = prio_control.logid;
#endif
	}
    }

    /* install the tcb into the parent prio queue.  Must be done after all
       other cpu state is finalized. */
    for (cpu = 0; cpu < num_cpus; cpu++)
    {
	tcb_t *domain_tcb = (tcb_t *) &domain_tcbs[cpu];

	ASSERT (hs_flag_is_set (&domain_tcb->sched_state.base,
				     HS_SCHED_FLAG_IS_SCHEDULE_DOMAIN));

	hs_sched_set_prio_queue (&domain_tcb->sched_state.base, self);
	if (cpu != get_current_cpu ())
	    tcb_migrate_to_processor (domain_tcb, cpu);

	TRACEPOINT (SCHEDULE_PRIO_DOMAIN, "new prio domain tcb %t, cpu %d, prio %d, stride %d\n",
		    domain_tcb, cpu,
		    hs_sched_get_priority (&domain_tcb->sched_state.base),
		    hs_sched_get_stride (&domain_tcb->sched_state.base));
    }

    return hs_sched_get_domain_prio_queue (
	&((tcb_t *) &domain_tcbs[get_current_cpu ()])->sched_state.base);
}

prio_queue_t * prio_queue_domain_partner (prio_queue_t *self, cpuid_t cpu)
{
    ASSERT (self->domain_tcb);

    if (tcb_get_cpu (self->domain_tcb) == cpu)
	return self;

#if defined(CONFIG_SMP)
    {
	prio_queue_t *partner_queue = self->cpu_head;

	ASSERT (partner_queue);
	/* look for the domain prio queue for the target CPU */
	while (tcb_get_cpu (partner_queue->domain_tcb) != cpu)
	{
	    partner_queue = partner_queue->cpu_link;
	    ASSERT (partner_queue);
	}
	return partner_queue;
    }
#else
    return NULL;
#endif
}


/**********************************************************************
 *
 *  scheduler_t methods (were INLINEs in sched-hs/schedule_functions.h)
 *
 **********************************************************************/

u64_t sched_get_current_time (void)
{
    return hs_sched_current_time;
}

void sched_set_accounted_tcb (tcb_t *tcb)
{
    scheduler_t *self = cur_sched ();
    u64_t now;

    ASSERT (tcb && tcb_is_local_cpu (tcb));

    if (self->__base.scheduled_tcb == tcb)
	return;

    now = get_cpu_cycles ();

    prio_queue_end_timeslice (self->__base.scheduled_queue, now);
    hs_sched_set_timeslice (&self->__base.scheduled_tcb->sched_state.base,
			    self->__base.current_timeslice);

    if (hs_sched_get_init_maximum_delay (&tcb->sched_state.base) <
	hs_sched_get_maximum_delay (&tcb->sched_state.base))
    {
	word_t delta = (word_t) (get_timestamp () - self->__base.delayed_preemption_start);
	word_t maxd = hs_sched_get_maximum_delay (&self->__base.scheduled_tcb->sched_state.base);

	if (delta > maxd)
	{
	    delta = maxd;
	    TRACEF ("blocked thread long delay\n");
	}
	hs_sched_add_delay_penalty (&self->__base.scheduled_tcb->sched_state.base, (u16_t) delta);
    }

    self->__base.scheduled_tcb = tcb;
    self->__base.scheduled_queue = hs_sched_get_prio_queue (&tcb->sched_state.base);
    prio_queue_start_timeslice (self->__base.scheduled_queue, now);

    TRACEPOINT (SCHEDULE_DETAILS, "sat %t (q %t pass %llu now %llu))\n",
		self->__base.scheduled_tcb, self->__base.scheduled_queue,
		hs_sched_get_pass (&self->__base.scheduled_tcb->sched_state.base), now);
}

tcb_t * sched_get_accounted_tcb (void)
{
    return cur_sched ()->__base.scheduled_tcb;
}

/**
 * selects the next runnable thread and activates it.
 * @return true if a runnable thread was found, false otherwise
 */
static bool do_schedule_current (void)
{
    scheduler_t *self = cur_sched ();
    tcb_t *tcb = sched_find_next_thread (self, sched_root_prio_queue (self));
    tcb_t *current = get_current_tcb ();

    ASSERT (tcb);
    ASSERT (current);

    /* do not switch to ourself */
    if (tcb == current)
	return false;

    if (current != get_idle_tcb_c ())
	sched_enqueue_ready (self, current, false);

    /* the newly selected thread gets accounted */
    sched_set_accounted_tcb (tcb);
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
	sched_set_accounted_tcb (dest);

	/* make sure we are in the ready queue */
	if (FLAG_IS_SET (flags, sched_c2r_flag) && current != get_idle_tcb_c ())
	    sched_enqueue_ready (self, current, true);

	tcb_switch_to (current, dest);
	return true;
    }
    else
    {
	ASSERT (dest);
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
    bool ret;

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

    sched_set_accounted_tcb (dest);

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
    threadid_t scheduler_tid, gid;
    tcb_t *scheduler_tcb;

    ASSERT (dest_tcb);

    if (is_privileged_space_c (tcb_get_space (tcb)))
	return true;

    /* are we in the same address space as the scheduler of the thread? */
    scheduler_tid = sched_ktcb_get_scheduler (&dest_tcb->sched_state);
    scheduler_tcb = tcb_get_tcb (scheduler_tid);

    gid = tcb_get_global_id (tcb);
    if (threadid_get_raw (&gid) != threadid_get_raw (&scheduler_tid) ||
	(tcb_get_space (tcb) != tcb_get_space (scheduler_tcb)))
	return false;

    return true;
}

word_t sched_check_schedule_parameters (tcb_t *scheduler, schedule_req_t *req)
{
    if (req->preemption_control.raw != 0)
    {
	/* Extended HS schedule control
	 * control &  1 -> new domain
	 * control &  2 -> migrate domain
	 * control &  4 -> retrieve tickets of dest
	 * control &  8 -> reset period cycles of current queue and retrieve utilization
	 * control & 16 -> set stride
	 */
	if (req->preemption_control.hs_extended)
	{
	    tcb_t *domain_tcb;
	    prio_queue_t *domain_queue;

	    /* must be privileged */
	    if (!is_privileged_space_c (tcb_get_space (scheduler)))
		return ENO_PRIVILEGE;

	    if ((req->preemption_control.hs_extended_ctrl & 0xc) ==
		req->preemption_control.hs_extended_ctrl)
		return EOK;

	    domain_tcb = tcb_get_tcb (req->time_control.tid);

	    /* target and domain must be different */
	    if (tcb_get_global_id (domain_tcb).raw != req->time_control.tid.raw ||
		hs_sched_get_prio_queue (&domain_tcb->sched_state.base) == NULL)
		return EINVALID_THREAD;

	    domain_queue = hs_sched_get_prio_queue (&domain_tcb->sched_state.base);
	    domain_queue = prio_queue_domain_partner (domain_queue, get_current_cpu ());

	    if (!domain_queue)
		return EINVALID_PARAM;

	    TRACE_SCHEDULE_DETAILS ("control %x, domain tid: %t queue %p, first dest tid: %t, prioctrl %x\n",
				    req->preemption_control.hs_extended_ctrl,
				    domain_tcb, domain_queue, req->tcb, req->prio_control.raw);

	    if ((req->preemption_control.hs_extended_ctrl & 0x3) && domain_tcb == req->tcb)
		return EINVALID_THREAD;

	    if ((req->preemption_control.hs_extended_ctrl & 0x10) &&
		!prio_queue_get_domain_tcb (domain_queue))
		return EINVALID_PARAM;

	    req->time_control.raw = (word_t) domain_tcb;
	    req->processor_control.raw = (word_t) domain_queue;

	    return EOK;
	}

	if ((prio_t) req->prio_control.sensitive_prio >
	    hs_sched_get_priority (&scheduler->sched_state.base))
	    return ENO_PRIVILEGE;
    }

    if (req->prio_control.raw != 0 &&
	req->prio_control.prio > hs_sched_get_priority (&scheduler->sched_state.base) &&
	!is_privileged_space_c (tcb_get_space (scheduler)))
	return ENO_PRIVILEGE;

    if (req->time_control.raw != 0 &&
	(!(req->time_control.total_quantum.time.type == 0) ||
	 !(req->time_control.timeslice.time.type == 0)))
	return EINVALID_THREAD;

    if (req->processor_control.raw != 0)
    {
	/* can't move domain tcbs */
	if (hs_flag_is_set (&req->tcb->sched_state.base,
				 HS_SCHED_FLAG_IS_SCHEDULE_DOMAIN))
	    return EINVALID_THREAD;
    }

    return EOK;
}

void sched_commit_schedule_parameters (schedule_req_t *req)
{
    if (req->preemption_control.raw != 0)
    {
	if (req->preemption_control.hs_extended)
	{
	    hs_extended_schedule (cur_sched (), req);
	    return;
	}

	hs_sched_init_maximum_delay (&req->tcb->sched_state.base,
				     (u16_t) req->preemption_control.max_delay);

	/* only set sensitive prio if _at most_ equal to current prio */
	if ((prio_t) req->preemption_control.sensitive_prio >=
	    hs_sched_get_priority (&req->tcb->sched_state.base))
	    hs_sched_set_sensitive_prio (&req->tcb->sched_state.base,
					 (prio_t) req->preemption_control.sensitive_prio);
    }

    if (req->prio_control.raw != 0)
    {
	sched_deschedule (req->tcb);

	if ((word_t) req->prio_control.prio <= MAX_PRIORITY &&
	    (word_t) req->prio_control.prio !=
	    hs_sched_get_priority (&req->tcb->sched_state.base))
	    hs_sched_set_priority (&req->tcb->sched_state.base, (prio_t) req->prio_control.prio);
#if defined(CONFIG_X_EVT_LOGGING)
	if ((word_t) req->prio_control.logid > 0 &&
	    (word_t) req->prio_control.logid < MAX_LOGIDS)
	    req->tcb->sched_state.logid = req->prio_control.logid;
#endif
	if (req->prio_control.stride > 0)
	    hs_sched_set_stride (&req->tcb->sched_state.base, (word_t) req->prio_control.stride);

	do_schedule (req->tcb, sched_current);
    }

    if (req->processor_control.raw != 0)
	tcb_migrate_to_processor (req->tcb, req->processor_control.processor);

    if (req->time_control.raw != 0)
    {
	hs_sched_init_timeslice (&req->tcb->sched_state.base, req->time_control.timeslice);
	hs_sched_set_total_quantum (&req->tcb->sched_state.base,
				    time_get_microseconds (&req->time_control.total_quantum));
    }
}

word_t sched_return_schedule_parameter (word_t num, schedule_req_t *req)
{
    if (!req->tcb) return 0;

    if (req->preemption_control.raw != 0 && req->preemption_control.hs_extended)
    {
	if (req->preemption_control.hs_extended_ctrl & 4)
	{
	    prio_queue_t *queue = hs_sched_get_prio_queue (&req->tcb->sched_state.base);
	    word_t current_tickets, tickets;

	    if (queue != sched_root_prio_queue (cur_sched ()))
	    {
		/* look into the priority queue which contains the domain of
		   the current thread */
		req->tcb = prio_queue_get_domain_tcb (queue);
		queue = hs_sched_get_prio_queue (&req->tcb->sched_state.base);
	    }

	    tickets = prio_queue_prio_tickets (queue, req->tcb, &current_tickets);
	    return (num == 0) ? tickets : current_tickets;
	}

	if (req->preemption_control.hs_extended_ctrl & 8)
	{
	    prio_queue_t *queue = cur_sched ()->__base.scheduled_queue;
	    period_cycles_t period_cycles = prio_queue_get_period_cycles (queue);

	    ASSERT (prio_queue_get_domain_tcb (queue));
#if defined(CONFIG_SMP)
	    queue = queue->cpu_head;
	    do {
		if (tcb_get_cpu (prio_queue_get_domain_tcb (queue)) != get_current_cpu ())
		    period_cycles += prio_queue_get_period_cycles (queue);
		queue = queue->cpu_link;
	    } while (queue != queue->cpu_head);
#endif
	    if (num == 0)
		return (word_t) (period_cycles * 1000 / prio_queue_get_poll_window (queue));
	    return (word_t) period_cycles;
	}

	return 1;
    }

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
		({ WARNING("invalid state (%x)\n", (word_t) state.state); (word_t) 0;}));
    }
    else
    {
	word_t rem_ts = (word_t) hs_sched_get_timeslice (&req->tcb->sched_state.base);
	word_t rem_tq = (word_t) hs_sched_get_total_quantum (&req->tcb->sched_state.base);
	return (rem_ts << 16) | (rem_tq & 0xffff);
    }
}

bool sched_idle_hlt (void)
{
    TRACEPOINT (SCHEDULE_IDLE, "idle loop by user hlt");
    if (is_privileged_space_c (tcb_get_space (get_current_tcb ())))
    {
	processor_sleep ();
	return true;
    }
    return false;
}


/**********************************************************************
 *
 *  the policy methods (were out-of-line in schedule.cc)
 *
 **********************************************************************/

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
 *
 * returns the selected tcb; if no runnable thread is available, the idle tcb
 * is returned.
 */
static tcb_t * sched_find_next_thread (scheduler_t *self, prio_queue_t *prio_queue)
{
    s16_t prio;

    ASSERT (prio_queue);

#if defined(CONFIG_SMP)
    /* requeue threads which got activated by other CPUs */
    hs_smp_requeue (self, false);
#endif

    for (prio = MAX_PRIORITY; prio >= 0; prio--)
    {
	/* proportional share stride scheduling search */
	tcb_t *search_tcb = NULL;
	tcb_t *tcb = prio_queue_get (prio_queue, (prio_t) prio);
	tcb_t *return_tcb = NULL;

	while (tcb && !search_tcb)
	{
	    while (tcb)
	    {
		thread_state_t st; st.state = tcb_get_state (tcb);

		if (thread_state_is_runnable (&st) ||
		    hs_flag_is_set (&tcb->sched_state.base,
					 HS_SCHED_FLAG_IS_SCHEDULE_DOMAIN))
		{
		    if (!search_tcb ||
			(hs_sched_get_pass (&tcb->sched_state.base) <
			 hs_sched_get_pass (&search_tcb->sched_state.base)))
			search_tcb = tcb;

		    TRACEPOINT (SCHEDULE_DETAILS, "fnt search %t (pass %llu) tcb %t (pass %U)\n",
				search_tcb, hs_sched_get_pass (&search_tcb->sched_state.base),
				tcb, hs_sched_get_pass (&tcb->sched_state.base));

		    tcb = tcb->sched_state.base.ready_list.next;

		    if (tcb == prio_queue_get (prio_queue, (prio_t) prio))
			tcb = NULL;	/* we wrapped around the list */
		}
		else
		{
		    /* dequeue a blocked thread */
		    tcb_t *next_tcb = tcb->sched_state.base.ready_list.next;
		    prio_queue_dequeue (prio_queue, tcb);
		    tcb = (tcb == next_tcb) ? NULL : next_tcb;
		}
	    }

	    return_tcb = search_tcb;
	    if (search_tcb &&
		hs_flag_is_set (&search_tcb->sched_state.base,
				     HS_SCHED_FLAG_IS_SCHEDULE_DOMAIN))
	    {
		/* subdomain: find a schedulable thread inside it */
		prio_queue_t *domain_prio_queue;
		tcb_t *domain_tcb;

		TRACEPOINT (SCHEDULE_PRIO_DOMAIN, "fnt prio domain tcb %t, prio %d, stride %d\n",
			    search_tcb, prio,
			    hs_sched_get_stride (&search_tcb->sched_state.base));

		domain_prio_queue =
		    hs_sched_get_domain_prio_queue (&search_tcb->sched_state.base);
		domain_tcb = sched_find_next_thread (self, domain_prio_queue);

		if (domain_tcb == get_idle_tcb_c ())
		{
		    /* nothing found in the subdomain, so dequeue */
		    prio_queue_dequeue (prio_queue, search_tcb);

		    /* prepare to restart the search at the current prio */
		    search_tcb = NULL;
		    tcb = prio_queue_get (prio_queue, (prio_t) prio);
		}
		else
		    return_tcb = domain_tcb;
	    }
	}

	if (search_tcb)
	{
	    /* we found a thread!  Account for it, whether it is a real thread
	       or a subdomain. */
	    prio_queue_set_global_pass (prio_queue,
					hs_sched_get_pass (&search_tcb->sched_state.base));
	    prio_queue_set (prio_queue, (prio_t) prio, search_tcb);
	    prio_queue->max_prio = prio;
	    /* now give the scheduler the newly scheduled thread's timeslice */
	    self->__base.current_timeslice =
		hs_sched_get_timeslice (&return_tcb->sched_state.base);
	    self->__base.delayed_preemption_start = get_timestamp ();
	    /* return the real thread (found in this domain or a subdomain) */
	    return return_tcb;
	}
    }

    /* if we can't find a schedulable thread - switch to idle */
    prio_queue->max_prio = -1;
    return get_idle_tcb_c ();
}

#if defined(CONFIG_SMP)
void do_xcpu_domain_reset_period (cpu_mb_entry_t *entry)
{
    prio_queue_t *queue = (prio_queue_t *) entry->param[0];
    ASSERT (queue);
    prio_queue_reset_period_cycles (queue, get_cpu_cycles ());
}

static void do_xcpu_domain_stride (cpu_mb_entry_t *entry)
{
    tcb_t *domain_tcb = entry->tcb;
    word_t stride = entry->param[0];
    scheduler_t *sched = cur_sched ();

    ASSERT (hs_flag_is_set (&domain_tcb->sched_state.base,
				 HS_SCHED_FLAG_IS_SCHEDULE_DOMAIN));
    sched_dequeue_ready (sched, domain_tcb);
    hs_sched_set_stride (&domain_tcb->sched_state.base, stride);
    sched_enqueue_ready (sched, domain_tcb, false);
}
#endif

/* was hs_scheduler_t::policy_scheduler_init; called from api/v4/schedule.c */
void policy_scheduler_init (scheduler_t *self)
{
    cpuid_t cpu = get_current_cpu ();

    self->__base.wakeup_list = NULL;
    self->__base.scheduled_tcb = NULL;
    self->__base.scheduled_queue = NULL;

    prio_queue_init (&self->__base.root_prio_queue,
		     (tcb_t *) get_on_cpu_c (cpu, get_idle_tcb_c ()));
#if defined(CONFIG_SMP)
    self->__base.root_prio_queue.cpu_head =
	hs_scheduler_get_prio_queue (&((scheduler_t *) get_on_cpu_c (0, cur_sched ()))->__base);
    self->__base.root_prio_queue.cpu_link =
	hs_scheduler_get_prio_queue (
	    &((scheduler_t *) get_on_cpu_c ((cpuid_t) ((cpu + 1) % cpu_count),
					    cur_sched ()))->__base);
#endif
}

static void hs_extended_schedule (scheduler_t *self, schedule_req_t *req)
{
    /* Extended HS scheduler control:
     * params:
     *  dest                = target
     *  time_control        = domain
     *  processor_control   = domain queue
     *  prio_control        = prio
     */
    word_t control = req->preemption_control.hs_extended_ctrl;
    tcb_t *dest = req->tcb;
    prio_queue_t *domain_queue = (prio_queue_t *) req->processor_control.raw;

    TRACE_SCHEDULE_DETAILS("extended HS schedule ctrl %x, domain: %x q %p, dest: %t, prio %d stride %d\n",
			   control, req->time_control.raw, domain_queue, dest,
			   req->prio_control.prio, req->prio_control.stride);

    /* target thread is the thread to put into a subdomain of the parent
       scheduling domain */
    if (control & 1)
    {
	/* create a new domain as a child of the domain */
	prio_queue_t *sub_queue = prio_queue_add_prio_domain (domain_queue, req->prio_control);
	ASSERT (sub_queue && prio_queue_get_domain_tcb (sub_queue));
	ASSERT (tcb_is_local_cpu (prio_queue_get_domain_tcb (sub_queue)));

	TRACE_SCHEDULE_DETAILS("new domain %p for %t prio %d stride %d\n", sub_queue, dest,
			       req->prio_control.prio, req->prio_control.stride);

	/* move the target thread to the new domain */
	hs_sched_migrate_prio_queue (&dest->sched_state.base, sub_queue);
	return;
    }
    else if (control & 2)
    {
	TRACE_SCHEDULE_DETAILS("migrate %t to domain queue %x\n", dest, domain_queue);
	hs_sched_migrate_prio_queue (&dest->sched_state.base, domain_queue);
    }

    if (control & 8)
    {
	prio_queue_t *queue = self->__base.scheduled_queue;

	TRACE_SCHEDULE_DETAILS( "reset period cycles queue %p, cpu %d\n",
				self->__base.scheduled_queue, get_current_cpu());
	UNTESTED();

	prio_queue_reset_period_cycles (queue, get_cpu_cycles ());

#if defined(CONFIG_SMP)
	queue = queue->cpu_head;
	do {
	    cpuid_t cpu;
	    ASSERT (queue);
	    ASSERT (prio_queue_get_domain_tcb (queue));
	    cpu = tcb_get_cpu (prio_queue_get_domain_tcb (queue));
	    if (cpu != get_current_cpu ())
		xcpu_request_c (cpu, do_xcpu_domain_reset_period, NULL, (word_t) queue);
	    queue = queue->cpu_link;
	} while (queue != queue->cpu_head);
#endif
	return;
    }

    if (control & 16)
    {
	tcb_t *cpu_domain_tcb = prio_queue_get_domain_tcb (domain_queue);

	if (req->prio_control.stride)
	{
	    TRACE_SCHEDULE_DETAILS( "restride queue %p domain tcb %t domain cpu tcb %t, stride %u, cpu %d\n",
				    domain_queue, req->time_control.raw, cpu_domain_tcb,
				    req->prio_control.stride, get_current_cpu() );

	    sched_dequeue_ready (self, cpu_domain_tcb);
	    hs_sched_set_stride (&cpu_domain_tcb->sched_state.base, (word_t) req->prio_control.stride);
	    sched_enqueue_ready (self, cpu_domain_tcb, false);

#if defined(CONFIG_SMP)
	    {
		prio_queue_t *queue = domain_queue->cpu_head;
		do {
		    cpuid_t cpu;
		    ASSERT (queue);
		    ASSERT (prio_queue_get_domain_tcb (queue));
		    cpu = tcb_get_cpu (prio_queue_get_domain_tcb (queue));

		    if (cpu != get_current_cpu ())
			xcpu_request_c (cpu, do_xcpu_domain_stride,
					prio_queue_get_domain_tcb (queue),
					(word_t) req->prio_control.stride);

		    queue = queue->cpu_link;
		} while (queue != domain_queue->cpu_head);
	    }
#endif
	}
    }
}

/**
 * sends preemption IPC to the scheduler thread that the total quantum
 * has expired
 */
static void hs_total_quantum_expired (tcb_t *tcb)
{
    TRACEPOINT (TOTAL_QUANTUM_EXPIRED, "total quantum expired for %t\n", tcb);
    enter_kdebug ("total quantum IPC unimplemented");
    UNIMPLEMENTED();
    /* Total quantum IPC is an open point.  The expiration may happen in the
     * wrong thread context and thus we have to tunnel the IPC.  Also it may
     * happen in the middle of a long IPC which leads to nesting of four.
     * Disabled for the time being.
     */
}

static tcb_t * hs_parse_wakeup_queues (scheduler_t *self, tcb_t *current)
{
    tcb_t *highest_wakeup;
    tcb_t *tcb;
    bool list_head_changed;

    if (!self->__base.wakeup_list)
	return current;

    highest_wakeup = current;
    tcb = self->__base.wakeup_list;

    do
    {
	sched_ktcb_t *sched_state = &tcb->sched_state;
	thread_state_t st;

	list_head_changed = false;
	st.state = tcb_get_state (tcb);

	if (hs_sched_has_timeout_expired (&sched_state->base, hs_sched_current_time))
	{
	    tcb_t *tmp;

	    /*
	     * We might try to wake up a thread which is waiting forever.
	     * This can happen if:
	     *
	     *  1) we have issued a timeout IPC, an IPC fast path reply
	     *     occurred before the timeout triggered, and an infinite
	     *     timeout IPC was issued.  Since we're doing fast IPC we will
	     *     do the dequeueing lazily (i.e., doing it now instead).
	     *
	     *  2) this is an xfer timeout, in which case we should let the
	     *     timeout trigger.
	     */
	    if (thread_state_is_waiting_forever (&st) &&
		!tcb_flags_is_set (tcb, TCB_FLAG_HAS_XFER_TIMEOUT))
	    {
		tmp = tcb->sched_state.base.wait_list.next;
		sched_ktcb_cancel_timeout (sched_state);
		list_head_changed = true;
		tcb = tmp;
		TRACEPOINT (SCHEDULE_WAKEUP_TIMEOUT,
			    "remove bogus wakeup timeout wu=%p to=%ld time=%ld tmp=%t\n",
			    tcb, hs_sched_get_timeout (&sched_state->base),
			    (word_t) hs_sched_current_time, tmp);
		continue;
	    }

	    TRACEPOINT (SCHEDULE_WAKEUP_TIMEOUT, "wakeup timeout wu=%t to=%ld time=%ld\n",
			tcb, hs_sched_get_timeout (&sched_state->base),
			(word_t) hs_sched_current_time);

	    /* we have to wake up the guy */
	    if (check_dispatch_thread (highest_wakeup, tcb))
		highest_wakeup = tcb;

	    tmp = tcb->sched_state.base.wait_list.next;
	    sched_ktcb_cancel_timeout (sched_state);
	    list_head_changed = true;

	    tcb_flags_remove (tcb, TCB_FLAG_HAS_XFER_TIMEOUT);

	    if (thread_state_is_sending (&st) || thread_state_is_receiving (&st))
	    {
		/*
		 * The thread must invoke the function which handles the IPC
		 * timeout.  The handler returns directly to user level with an
		 * error code.  As such, no special timeout code is needed in
		 * the IPC path.
		 */
		tcb_notify_word (tcb, handle_ipc_timeout, (word_t) st.state);
	    }

	    /* set it running and enqueue into ready-queue */
	    tcb_set_state (tcb, THREAD_STATE_RUNNING);
	    sched_enqueue_ready (self, tcb, false);

	    tcb = tmp;
	}
	else
	    tcb = tcb->sched_state.base.wait_list.next;
    } while (self->__base.wakeup_list &&
	     (tcb != self->__base.wakeup_list || list_head_changed));

    return highest_wakeup;
}

/**
 * end of timeslice: requeue at the head and account the pass
 */
static void hs_end_of_timeslice (scheduler_t *self, tcb_t *tcb)
{
    spin (74, get_current_cpu ());
    ASSERT (tcb);
    ASSERT (self->__base.scheduled_tcb);
    ASSERT (tcb != get_idle_tcb_c ());	/* the idler never yields */

    sched_enqueue_ready (self, tcb, true);

    if (tcb_is_local_cpu (self->__base.scheduled_tcb))
    {
	hs_sched_ktcb_t *sktcb = &self->__base.scheduled_tcb->sched_state.base;
	hs_sched_set_timeslice (sktcb, self->__base.current_timeslice +
				(s64_t) hs_sched_get_timeslice_length (sktcb));
	hs_sched_account_pass (sktcb);
    }
}

#if defined(CONFIG_SMP)
static void hs_smp_requeue (scheduler_t *self, bool holdlock)
{
    smp_requeue_t *rq = &smp_requeue_lists[get_current_cpu ()];

    if (!smp_requeue_is_empty (rq) || holdlock)
    {
	spinlock_lock (&rq->lock);

	while (!smp_requeue_is_empty (rq))
	{
	    tcb_t *tcb = rq->tcb_list;
	    thread_state_t st;

	    rq->tcb_list = tcb->sched_state.base.requeue;
	    tcb->sched_state.base.requeue = NULL;

	    ASSERT (tcb_get_cpu (tcb) == get_current_cpu ());

	    if (tcb->sched_state.base.requeue_callback)
	    {
		tcb->sched_state.base.requeue_callback (tcb);
		tcb->sched_state.base.requeue_callback = NULL;
	    }
	    else
	    {
		sched_ktcb_cancel_timeout (&tcb->sched_state);
		st.state = tcb_get_state (tcb);
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
	    TRACEF ("curr=%p, %p, CPU#%d != CPU#%d\n", get_current_tcb (),
		    tcb, cpu, tcb_get_cpu (tcb));
	    UNIMPLEMENTED();
	}
	tcb->sched_state.base.requeue = rq->tcb_list;
	rq->tcb_list = tcb;
	spinlock_unlock (&rq->lock);
	smp_xcpu_trigger (cpu);
    }
}

/**
 * re-integrates a thread into the CPUs queues etc.
 */
static void xcpu_integrate_thread (tcb_t *tcb)
{
    sched_ktcb_t *sched_state;
    thread_state_t st;

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

    sched_state = &tcb->sched_state;

    /* the thread may have received an IPC meanwhile, so check whether it is
     * already running again */
    st.state = tcb_get_state (tcb);
    if (thread_state_is_runnable (&st))
	do_schedule (tcb, sched_default);
    else if (hs_sched_get_timeout (&sched_state->base) &&
	     thread_state_is_waiting_with_timeout (&st))
	sched_ktcb_set_timeout_abs (sched_state,
				    sched_get_current_time () +
				    hs_sched_get_timeout (&sched_state->base), true);
}

void sched_move_tcb (tcb_t *tcb, cpuid_t cpu)
{
    scheduler_t *self = cur_sched ();
    prio_queue_t *new_prio_queue;
    thread_state_t st;
    bool need_xcpu;

    tcb_lock (tcb);

    /* it is only necessary to notify the other CPU if the thread is in one of
     * the scheduling queues (wakeup, ready) or is an interrupt thread */
    st.state = tcb_get_state (tcb);
    need_xcpu = thread_state_is_runnable (&st) ||
	(hs_sched_get_timeout (&tcb->sched_state.base) != 0) ||
	tcb_is_interrupt_thread (tcb);

    hs_smp_requeue (self, true);
    ASSERT (tcb->sched_state.base.requeue == NULL);

    if (tcb_get_space (tcb))
	space_move_tcb (tcb_get_space (tcb), tcb, get_current_cpu (), cpu);

    tcb_set_cpu (tcb, cpu);
    spinlock_unlock (&smp_requeue_lists[get_current_cpu ()].lock);

    new_prio_queue = prio_queue_domain_partner (
	hs_sched_get_prio_queue (&tcb->sched_state.base), cpu);
    /* force tcb to use the pass of the target domain */
    hs_sched_set_pass (&tcb->sched_state.base, 0);
    hs_sched_set_prio_queue (&tcb->sched_state.base, new_prio_queue);

    if (need_xcpu)
    {
	tcb->sched_state.base.requeue_callback = xcpu_integrate_thread;
	sched_remote_schedule (tcb);
    }

    tcb_unlock (tcb);

    TRACE_SCHEDULE_DETAILS("move_tcb: %t cpu %d pq %p dtcb %t", tcb, cpu,
			   new_prio_queue, prio_queue_get_domain_tcb (new_prio_queue));
}
#endif /* CONFIG_SMP */

void sched_handle_timer_interrupt (void)
{
    scheduler_t *self = cur_sched ();
    tcb_t *current, *wakeup;
    hs_sched_ktcb_t *cstcb, *sstcb;
    bool reschedule = false;
    preempt_flags_t pf;

    spin (77, get_current_cpu ());

#if defined(CONFIG_DEBUG)
    if (kdebug_check_interrupt ())
	return;
#endif

    if (get_current_cpu () == 0)
    {
	/* update the global time */
	hs_sched_current_time += get_timer_tick_length ();
    }

#if defined(CONFIG_SMP)
    process_xcpu_mailbox ();
    hs_smp_requeue (self, false);
#endif

    current = get_current_tcb ();
    wakeup = hs_parse_wakeup_queues (self, current);

    /* the idle thread schedules itself so no point to do it here.
     * Furthermore, it should not be preempted on end of timeslice etc. */
    if (current == get_idle_tcb_c ())
	return;

    /* tick timeslice */
    self->__base.current_timeslice -= (s64_t) get_timer_tick_length ();

    cstcb = &current->sched_state.base;
    sstcb = &self->__base.scheduled_tcb->sched_state.base;

    /* check for not infinite timeslice and expired */
    if (EXPECT_FALSE (self->__base.current_timeslice <= 0))
    {
	ASSERT (tcb_utcb (current));

	pf = tcb_preempt_flags (current);
	if (preempt_flags_is_delayed (&pf) && hs_sched_delay_preemption (cstcb, wakeup))
	{
	    /* VU: should we give max_delay? */
	    hs_sched_set_timeslice (cstcb, hs_sched_get_timeslice (cstcb) +
				    (s64_t) hs_sched_get_maximum_delay (cstcb));
	    hs_sched_set_maximum_delay (cstcb, 0);
	    pf = tcb_preempt_flags (current);
	    preempt_flags_set_pending (&pf);
	    tcb_preempt_flags_set (current, pf);
	}
	else
	{
	    /* we have end-of-timeslice */
	    TRACEPOINT (TIMESLICE_EXPIRED, "timeslice expired for %t\n", current);

	    pf = tcb_preempt_flags (current);
	    if (EXPECT_FALSE (preempt_flags_is_delayed (&pf)))
	    {
		/* penalize thread for its delayed time slice */
		self->__base.current_timeslice =
		    (s64_t) hs_sched_get_maximum_delay (cstcb) -
		    (s64_t) hs_sched_get_init_maximum_delay (cstcb);
		/* refresh max delay */
		hs_sched_set_maximum_delay (cstcb, hs_sched_get_init_maximum_delay (cstcb));
		preempt_flags_clear_pending (&pf);
		tcb_preempt_flags_set (current, pf);
	    }
	    hs_end_of_timeslice (self, current);
	    reschedule = true;
	}
    }

    /* a higher priority thread was woken up - switch to him.
     * Note: wakeup respects delayed preemption flags */
    if (!reschedule)
    {
	if (wakeup == current)
	{
	    self->__base.delayed_preemption_start = get_timestamp ();
	    return;
	}

	ASSERT (wakeup);
	TRACE_SCHEDULE_DETAILS("wakeup preemption %t %t", current, wakeup);

	if (hs_sched_get_prio_queue (&wakeup->sched_state.base) !=
	    hs_sched_get_prio_queue (cstcb))
	{
	    do_schedule_current ();
	}
	else
	{
	    if (tcb_is_local_cpu (self->__base.scheduled_tcb))
		hs_sched_set_timeslice (sstcb, self->__base.current_timeslice);

	    /* now switch to timeslice of dest */
	    do_schedule (wakeup, sched_dest);
	}
	return;
    }

    /* time slice expired */
    if (EXPECT_FALSE (hs_sched_get_total_quantum (cstcb) != 0))
    {
	/* we have a total quantum - so do some book-keeping */
	if (hs_sched_get_total_quantum (sstcb) == TOTAL_QUANTUM_EXPIRED)
	{
	    /* VU: must be revised.  If a thread has an expired time quantum
	     * and is activated with switch_to his timeslice will expire and
	     * the event will be raised multiple times */
	    hs_total_quantum_expired (self->__base.scheduled_tcb);
	}
	else if (hs_sched_get_total_quantum (sstcb) <= hs_sched_get_timeslice_length (sstcb))
	{
	    /* we are getting close... */
	    hs_sched_set_timeslice (sstcb, hs_sched_get_timeslice (sstcb) +
				    (s64_t) hs_sched_get_total_quantum (sstcb));
	    hs_sched_set_total_quantum (sstcb, TOTAL_QUANTUM_EXPIRED);
	}
	else
	{
	    /* account this time slice */
	    hs_sched_account_quantum (sstcb, hs_sched_get_timeslice_length (sstcb));
	}
    }

    /* schedule the next thread */
    sched_enqueue_ready (self, current, false);
    do_schedule_current ();
}

bool sched_schedule_requests_pending (cpuid_t cpu)
{
    return !schedule_request_queue_is_empty (&schedule_request_queue[cpu]);
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

void xcpu_request_c (cpuid_t dstcpu, xcpu_handler_t handler, tcb_t *tcb, word_t param0)
{ xcpu_request_many (dstcpu, handler, tcb, param0, 0, 0, 0); }
#endif /* CONFIG_SMP */


/* scheduler init/start entry points (the class methods are asm-named
   scheduler_init/scheduler_start and defined in C in api/v4/schedule.c). */
void scheduler_init (scheduler_t *self, bool bootcpu);
void scheduler_start (scheduler_t *self, cpuid_t cpuid);

void sched_init (bool bootcpu)		{ scheduler_init (cur_sched (), bootcpu); }
void sched_start (cpuid_t cpu)		{ scheduler_start (cur_sched (), cpu); }

/* public (void) entry points declared in api/v4/schedule.h */
void sched_schedule_current (void)			{ do_schedule_current (); }
void sched_schedule (tcb_t *dest, word_t flags)		{ do_schedule (dest, flags); }
void sched_schedule_two (tcb_t *dest1, tcb_t *dest2, word_t flags)
							{ do_schedule_two (dest1, dest2, flags); }
void sched_schedule_interrupt (tcb_t *irq, tcb_t *handler) { do_schedule_interrupt (irq, handler); }


/**********************************************************************
 *
 *  sched_ktcb_t entry points (were C++ methods)
 *
 **********************************************************************/

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
    time_t never; never.raw = 0;		/* DEFAULT_TOTAL_QUANTUM */

    hs_sched_init_timeslice (&self->base, hs_default_timeslice ());
    hs_sched_init_total_quantum (&self->base, never);
    self->base.prio_queue = hs_scheduler_get_prio_queue (&cur_sched ()->__base);

    switch (type)
    {
    case sktcb_user:
	hs_sched_set_priority (&self->base, DEFAULT_PRIORITY);
	break;
    case sktcb_root:
	hs_sched_set_priority (&self->base, ROOT_PRIORITY);
	break;
    case sktcb_irq:
    case sktcb_hi:
	hs_sched_set_priority (&self->base, MAX_PRIORITY);
	break;
    case sktcb_lo:
	hs_sched_set_priority (&self->base, 0);
	break;
    }

    /* defacto disable delayed preemptions */
    hs_sched_set_sensitive_prio (&self->base, self->base.priority);
    hs_sched_set_maximum_delay (&self->base, 0);
    hs_sched_set_stride (&self->base, DEFAULT_STRIDE);

#if defined(CONFIG_SMP)
    self->base.requeue = NULL;
#endif
#if defined(CONFIG_X_EVT_LOGGING)
    /* set domain */
    self->logid = (type == sktcb_root) ? ROOTSERVER_LOGID : IDLE_LOGID;
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
	hs_smp_requeue (cur_sched (), false);
#else
    (void) self;
#endif
}

void sched_ktcb_sys_thread_switch (sched_ktcb_t *self)
{
    scheduler_t *sched = cur_sched ();

    /* user cooperatively preempts */
    if (hs_sched_get_maximum_delay (&self->base) <
	hs_sched_get_init_maximum_delay (&self->base))
    {
	tcb_t *tcb = to_tcb (self);
	tcb_t *atcb;
	preempt_flags_t pf = tcb_preempt_flags (tcb);

	preempt_flags_clear_pending (&pf);
	tcb_preempt_flags_set (tcb, pf);

	/* refresh max delay */
	hs_sched_set_maximum_delay (&self->base,
				    hs_sched_get_init_maximum_delay (&self->base));
	TRACEPOINT (SCHEDULE_PM_DELAY_REFRESH, "delayed preemption refresh for %t\n", tcb);

	atcb = sched_get_accounted_tcb ();
	if (atcb && tcb_is_local_cpu (atcb))
	{
	    word_t delta = (word_t) (get_timestamp () - sched->__base.delayed_preemption_start);

	    if (delta > atcb->sched_state.base.max_delay)
	    {
		delta = atcb->sched_state.base.max_delay;
		TRACEF ("sched-hs: large delay penalty\n");
	    }

	    atcb->sched_state.base.delay_penalty += (u16_t) delta;
	    if (atcb->sched_state.base.delay_penalty > get_timer_tick_length ())
	    {
		atcb->sched_state.base.delay_penalty -= (u16_t) get_timer_tick_length ();
		sched->__base.current_timeslice = -(s64_t) get_timer_tick_length ();
	    }
	}
    }

    /* eat up timeslice - we get a fresh one */
    sched->__base.current_timeslice = 0;
    do_schedule_current ();
}

#if defined(CONFIG_DEBUG)
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
	   (word_t) self->base.total_quantum, (word_t) self->base.timeslice_length,
	   (word_t) self->base.current_timeslice);
    printf("abs timeout:    %wdus, rel timeout:       %wdus, prio_queue %p [%c]\n",
	   (word_t) self->base.absolute_timeout,
	   self->base.absolute_timeout == 0 ? 0 :
	   (word_t) (self->base.absolute_timeout - current_time),
	   self->base.prio_queue,
	   hs_flag_is_set (&self->base, HS_SCHED_FLAG_IS_SCHEDULE_DOMAIN) ? 'D' : 'd');
    printf("sens prio: %d, delay: max=%dus, curr=%dus, ",
	   self->base.sensitive_prio, self->base.max_delay, self->base.current_max_delay);
    printf("stride : %wd  pass: %wd\n", self->base.stride, (word_t) self->base.pass);
}
#endif
