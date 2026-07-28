/*********************************************************************
 *                
 * Copyright (C) 2007-2009,  Karlsruhe University
 *                
 * File path:     api/v4/sched-rr/schedule.h
 * Description:   
 *                
 * @LICENSE@
 *                
 * $Id:$
 *                
 ********************************************************************/
#ifndef __API__V4__SCHED_RR__SCHEDULE_H__
#define __API__V4__SCHED_RR__SCHEDULE_H__


/* C rep: same layout as the C++ prio_queue_t (max_prio, then the private
   timeslice_tcb + prio_queue[] in declaration order). */
typedef struct prio_queue_t
{
    s16_t max_prio;
    tcb_t * timeslice_tcb;
    tcb_t * prio_queue[MAX_PRIORITY + 1];
} prio_queue_t;


/* C rep of rr_scheduler_t: instance data only (its statics and methods, and
   smp_requeue_t, are C++-only). */
typedef struct rr_scheduler_t
{
    tcb_t * wakeup_list;
    prio_queue_t root_prio_queue;
} rr_scheduler_t;
typedef rr_scheduler_t policy_scheduler_t;
typedef void policy_sched_next_thread_t;

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

INLINE void smp_requeue_enqueue_head (smp_requeue_t *self, tcb_t *tcb)
{
    ASSERT (spinlock_is_locked (&self->lock));
    ASSERT (tcb);
    tcb->sched_state.base.requeue = self->tcb_list;
    self->tcb_list = tcb;
}

INLINE tcb_t * smp_requeue_dequeue_head (smp_requeue_t *self)
{
    ASSERT (spinlock_is_locked (&self->lock));
    ASSERT (!smp_requeue_is_empty (self));

    tcb_t *tcb = self->tcb_list;
    self->tcb_list = tcb->sched_state.base.requeue;
    tcb->sched_state.base.requeue = NULL;
    return tcb;
}
#endif /* defined(CONFIG_SMP) */

/* C forms of the trivial prio_queue_t accessors.  enqueue/dequeue and the
   scheduler_t helpers need tcb_t/scheduler_t, which are not complete this
   early in the include order -- they are static functions in schedule.c. */
INLINE tcb_t * prio_queue_get (prio_queue_t *self, prio_t prio)
{ return self->prio_queue[prio]; }
INLINE void prio_queue_set (prio_queue_t *self, prio_t prio, tcb_t *tcb)
{ self->prio_queue[prio] = tcb; }
INLINE void prio_queue_set_timeslice_tcb (prio_queue_t *self, tcb_t *tcb)
{ self->timeslice_tcb = tcb; }
INLINE tcb_t * prio_queue_get_timeslice_tcb (prio_queue_t *self)
{ return self->timeslice_tcb; }





#endif /* !__API__V4__SCHED_RR__SCHEDULE_H__ */
