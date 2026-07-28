/*********************************************************************
 *
 * Copyright (C) 2007-2010,  Karlsruhe University
 *
 * File path:     api/v4/sched-hs/ktcb.h
 * Description:
 *
 * @LICENSE@
 *
 * $Id:$
 *
 ********************************************************************/
#ifndef __API__V4__SCHED_HS__SCHED_KTCB_H__
#define __API__V4__SCHED_HS__SCHED_KTCB_H__

#include <tcb_layout.h>
#include <generic/bitmask.h>

#include INC_GLUE(ipc.h)
#if defined(CONFIG_X_EVT_LOGGING)
#include INC_GLUE(logging.h)
#endif


/* DEFAULT_TIMESLICE_LENGTH/DEFAULT_TOTAL_QUANTUM were time_t::period(625,3)
   and time_t::never().  time_t's constructors are gone with C++, and its C
   helpers live in api/v4/tcb.h -- far too late in the include order to use
   here -- so the two defaults are built by hs_default_timeslice() in
   sched-hs/schedule.c, next to the only code that consumes them. */
#define DEFAULT_STRIDE			100
#define DEFAULT_PRIORITY		100
#define MAX_PRIORITY			255
#define ROOT_PRIORITY			MAX_PRIORITY


typedef u8_t prio_t;
typedef void (*requeue_callback_t)(tcb_t* tcb);

/* Defined in sched-hs/schedule.h; only ever used through a pointer here. */
struct prio_queue_t;
typedef struct prio_queue_t prio_queue_t;

/* was hs_sched_ktcb_t::flags_e */
#define HS_SCHED_FLAG_IS_SCHEDULE_DOMAIN	0

struct hs_sched_ktcb_t
{
#if defined(CONFIG_SMP)
    tcb_t		*requeue;
    requeue_callback_t	requeue_callback;
#endif

    ringlist_tcb_t	ready_list;
    ringlist_tcb_t	wait_list;

    bitmask_word_t	flags;

    /* were protected; plain fields in C */
    u64_t		total_quantum;
    u64_t		timeslice_length;
    s64_t		current_timeslice;
    u64_t		absolute_timeout;

    u64_t		pass;
    word_t		stride;

    prio_t		priority;
    prio_t		sensitive_prio;

    u16_t		current_max_delay;
    u16_t		max_delay;
    u16_t		delay_penalty;
    u16_t		reserved0;

    prio_queue_t	*prio_queue;
};
typedef struct hs_sched_ktcb_t hs_sched_ktcb_t;

typedef hs_sched_ktcb_t policy_sched_ktcb_t;


/* Accessors for hs_sched_ktcb_t, used by api/v4/sched-hs/schedule.c.
   `self' is the policy base -- reach it as &tcb->sched_state.base.
   The non-trivial ones (delay_preemption, account_pass, set_prio_queue,
   migrate_prio_queue, and the time_t-taking initialisers) are functions in
   sched-hs/schedule.c. */
INLINE u64_t hs_sched_get_total_quantum (hs_sched_ktcb_t *self)		{ return self->total_quantum; }
INLINE void  hs_sched_set_total_quantum (hs_sched_ktcb_t *self, u64_t q){ self->total_quantum = q; }
INLINE u64_t hs_sched_account_quantum (hs_sched_ktcb_t *self, u64_t t)
{ self->total_quantum -= t; return self->total_quantum; }

INLINE s64_t hs_sched_get_timeslice (hs_sched_ktcb_t *self)		{ return self->current_timeslice; }
/* current_timeslice is signed and is deliberately driven negative by the
   preemption logic, so this must not narrow to u32_t. */
INLINE void  hs_sched_set_timeslice (hs_sched_ktcb_t *self, s64_t t)	{ self->current_timeslice = t; }
INLINE u64_t hs_sched_get_timeslice_length (hs_sched_ktcb_t *self)	{ return self->timeslice_length; }

INLINE void  hs_sched_set_maximum_delay (hs_sched_ktcb_t *self, u16_t usec)
{ self->current_max_delay = usec; self->delay_penalty = 0; }
INLINE u16_t hs_sched_get_maximum_delay (hs_sched_ktcb_t *self)		{ return self->current_max_delay; }
INLINE void  hs_sched_init_maximum_delay (hs_sched_ktcb_t *self, u16_t usec)
{ self->current_max_delay = self->max_delay = usec; }
INLINE u16_t hs_sched_get_init_maximum_delay (hs_sched_ktcb_t *self)	{ return self->max_delay; }
INLINE void  hs_sched_add_delay_penalty (hs_sched_ktcb_t *self, u16_t usec)
{ self->delay_penalty = usec; }

INLINE prio_t hs_sched_get_sensitive_prio (hs_sched_ktcb_t *self)	{ return self->sensitive_prio; }
INLINE void   hs_sched_set_sensitive_prio (hs_sched_ktcb_t *self, prio_t prio)
{ self->sensitive_prio = prio; }
INLINE prio_t hs_sched_get_priority (hs_sched_ktcb_t *self)		{ return self->priority; }
INLINE void   hs_sched_set_priority (hs_sched_ktcb_t *self, prio_t prio)
{
    self->priority = prio;
    /* keep sensitive and current prio in-sync to reduce checking overhead */
    if (self->sensitive_prio < prio)
	hs_sched_set_sensitive_prio (self, prio);
}

INLINE u64_t hs_sched_get_timeout (hs_sched_ktcb_t *self)		{ return self->absolute_timeout; }
INLINE bool  hs_sched_has_timeout_expired (hs_sched_ktcb_t *self, u64_t time)
{ return self->absolute_timeout <= time; }

INLINE void   hs_sched_set_stride (hs_sched_ktcb_t *self, word_t s)	{ self->stride = s; }
INLINE word_t hs_sched_get_stride (hs_sched_ktcb_t *self)		{ return self->stride; }
INLINE void   hs_sched_set_pass (hs_sched_ktcb_t *self, u64_t p)	{ self->pass = p; }
INLINE u64_t  hs_sched_get_pass (hs_sched_ktcb_t *self)			{ return self->pass; }

INLINE prio_queue_t * hs_sched_get_prio_queue (hs_sched_ktcb_t *self)	{ return self->prio_queue; }

/* The domain queue is stored in the TCB's kernel stack area; `self' stands in
   for the C++ `this'.  Guarded because OFS_TCB_KERNEL_STACK is what the
   tcb_layout pass is in the middle of computing.  The C++ form fell off the
   end of the function in that case; return NULL instead. */
INLINE prio_queue_t * hs_sched_get_domain_prio_queue (hs_sched_ktcb_t *self)
{
#if !defined(BUILD_TCB_LAYOUT)
    return (prio_queue_t *) (((word_t) self & KTCB_MASK) + OFS_TCB_KERNEL_STACK);
#else
    (void) self;
    return NULL;
#endif
}

BEGIN_DECLS
bool   hs_sched_delay_preemption (hs_sched_ktcb_t *self, tcb_t *tcb);
void   hs_sched_account_pass (hs_sched_ktcb_t *self);
void   hs_sched_set_prio_queue (hs_sched_ktcb_t *self, prio_queue_t *q);
void   hs_sched_migrate_prio_queue (hs_sched_ktcb_t *self, prio_queue_t *nq);
END_DECLS

#endif /* !__API__V4__SCHED_HS__SCHED_KTCB_H__ */
