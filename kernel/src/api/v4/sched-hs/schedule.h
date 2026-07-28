/*********************************************************************
 *
 * Copyright (C) 2007-2010,  Karlsruhe University
 *
 * File path:     api/v4/sched-hs/schedule.h
 * Description:
 *
 * @LICENSE@
 *
 * $Id:$
 *
 ********************************************************************/
#ifndef __API__V4__SCHED_HS__SCHEDULE_H__
#define __API__V4__SCHED_HS__SCHEDULE_H__

#define DELAY_PREEMPT_TICKS	1
typedef u64_t period_cycles_t;
struct schedule_req_t;
typedef struct schedule_req_t schedule_req_t;

#include INC_API(smp.h)

EXTERN_KMEM_GROUP(kmem_sched);


/* C rep of the C++ prio_queue_t: data members in declaration order
   (max_prio, the private block, then the public counters and the queue). */
struct prio_queue_t
{
    s16_t		max_prio;

    /* were private */
    word_t		refcnt;
    tcb_t *		domain_tcb;
    word_t		depth;
    word_t		count;

    u64_t		global_pass;	/* proportional share in queue */
    period_cycles_t	period_cycles;	/* CPU cycles used this sampling period */
    period_cycles_t	poll_window;
    u64_t		timeslice_start;
#if defined(CONFIG_SMP)
    struct prio_queue_t *cpu_link, *cpu_head;
#endif
    tcb_t *		prio_queue[MAX_PRIORITY + 1];
};


/* C rep of hs_scheduler_t: instance data only.  Its statics (current_time,
   smp_requeue_lists) become file-scope globals in sched-hs/schedule.c, and
   its methods become functions there. */
struct hs_scheduler_t
{
    tcb_t *		wakeup_list;
    s64_t		current_timeslice;
    u64_t		delayed_preemption_start;

    tcb_t *		scheduled_tcb;
    prio_queue_t	root_prio_queue;
    prio_queue_t *	scheduled_queue;
};
typedef struct hs_scheduler_t hs_scheduler_t;

typedef hs_scheduler_t policy_scheduler_t;
typedef prio_queue_t policy_sched_next_thread_t;

#if defined(CONFIG_SMP)
/* C rep of smp_requeue_t (same layout: list head + pad, lock + pad). */
typedef struct smp_requeue_t
{
    tcb_t *	tcb_list;
    char	cache_pad0[CACHE_LINE_SIZE - sizeof(tcb_t*)];
    spinlock_t	lock;
    char	cache_pad1[CACHE_LINE_SIZE - sizeof(spinlock_t)];
} smp_requeue_t;

INLINE bool smp_requeue_is_empty (smp_requeue_t *self)
{ return self->tcb_list == NULL; }
#endif /* defined(CONFIG_SMP) */


/* C forms of the trivial prio_queue_t accessors.  enqueue/dequeue, init,
   prio_tickets and everything else that dereferences a tcb_t need the complete
   type, which is not available this early in the include order -- those are
   functions in sched-hs/schedule.c. */
INLINE tcb_t * prio_queue_get (prio_queue_t *self, prio_t prio)
{ return self->prio_queue[prio]; }
INLINE void prio_queue_set (prio_queue_t *self, prio_t prio, tcb_t *tcb)
{ self->prio_queue[prio] = tcb; }

INLINE tcb_t * prio_queue_get_domain_tcb (prio_queue_t *self)
{ return self->domain_tcb; }

INLINE u64_t prio_queue_get_global_pass (prio_queue_t *self)
{ return self->global_pass; }
INLINE void prio_queue_set_global_pass (prio_queue_t *self, u64_t pass)
{ self->global_pass = pass; }

INLINE word_t prio_queue_get_depth (prio_queue_t *self)
{ return self->depth; }
INLINE void prio_queue_set_depth (prio_queue_t *self, prio_queue_t *parent_queue)
{ self->depth = prio_queue_get_depth (parent_queue) + 1; }

INLINE void prio_queue_start_timeslice (prio_queue_t *self, u64_t current_time)
{ self->timeslice_start = current_time; }
INLINE void prio_queue_end_timeslice (prio_queue_t *self, u64_t current_time)
{
    self->period_cycles += (period_cycles_t) (current_time - self->timeslice_start);
    self->timeslice_start = current_time;
}
INLINE void prio_queue_reset_period_cycles (prio_queue_t *self, u64_t current_time)
{
    self->period_cycles = 0;
    self->timeslice_start = current_time;
    self->poll_window = current_time;
}
INLINE period_cycles_t prio_queue_get_period_cycles (prio_queue_t *self)
{ return self->period_cycles; }
INLINE period_cycles_t prio_queue_get_poll_window (prio_queue_t *self)
{ return self->poll_window; }

INLINE bool prio_queue_is_same_domain (prio_queue_t *self, prio_queue_t *prio_queue)
{
#if defined(CONFIG_SMP)
    return prio_queue->cpu_head == self->cpu_head;
#else
    return self == prio_queue;
#endif
}

INLINE prio_queue_t * hs_scheduler_get_prio_queue (hs_scheduler_t *self)
{ return &self->root_prio_queue; }
INLINE prio_queue_t * hs_scheduler_get_scheduled_queue (hs_scheduler_t *self)
{ return self->scheduled_queue; }

#endif /* !__API__V4__SCHED_HS__SCHEDULE_H__ */
