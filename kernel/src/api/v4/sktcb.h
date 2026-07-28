/*********************************************************************
 *                
 * Copyright (C) 2009-2010,  Karlsruhe University
 *                
 * File path:     api/v4/sktcb.h
 * Description:   
 *                
 * @LICENSE@
 *                
 * $Id:$
 *                
 ********************************************************************/
#ifndef __API__V4__SKTCB_H__
#define __API__V4__SKTCB_H__

#include INC_API(ipc.h)

#define MAX_LOGIDS		__UL(32)
#define NULL_LOGID		(0xFFFFFFFF)
#define IDLE_LOGID		(0)
#define ROOTSERVER_LOGID	(1)

enum sktcb_type_e {
    sktcb_user	= 0,
    sktcb_hi	= 1,
    sktcb_lo	= 2,
    sktcb_root	= 3,
    sktcb_irq	= 4,
};
/* C has no implicit type name for an enum tag; make `sktcb_type_e' usable
   bare (as C++ already does) for the C decls below and api/v4/thread.c. */
typedef enum sktcb_type_e sktcb_type_e;

typedef u8_t prio_t;

#include INC_API_SCHED(ktcb.h)

struct sched_ktcb_t {
    policy_sched_ktcb_t	base;	/* single inheritance -> base as first member */
    /* do not delete this TCB_START_MARKER */

    threadid_t		scheduler;
#if defined(CONFIG_X_EVT_LOGGING)
    word_t              logid;
#endif
    /* TCB_END_MARKER */
};
typedef struct sched_ktcb_t sched_ktcb_t;

/* C wrappers for the sched_ktcb_t methods api/v4/thread.c drives (defined in
   schedule.cc). set_timeout(time_t) is tcb_sched_set_timeout in tcb.h. */
BEGIN_DECLS
void sched_ktcb_init (sched_ktcb_t *self, sktcb_type_e type);
void sched_ktcb_set_scheduler (sched_ktcb_t *self, threadid_t tid);
void sched_ktcb_delete_tcb (sched_ktcb_t *self);
void sched_ktcb_cancel_timeout (sched_ktcb_t *self);
void sched_ktcb_set_timeout_abs (sched_ktcb_t *self, u64_t absolute_time, bool enqueue);
threadid_t sched_ktcb_get_scheduler (sched_ktcb_t *self);
void sched_ktcb_sys_thread_switch (sched_ktcb_t *self);
#if defined(CONFIG_DEBUG)
/* Debug dumps used by the kdb showtcb commands (sched-rr/schedule.c). */
void sched_ktcb_dump_priority (sched_ktcb_t *self);
void sched_ktcb_dump_list1 (sched_ktcb_t *self);
void sched_ktcb_dump_list2 (sched_ktcb_t *self);
void sched_ktcb_dump (sched_ktcb_t *self, u64_t current_time);
#endif
END_DECLS

#endif /* !__API__V4__SKTCB_H__ */
