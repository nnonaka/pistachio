/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2007-2010,  Karlsruhe University
 *                
 * File path:     api/v4/schedule.h
 * Description:   scheduling declarations
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
 * $Id: schedule.h,v 1.24 2006/10/19 22:57:34 ud3 Exp $
 *                
 ********************************************************************/

#ifndef __API__V4__SCHEDULE_H__
#define __API__V4__SCHEDULE_H__

#include INC_API(tcb.h)
#include INC_GLUE(schedule.h)
#include <kdb/tracepoints.h>

EXTERN_TRACEPOINT(SCHEDULE_DETAILS);

enum sched_flags_e 
{
    sched_chk_flag	   = 0, // run scheduling policy
    sched_ds1_flag	   = 1, // schedule dest1
    sched_ds2_flag	   = 2, // schedule dest2
    sched_c2r_flag	   = 3, // current was running
    sched_timeout_flag	   = 4, // cancel timeout
    /* rr specific flags */
    rr_tsdonate_flag	   = 5, // round-robin TS donation
    /* pm specific flags */
    pm_chk_preemption_flag = 6, // PM-scheduling check
};

typedef u8_t sched_flags_t;


/* 'static' keeps these at internal linkage in C too (C++ file-scope const is
   already internal); without it, every C TU that includes this header emits an
   external definition and they collide at link time. */
static const sched_flags_t sched_default = FLAGFIELD2(sched_chk_flag, sched_c2r_flag);
static const sched_flags_t sched_current = FLAGFIELD2(sched_ds1_flag, sched_c2r_flag);
static const sched_flags_t sched_dest =    FLAGFIELD2(sched_ds2_flag, sched_c2r_flag);
static const sched_flags_t sched_handoff = FLAGFIELD1(sched_ds2_flag);

/* IPC default flags */
static const sched_flags_t sched_sndonly = FLAGFIELD3(sched_chk_flag, sched_c2r_flag, sched_timeout_flag);
static const sched_flags_t sched_ipcblk  = FLAGFIELD3(sched_ds2_flag, rr_tsdonate_flag, pm_chk_preemption_flag);
static const sched_flags_t sched_rcverr  = FLAGFIELD3(sched_ds2_flag, rr_tsdonate_flag, pm_chk_preemption_flag);
static const sched_flags_t sched_rplywt  = FLAGFIELD3(sched_chk_flag, rr_tsdonate_flag, sched_timeout_flag);


/* C entry point: dispatch a timer tick to the current scheduler
   (defined in api/v4/schedule.cc). */
BEGIN_DECLS
void sched_handle_timer_interrupt(void);
/* Current scheduler wrappers for C callers (defined in schedule.cc). flags is
   a sched_flags_t value (sched_default/sched_handoff/...). */
void  sched_schedule (tcb_t *dest, word_t flags);
u64_t sched_get_current_time (void);
void  sched_deschedule (tcb_t *tcb);
tcb_t * sched_get_accounted_tcb (void);
void  sched_set_accounted_tcb (tcb_t *tcb);
void  sched_schedule_current (void);
void  sched_move_tcb (tcb_t *tcb, cpuid_t cpu);
void  sched_schedule_interrupt (tcb_t *irq, tcb_t *handler);
void  sched_remote_schedule (tcb_t *tcb);
void  sched_schedule_two (tcb_t *dest1, tcb_t *dest2, word_t flags);
bool  sched_idle_hlt (void);
void  sched_init (bool bootcpu);
void  sched_start (cpuid_t cpu);
void  sched_idle (void);
bool  sched_schedule_requests_pending (cpuid_t cpu);
bool  sched_is_scheduler (tcb_t *tcb, tcb_t *dest);
END_DECLS

/* schedule_req_t / schedule_request_queue_t are dual-repped: api/v4/schedule.c
   uses schedule_req_t by value and indexes the request queue.  The data is
   C-visible; the C++ methods stay guarded. */
#define SCHEDULE_QUEUE_LEN 128

struct schedule_req_t
{
    schedule_ctrl_t time_control;
    schedule_ctrl_t prio_control;
    schedule_ctrl_t preemption_control;
    schedule_ctrl_t processor_control;
    tcb_t* tcb;
    bool valid;

};
typedef struct schedule_req_t schedule_req_t;

struct schedule_request_queue_t
{
    schedule_req_t entries[SCHEDULE_QUEUE_LEN];
    word_t first_alloc;
    word_t first_free;
    spinlock_t lock;

    char pad2[CACHE_LINE_SIZE - sizeof(spinlock_t)];


};
typedef struct schedule_request_queue_t schedule_request_queue_t;

/* Was a scheduler_t static member; now a plain global (defined in
   api/v4/schedule.c) so C and the C++ schedule_requests_pending inline agree. */
extern schedule_request_queue_t schedule_request_queue[CONFIG_SMP_MAX_CPUS];

/* C forms of the schedule_request_queue_t methods (mirror the C++ inlines;
   the data + spinlock are C-visible). */
INLINE bool schedule_request_queue_is_empty (schedule_request_queue_t *self)
{ return self->first_alloc == self->first_free; }

INLINE schedule_req_t * schedule_request_queue_reserve_request (schedule_request_queue_t *self)
{
    spinlock_lock (&self->lock);
    if ( ((self->first_free + 1) % SCHEDULE_QUEUE_LEN) == self->first_alloc )
    {
	spinlock_unlock (&self->lock);
	return (schedule_req_t *) 0;
    }
    word_t idx = self->first_free;
    self->first_free = (self->first_free + 1) % SCHEDULE_QUEUE_LEN;
    return &self->entries[idx];
}

INLINE schedule_req_t schedule_request_queue_process_request (schedule_request_queue_t *self)
{
    spinlock_lock (&self->lock);
    schedule_req_t req = self->entries[self->first_alloc];
    self->entries[self->first_alloc].valid = false;
    self->first_alloc = (self->first_alloc + 1) % SCHEDULE_QUEUE_LEN;
    spinlock_unlock (&self->lock);
    return req;
}

INLINE void schedule_request_queue_commit_request (schedule_request_queue_t *self)
{ spinlock_unlock (&self->lock); }

/* current-scheduler wrappers taking schedule_req_t (defined above) by pointer;
   for the SYS_SCHEDULE path in api/v4/schedule.c.  Defined in
   api/v4/sched-rr/schedule.cc. */
BEGIN_DECLS
word_t sched_check_schedule_parameters (tcb_t *scheduler, schedule_req_t *req);
word_t sched_return_schedule_parameter (word_t num, schedule_req_t *req);
void   sched_commit_schedule_parameters (schedule_req_t *req);
END_DECLS

/* The RR policy scheduler types are dual-repped, so this is C-includable now. */
#include INC_API_SCHED(schedule.h)

/* C rep of scheduler_t: derives from policy_scheduler_t with no added instance
   data, so it has that base's layout (composed as __base at offset 0). */
typedef struct scheduler_t { policy_scheduler_t __base; } scheduler_t;


#endif /*__API__V4__SCHEDULE_H__*/

