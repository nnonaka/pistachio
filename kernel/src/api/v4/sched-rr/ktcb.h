/*********************************************************************
 *                
 * Copyright (C) 2007-2010,  Karlsruhe University
 *                
 * File path:     api/v4/sched-rr/ktcb.h
 * Description:   
 *                
 * @LICENSE@
 *                
 * $Id:$
 *                
 ********************************************************************/
#ifndef __API__V4__SCHED_RR__SCHED_KTCB_H__
#define __API__V4__SCHED_RR__SCHED_KTCB_H__

#include INC_GLUE(ipc.h)
#if defined(CONFIG_X_EVT_LOGGING)
#include INC_GLUE(logging.h)
#endif


#define DEFAULT_TIMESLICE_LENGTH	(time_t::period(625, 4))
#define DEFAULT_TOTAL_QUANTUM		(time_t::never())
#define DEFAULT_PRIORITY		100
#define MAX_PRIORITY			255
#define ROOT_PRIORITY			MAX_PRIORITY

typedef u8_t prio_t;
typedef void (*requeue_callback_t)(tcb_t* tcb);

struct rr_sched_ktcb_t
{
  
    
#if defined(CONFIG_SMP)
    tcb_t		*requeue;
    requeue_callback_t	requeue_callback;
#endif

    /* scheduling lists  */
    ringlist_tcb_t	ready_list;
    ringlist_tcb_t	wait_list;
    

   
    u64_t		total_quantum;
    u64_t		timeslice_length;
    s64_t		current_timeslice;
    u64_t		absolute_timeout;

    prio_t		priority;
    /* delayed preemption */
    prio_t		sensitive_prio;
    u16_t		current_max_delay;
    u16_t		max_delay;

    /* Explicit tail padding.  Without it this struct ends at offset 86 with
       two bytes of padding, and C++ reuses that padding for the first member
       of the derived sched_ktcb_t (placing `scheduler` at 86) while the C
       rep, which embeds this struct by value, cannot -- putting `scheduler`
       at 88.  Filling the hole makes both languages agree.  See notes §78. */
    u16_t		__tail_pad;
    
    
   
   
};
typedef struct rr_sched_ktcb_t rr_sched_ktcb_t;

/* Accessors for rr_sched_ktcb_t, used by api/v4/sched-rr/schedule.c.
   `self' is the policy base -- reach it as &tcb->sched_state.base. */
INLINE u64_t rr_sched_get_total_quantum (rr_sched_ktcb_t *self)		{ return self->total_quantum; }
INLINE void  rr_sched_set_total_quantum (rr_sched_ktcb_t *self, u64_t q){ self->total_quantum = q; }
INLINE u64_t rr_sched_account_quantum (rr_sched_ktcb_t *self, u32_t t)
{ self->total_quantum -= t; return self->total_quantum; }

INLINE s64_t rr_sched_get_timeslice (rr_sched_ktcb_t *self)		{ return self->current_timeslice; }
INLINE s64_t rr_sched_account_timeslice (rr_sched_ktcb_t *self, u32_t t)
{ self->current_timeslice -= t; return self->current_timeslice; }
INLINE void  rr_sched_renew_timeslice (rr_sched_ktcb_t *self, u32_t t)	{ self->current_timeslice += t; }
INLINE u64_t rr_sched_get_timeslice_length (rr_sched_ktcb_t *self)	{ return self->timeslice_length; }

INLINE prio_t rr_sched_get_priority (rr_sched_ktcb_t *self)		{ return self->priority; }
INLINE prio_t rr_sched_get_sensitive_prio (rr_sched_ktcb_t *self)	{ return self->sensitive_prio; }

INLINE u64_t rr_sched_get_timeout (rr_sched_ktcb_t *self)		{ return self->absolute_timeout; }
INLINE bool  rr_sched_has_timeout_expired (rr_sched_ktcb_t *self, u64_t time)
{ return self->absolute_timeout <= time; }

INLINE void   rr_sched_set_maximum_delay (rr_sched_ktcb_t *self, u16_t usec) { self->current_max_delay = usec; }
INLINE void   rr_sched_init_maximum_delay (rr_sched_ktcb_t *self, u16_t usec)
{ self->current_max_delay = self->max_delay = usec; }
INLINE void   rr_sched_set_sensitive_prio (rr_sched_ktcb_t *self, prio_t prio) { self->sensitive_prio = prio; }
INLINE void   rr_sched_set_priority (rr_sched_ktcb_t *self, prio_t prio)
{
    self->priority = prio;
    /* keep sensitive and current prio in-sync to reduce checking overhead */
    if (self->sensitive_prio < prio)
	rr_sched_set_sensitive_prio (self, prio);
}
/* time_t::period(625,4) as a C initializer (mantissa 625, exponent 4, type 0) */
INLINE time_t rr_default_timeslice (void)
{ time_t t; t.raw = 0; t.time.mantissa = 625; t.time.exponent = 4; t.time.type = 0; return t; }

INLINE void   rr_sched_init_timeslice (rr_sched_ktcb_t *self, time_t timeslice)
{
    ASSERT (timeslice.time.type == 0);  /* is_period */
    /* time_t::get_microseconds inlined (declared later in tcb.h) */
    self->current_timeslice = self->timeslice_length =
	(s64_t) ((1 << timeslice.time.exponent) * timeslice.time.mantissa);
}
INLINE u16_t  rr_sched_get_maximum_delay (rr_sched_ktcb_t *self)	{ return self->current_max_delay; }

typedef rr_sched_ktcb_t policy_sched_ktcb_t;

#endif /* !__API__V4__SCHED_RR__SCHED_KTCB_H__ */
