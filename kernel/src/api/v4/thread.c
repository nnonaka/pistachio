/*********************************************************************
 *
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, Jan Stoess, IBM Corporation
 *
 * File path:     api/v4/thread.cc
 * Description:
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
 ********************************************************************/

#include INC_API(config.h)
#include INC_API(tcb.h)
#include INC_API(thread.h)
#include INC_API(interrupt.h)
#include INC_API(schedule.h)
#include INC_API(space.h)
#include INC_API(generic-archmap.h)
#include INC_API(kernelinterface.h)
#include INC_GLUE(syscalls.h)
#include INC_API(syscalls.h)
#include INC_API(smp.h)

#include <generic/lib.h>
#include <kdb/tracepoints.h>

DECLARE_TRACEPOINT(SYSCALL_THREAD_CONTROL);
DECLARE_KMEM_GROUP(kmem_tcb);

whole_tcb_t __whole_dummy_tcb  __attribute__((aligned(sizeof(whole_tcb_t))));

/* TID uses a C++ method; C form. */
#undef TID
#define TID(x)	threadid_get_raw (&(x))

/* C prototypes for the asm-named functions (C++ methods elsewhere, invisible
   to C) that thread.c defines or calls. */
void tcb_delete_tcb (tcb_t *);
void tcb_unwind (tcb_t *, word_t);
void tcb_save_state (tcb_t *);
void tcb_restore_state (tcb_t *);
bool tcb_migrate_to_space (tcb_t *, space_t *);
bool tcb_migrate_to_processor (tcb_t *, cpuid_t);
void tcb_send_pagefault_ipc (tcb_t *, addr_t, addr_t, int);
bool tcb_send_preemption_ipc (tcb_t *);
bool tcb_activate (tcb_t *, void (*)(void), threadid_t);
void tcb_create_inactive (tcb_t *, threadid_t, threadid_t, sktcb_type_e);
void tcb_create_kernel_thread (tcb_t *, threadid_t, utcb_t *, sktcb_type_e);
bool tcb_is_interrupt_thread (tcb_t *);
void handle_ipc_error (void);
/* asm-named functions defined in other C/C++ files */
void space_free (space_t *);
void tcb_resources_init (thread_resources_t *, tcb_t *);
void tcb_resources_free (thread_resources_t *, tcb_t *);
void tcb_resources_load (thread_resources_t *, tcb_t *);
void tcb_resources_purge (thread_resources_t *, tcb_t *);

tcb_t *__dummy_tcb = (tcb_t *) &__whole_dummy_tcb;

/* Architecture-neutral: __dummy_tcb is defined just above. */
tcb_t * get_dummy_tcb_c (void)			{ return __dummy_tcb; }


#if defined(CONFIG_STATIC_TCBS)
/*
 * Static TCBs: a linear array of pointers rather than a demand-paged virtual
 * KTCB area.  Required by PPC440 (config/powerpc.cml: CPU_POWERPC_PPC440
 * implies STATIC_TCBS), whose software-managed TLB cannot take faults on the
 * KTCB area.  tcb_array was a static member of class tcb_t; in C it is a
 * plain file-scope array, which is what the assembler stubs already assume --
 * they reach it through static_tcb_array (see x86 trap.S).
 */
tcb_t *tcb_array[TOTAL_KTCBS];
addr_t static_tcb_array;

/* FIXME (inherited): is_tcb() scans all TOTAL_KTCBS entries. */
tcb_t * tcb_allocate (threadid_t dest)
{
    word_t idx = threadid_get_threadno (&dest);
    ASSERT (idx < TOTAL_KTCBS);
    if (tcb_array[idx] == get_dummy_tcb_c ())
	tcb_array[idx] = (tcb_t *) kmem_alloc (&kmem, kmem_tcb, KTCB_SIZE);
    return tcb_array[idx];
}

void tcb_deallocate (threadid_t dest)
{
    word_t idx = threadid_get_threadno (&dest);
    ASSERT (idx < TOTAL_KTCBS);
    tcb_t *tcb = tcb_array[idx];
    tcb_array[idx] = get_dummy_tcb_c ();
    kmem_free (&kmem, kmem_tcb, (void *) tcb, KTCB_SIZE);
}

void tcb_init_tcbs (void)
{
    word_t i;

    for (i = 0; i < TOTAL_KTCBS; i++)
	tcb_array[i] = get_dummy_tcb_c ();

    static_tcb_array = (addr_t) &tcb_array[0];
}
#endif /* defined(CONFIG_STATIC_TCBS) */


/* Forward declarations for the file-static / self-referencing pieces. */
static tcb_t * create_root_server (threadid_t dest_tid, threadid_t scheduler_tid,
				   threadid_t pager_tid, fpage_t utcb_area,
				   fpage_t kip_area, word_t utcb_location,
				   word_t ip, word_t sp);
void tcb_init (tcb_t *self, threadid_t dest, sktcb_type_e type);
bool tcb_check_utcb_location (tcb_t *self, word_t utcb_location);
static void thread_startup (void);


bool tcb_is_interrupt_thread (tcb_t *self)
{
    threadid_t g = tcb_get_global_id (self);
    return (threadid_is_interrupt (&g) &&
	    threadid_get_threadno (&g) < thread_info_get_system_base (&get_kip()->thread_info));
}

/**
 * Stub invoked after a startup message has been received from the
 * thread's pager.
 */
static void thread_startup (void)
{
    tcb_t * current = get_current_tcb();
    msg_tag_t tag = tcb_get_tag (current);

    // To avoid a mess when thread is set up via ctrlxfer items,
    // set IP/SP only if pager sends at least 2 untyped words
    if (msg_tag_get_untyped (&tag) == 2)
    {
	tcb_set_user_ip (current, (addr_t) tcb_get_mr (current, 1));
	tcb_set_user_sp (current, (addr_t) tcb_get_mr (current, 2));
    }

    TRACE_SCHEDULE_DETAILS("startup %t: ip=%p  sp=%p\n", current,
			    tcb_get_user_ip (current), tcb_get_user_sp (current));

    current->misc.saved_state[0].state = THREAD_STATE_ABORTED;
    tcb_set_state (current, THREAD_STATE_RUNNING);
}


/**
 * Fake that thread is waiting for IPC startup message.
 */
static void fake_wait_for_startup (tcb_t * tcb, threadid_t pager)
{
    // Fake that we are waiting to receive untyped words from our pager.
    tcb_set_state (tcb, THREAD_STATE_WAITING_FOREVER);

    tcb->partner = pager;

    acceptor_t acceptor;
    acceptor.raw = 0;
    tcb_set_br (tcb, 0, acceptor.raw);

    // Make sure that unwind will work on waiting thread.
    tcb->misc.saved_state[0].partner = threadid_nilthread ();

    // Make sure that IPC TCB_UNWIND_ABORT will restore user-level exception frame.
    tcb->misc.saved_state[0].state = THREAD_STATE_RUNNING;
}


void thread_return (void)
{
    /* normal return function - do nothing */
}

void tcb_init (tcb_t *self, threadid_t dest, sktcb_type_e type)
{
    /* clear utcb and space */
    self->utcb = NULL;
    self->space = NULL;

    tcb_lock_init (self);

    /* make sure, nobody messes around with the thread */
    tcb_set_state (self, THREAD_STATE_ABORTED);
    self->partner = threadid_nilthread ();
    tcb_init_saved_state (self);

    /* set thread id */
    self->myself_global = dest;
    self->myself_local = ANYLOCALTHREAD;

    /* initialize thread resources */
    tcb_resources_init (&self->resources, self);

    /* queue initialization */
    queue_state_init (&self->queue_state);
    self->send_head = NULL;

#if defined(CONFIG_SMP)
    /* initially assign to this CPU */
    self->cpu = get_current_cpu();
    tcb_lock_state_init (self);
#endif

    /* initialize scheduling */
    sched_ktcb_init (&self->sched_state, type);

    /* enqueue into present list, do not enqueue the idle thread */
    if (self != get_idle_tcb_c())
	tcb_enqueue_present (self);

    tcb_init_stack (self);
}


void tcb_create_kernel_thread (tcb_t *self, threadid_t dest, utcb_t * utcb, sktcb_type_e type)
{
    tcb_init (self, dest, type);
    self->utcb = utcb;
}

void tcb_create_inactive (tcb_t *self, threadid_t dest, threadid_t scheduler, sktcb_type_e type)
{
    tcb_init (self, dest, type);
    sched_ktcb_set_scheduler (&self->sched_state, scheduler);
}

bool tcb_activate (tcb_t *self, void (*startup_func)(void), threadid_t pager)
{
    ASSERT(self->space);
    ASSERT(!tcb_is_activated (self));

    // UTCB location has already been checked during thread creation.
    ASSERT(tcb_check_utcb_location (self, tcb_get_utcb_location (self)));

    /* allocate UTCB */
    self->utcb = space_allocate_utcb (self->space, self);
    if (!self->utcb)
	return false;

    /* update global id in UTCB */
    tcb_set_global_id (self, tcb_get_global_id (self));

    /* initialize pager and exception handler */
    tcb_set_pager (self, pager);
    tcb_set_cpu (self, get_current_cpu());
    tcb_set_exception_handler (self, NILTHREAD);

    /* initialize the startup stack */
    tcb_create_startup_stack (self, startup_func);
    return true;
}

/**
 * Check if supplied address is a valid UTCB location.
 */
bool tcb_check_utcb_location (tcb_t *self, word_t utcb_location)
{
    fpage_t utcb_area = space_get_utcb_page_area (self->space);
    return (utcb_info_is_valid_utcb_location (&get_kip()->utcb_info, utcb_location) &&
	    fpage_is_range_in_fpage (&utcb_area,
				     (addr_t) utcb_location,
				     (addr_t) (utcb_location + sizeof (utcb_t))));
}


#if defined(CONFIG_SMP)
static void do_xcpu_delete_done (cpu_mb_entry_t * entry)
{
    tcb_t * tcb = entry->tcb;
    ASSERT(tcb);

    // still on same CPU? --> otherwise forward
    if (!tcb_is_local_cpu (tcb))
	UNIMPLEMENTED();

    if (tcb_get_state (tcb) != THREAD_STATE_XCPU_WAITING_DELTCB)
	UNIMPLEMENTED();

    tcb->xcpu_status = entry->param[0];
    tcb_set_state (tcb, THREAD_STATE_RUNNING);
    sched_schedule (tcb, sched_default);
}

static void idle_xcpu_delete (tcb_t *tcb, word_t src)
{
    tcb_t *src_tcb = (tcb_t *) src;

    tcb_delete_tcb (tcb);
    xcpu_request_c (tcb_get_cpu (src_tcb), do_xcpu_delete_done, src_tcb, 0);
}

static void do_xcpu_delete (cpu_mb_entry_t * entry)
{
    tcb_t * tcb = entry->tcb;
    tcb_t * current = get_current_tcb();
    tcb_t * src = (tcb_t*)entry->param[0];
    word_t done = 1;

    // migrated meanwhile?
    if (tcb_is_local_cpu (tcb))
    {
	if (current == get_idle_tcb_c() || current == tcb)
	{
	    // make sure we don't run on the deleted thread's ptab
	    space_switch_to_kernel_space (get_current_cpu());
	}

	if (current == tcb)
	{
	    tcb_notify_word2 (get_idle_tcb_c(), (void(*)(word_t,word_t)) idle_xcpu_delete,
			      (word_t) tcb, (word_t) src);
	    sched_schedule (get_idle_tcb_c(), sched_handoff);
	}

	tcb_delete_tcb (tcb);
	done = 0;
    }
    xcpu_request_c (tcb_get_cpu (src), do_xcpu_delete_done, src, done);

    if (!sched_get_accounted_tcb ())
	sched_set_accounted_tcb (current);
}
#endif

void tcb_delete_tcb (tcb_t *self)
{
    ASSERT(tcb_exists (self));

#if defined(CONFIG_SMP)
delete_tcb_retry:
    if ( !tcb_is_local_cpu (self) )
    {
	xcpu_request_c (tcb_get_cpu (self), do_xcpu_delete, self, (word_t)get_current_tcb());
	tcb_set_state (get_current_tcb(), THREAD_STATE_XCPU_WAITING_DELTCB);
	sched_schedule (get_idle_tcb_c(), sched_handoff);

	// wait for re-activation, if not successfull simply retry
	if (get_current_tcb()->xcpu_status)
	    goto delete_tcb_retry;
	return;
    }
#endif

    if ( tcb_is_activated (self) )
    {
	ASSERT(self->utcb);

	// dequeue from ready queue
	sched_deschedule (self);

	// unwind ongoing IPCs
	if (thread_state_is_sending (&self->thread_state) ||
	    thread_state_is_receiving (&self->thread_state))
	    tcb_unwind (self, TCB_UNWIND_ABORT);

	tcb_lock (self);

	// dequeue pending send requests
	while (self->send_head)
	{
	    tcb_dequeue_send (self->send_head, self);
	    // what do we do with these guys?
	}

	tcb_unlock (self);

	// free any used resources
	tcb_resources_free (&self->resources, self);

	// free UTCB
	self->utcb = NULL;

	// make sure that we don't get accounted anymore
	if (sched_get_accounted_tcb () == self)
	    sched_set_accounted_tcb (get_idle_tcb_c());

	// remove from requeue list
	sched_ktcb_delete_tcb (&self->sched_state);
    }

    // clear ids
    self->myself_global = NILTHREAD;
    self->myself_local = ANYLOCALTHREAD;

    tcb_set_space (self, NULL);
    tcb_set_state (self, THREAD_STATE_ABORTED);
    tcb_dequeue_present (self);
}


/**
 * Calculate sender and receiver errorcodes for an aborted IPC.
 */
static void calculate_errorcodes (word_t reason, tcb_t * snd, tcb_t * rcv,
				  word_t * err_s, word_t * err_r)
{
    word_t offset = snd->misc.ipc_copy.copy_length;

    if (space_is_copy_area (snd->misc.ipc_copy.copy_fault))
	offset += (word_t) snd->misc.ipc_copy.copy_fault -
	    (word_t) snd->misc.ipc_copy.copy_start_dst;
    else
	offset += (word_t) snd->misc.ipc_copy.copy_fault -
	    (word_t) snd->misc.ipc_copy.copy_start_src;

    if (reason == TCB_UNWIND_TIMEOUT)
    {
	time_t snd_to = tcb_get_xfer_timeout_snd (snd);
	time_t rcv_to = tcb_get_xfer_timeout_rcv (rcv);

	if (time_lt (rcv_to, snd_to))
	{
	    *err_s = IPC_SND_ERROR (ERR_IPC_XFER_TIMEOUT_PARTNER (offset));
	    *err_r = IPC_RCV_ERROR (ERR_IPC_XFER_TIMEOUT_CURRENT (offset));
	}
	else
	{
	    *err_s = IPC_SND_ERROR (ERR_IPC_XFER_TIMEOUT_CURRENT (offset));
	    *err_r = IPC_RCV_ERROR (ERR_IPC_XFER_TIMEOUT_PARTNER (offset));
	}
    }
    else
    {
	*err_s = IPC_SND_ERROR (ERR_IPC_ABORTED (offset));
	*err_r = IPC_RCV_ERROR (ERR_IPC_ABORTED (offset));
    }
}


#if defined(CONFIG_SMP)

DECLARE_TRACEPOINT (IPC_XCPU_UNWIND);

/**
 * Handler invoked when our IPC partner has aborted/timed out the IPC.
 */
static void do_xcpu_unwind_partner (cpu_mb_entry_t * entry)
{
    tcb_t * tcb = entry->tcb;
    threadid_t partner_id; partner_id.raw = entry->param[0];
    word_t expected_state = entry->param[1];
    msg_tag_t tag; tag.raw = entry->param[2];
    word_t err = entry->param[3];

    if (EXPECT_FALSE (! tcb_is_local_cpu (tcb)))
    {
	// Forward request.
	xcpu_request_many (tcb_get_cpu (tcb), do_xcpu_unwind_partner, tcb,
			   entry->param[0], entry->param[1],
			   entry->param[2], entry->param[3]);
	return;
    }

    {
	threadid_t p = tcb_get_partner (tcb);
	if (EXPECT_FALSE (tcb_get_state (tcb) != expected_state) ||
	    EXPECT_FALSE (threadid_get_raw (&p) != threadid_get_raw (&partner_id)))
	    // Request is outdated.
	    return;
    }

    {
	threadid_t sp = tcb->misc.saved_state[0].partner;
	if (! threadid_is_nilthread (&sp) &&
	    ! (tcb->misc.saved_state[0].state == THREAD_STATE_RUNNING))
	{
	    // We have a nested IPC operation.  Perform another unwind.
	    tcb_restore_state (tcb);
	    word_t e = (err >> 1) & 0x7;
	    tcb_unwind (tcb, (e == 1 || e == 5 || e == 6) ? TCB_UNWIND_TIMEOUT : TCB_UNWIND_ABORT);
	}
	else
	{
	    tcb_set_error_code (tcb, err);
	    tcb_set_tag (tcb, tag);
	    tcb_set_state (tcb, THREAD_STATE_RUNNING);
	}
    }

    // Reactivate thread.
    tcb_notify (tcb, handle_ipc_error);
    sched_schedule (tcb, sched_default);
}

#endif /* CONFIG_SMP */


DECLARE_TRACEPOINT (IPC_UNWIND);

/**
 * Unwinds a thread from an ongoing IPC.
 */
void tcb_unwind (tcb_t *self, word_t reason)
{
    msg_tag_t tag = tcb_get_tag (self);
    word_t cstate;
    tcb_t * partner;

redo_unwind:

    cstate = tcb_get_state (self);
    tcb_set_state (self, THREAD_STATE_RUNNING);
    partner = tcb_get_partner_tcb (self);

    if (cstate == THREAD_STATE_POLLING ||
	cstate == THREAD_STATE_WAITING_FOREVER || cstate == THREAD_STATE_WAITING_TIMEOUT)
    {
	// IPC operation has not yet started.
	sched_ktcb_cancel_timeout (&self->sched_state);

	if (cstate == THREAD_STATE_POLLING)
	{
	    // The thread is enqueued in the send queue of the partner.
	    tcb_lock (partner);
	    tcb_dequeue_send (self, partner);
	    tcb_unlock (partner);
	}
	else
	{
	    // Thread is not yet receiving.  Cancel receive phase.
	    tag = msg_tag_error_tag ();
	}

	{
	    threadid_t sp = self->misc.saved_state[0].partner;
	    if (! threadid_is_nilthread (&sp) &&
		! (self->misc.saved_state[0].state == THREAD_STATE_RUNNING))
	    {
		// We're handling a nested IPC.
		tcb_restore_state (self);
		goto redo_unwind;
	    }
	}

	// Set appropriate error code
	msg_tag_set_error (&tag);
	tcb_set_tag (self, tag);
	{
	    word_t err = (reason == TCB_UNWIND_TIMEOUT) ? ERR_IPC_TIMEOUT : ERR_IPC_CANCELED;
	    tcb_set_error_code (self, (cstate == THREAD_STATE_POLLING) ?
			   IPC_SND_ERROR (err) : IPC_RCV_ERROR (err));
	}
	return;
    }
    else if (cstate == THREAD_STATE_WAITING_TUNNELED_PF)
    {
	// We have tunneled a pagefault to our partner.
	word_t err_s, err_r;
	calculate_errorcodes (reason, self, partner, &err_s, &err_r);
	msg_tag_set_error (&tag);

#if defined(CONFIG_SMP)
	if (! tcb_is_local_cpu (partner))
	{
	    threadid_t g = tcb_get_global_id (self);
	    xcpu_request_many (tcb_get_cpu (partner), do_xcpu_unwind_partner, partner,
			       threadid_get_raw (&g),
			       (word_t) THREAD_STATE_LOCKED_WAITING,
			       tag.raw, err_r);
	}
	else
#endif
	{
	    // Reactivate partner directly
	    tcb_set_error_code (partner, err_r);
	    tcb_set_tag (partner, tag);
	    tcb_set_state (partner, THREAD_STATE_RUNNING);
	    tcb_notify (partner, handle_ipc_error);
	    sched_schedule (partner, sched_default);
	}

	tcb_set_tag (self, tag);
	tcb_set_error_code (self, err_s);
	return;
    }

    else if (cstate == THREAD_STATE_LOCKED_RUNNING_IPC_DONE)
    {
	// Nested pagefault (almost) completed.  No partner.
	tcb_restore_state (self);
	goto redo_unwind;
    }

    else if (cstate == THREAD_STATE_LOCKED_RUNNING)
    {
	// Thread is an active sender.  Abort both threads.
	word_t err_s, err_r;
	calculate_errorcodes (reason, self, partner, &err_s, &err_r);
	msg_tag_set_error (&tag);

#if defined(CONFIG_SMP)
	if (! tcb_is_local_cpu (partner))
	{
	    threadid_t g = tcb_get_global_id (self);
	    xcpu_request_many (tcb_get_cpu (partner), do_xcpu_unwind_partner,
			       partner, threadid_get_raw (&g),
			       (word_t) THREAD_STATE_LOCKED_WAITING,
			       tag.raw, err_r);
	}
	else
#endif
	{
	    threadid_t pp = tcb_get_partner (partner);
	    threadid_t g = tcb_get_global_id (self);
	    if (tcb_get_state (partner) == THREAD_STATE_LOCKED_WAITING &&
		threadid_get_raw (&pp) == threadid_get_raw (&g))
	    {
		tcb_set_error_code (partner, err_r);
		tcb_set_tag (partner, tag);
		tcb_set_state (partner, THREAD_STATE_RUNNING);
		tcb_notify (partner, handle_ipc_error);
		sched_schedule (partner, sched_default);
	    }
	}

	{
	    threadid_t sp = self->misc.saved_state[0].partner;
	    if (! threadid_is_nilthread (&sp) &&
		! (self->misc.saved_state[0].state == THREAD_STATE_RUNNING))
	    {
		tcb_restore_state (self);
		goto redo_unwind;
	    }
	}

	tcb_set_tag (self, tag);
	tcb_set_error_code (self, err_s);
	return;
    }

    else if (cstate == THREAD_STATE_LOCKED_RUNNING_NESTED)
    {
	// Thread is handling tunneled pagefault, IPC not yet started.
	word_t err_s, err_r;
	calculate_errorcodes (reason, partner, self, &err_s, &err_r);
	msg_tag_set_error (&tag);

#if defined(CONFIG_SMP)
	if (! tcb_is_local_cpu (partner))
	{
	    threadid_t g = tcb_get_global_id (self);
	    xcpu_request_many (tcb_get_cpu (partner), do_xcpu_unwind_partner,
			       partner, threadid_get_raw (&g),
			       (word_t) THREAD_STATE_WAITING_TUNNELED_PF,
			       tag.raw, err_s);
	}
	else
#endif
	{
	    if (tcb_get_state (partner) == THREAD_STATE_WAITING_TUNNELED_PF)
	    {
		tcb_set_error_code (partner, err_s);
		tcb_set_tag (partner, tag);
		tcb_set_state (partner, THREAD_STATE_RUNNING);
		tcb_notify (partner, handle_ipc_error);
		sched_schedule (partner, sched_default);
	    }
	}

	tcb_set_tag (self, tag);
	tcb_set_error_code (self, err_r);
	return;
    }

    else if (cstate == THREAD_STATE_LOCKED_WAITING)
    {
	// Thread is an active receiver.  Abort both threads.
	word_t err_s, err_r;
	calculate_errorcodes (reason, partner, self, &err_s, &err_r);
	msg_tag_set_error (&tag);

#if defined(CONFIG_SMP)
	if (! tcb_is_local_cpu (partner))
	{
	    threadid_t g = tcb_get_global_id (self);
	    xcpu_request_many (tcb_get_cpu (partner), do_xcpu_unwind_partner,
			       partner, threadid_get_raw (&g),
			       (word_t) THREAD_STATE_LOCKED_RUNNING,
			       tag.raw, err_s);
	}
	else
#endif
	{
	    if (tcb_get_state (partner) == THREAD_STATE_LOCKED_RUNNING)
	    {
		tcb_set_error_code (partner, err_s);
		tcb_set_tag (partner, tag);
		tcb_set_state (partner, THREAD_STATE_RUNNING);
		tcb_notify (partner, handle_ipc_error);
		sched_schedule (partner, sched_default);
	    }
	}

	{
	    threadid_t sp = self->misc.saved_state[0].partner;
	    if (! threadid_is_nilthread (&sp) &&
		! (self->misc.saved_state[0].state == THREAD_STATE_RUNNING))
	    {
		tcb_restore_state (self);
		goto redo_unwind;
	    }
	}

	tcb_set_tag (self, tag);
	tcb_set_error_code (self, err_r);
	return;
    }

    else if (cstate == THREAD_STATE_XCPU_WAITING_DELTCB ||
	     cstate == THREAD_STATE_XCPU_WAITING_EXREGS)
    {
	// Waiting for xcpu TCB deletion or exregs completion.  Just ignore.
	return;
    }

    else if (cstate == THREAD_STATE_RUNNING)
    {
	// May happen on a redo_unwind of a synthesized IPC that was aborted.
	return;
    }

    WARNING ("Unresolved unwind: tcb=%p state=%p\n", self, cstate);
    return;
}


bool tcb_migrate_to_space (tcb_t *self, space_t * space)
{
    ASSERT(space);
    ASSERT(self->space);
    space_t *old_space = self->space;

    tcb_lock (self);

    if (tcb_is_activated (self))
    {
	// allocate utcb in destination space
	utcb_t * new_utcb = space_allocate_utcb (space, self);
	if (! new_utcb)
	    return false;

	// make sure nobody messes around with the tcb
	tcb_unwind (self, TCB_UNWIND_ABORT);

	memcpy (new_utcb, self->utcb, sizeof (utcb_t));
	self->utcb = new_utcb;
    }

    // remove from old space
    if (space_remove_tcb (old_space, self, get_current_cpu()))
    {
	space_free (old_space);
	space_free_space (old_space);
    }

    // now change the space
    tcb_set_space (self, space);
    space_add_tcb (space, self, get_current_cpu());

    tcb_unlock (self);

    return true;
}

#if defined(CONFIG_SMP)
/**
 * releases all resources held by the thread
 */
static void xcpu_release_thread (tcb_t * tcb)
{
    // thread is on the current CPU
    sched_deschedule (tcb);

    // make sure that we don't get accounted anymore
    if (sched_get_accounted_tcb () == tcb)
	sched_set_accounted_tcb (get_idle_tcb_c());

    // remove from wakeup queue
    if (!queue_state_is_set (&tcb->queue_state, QUEUE_STATE_WAKEUP))
	sched_ktcb_set_timeout_abs (&tcb->sched_state, 0, false);
    else
	sched_ktcb_cancel_timeout (&tcb->sched_state);

    tcb_resources_purge (&tcb->resources, tcb);

    // when migrating IRQ threads disable interrupts
    if (tcb_is_interrupt_thread (tcb))
	migrate_interrupt_start_c (tcb);
}


static void xcpu_put_thread (tcb_t * tcb, word_t processor)
{
    // dequeue all threads from requeue list and continue holding lock
    sched_move_tcb (tcb, (cpuid_t) processor);
}

/**
 * remote handler for tcb_t::migrate_to_processor
 */
static void do_xcpu_set_thread (cpu_mb_entry_t * entry)
{
    if (!entry->tcb)
    {
	enter_kdebug("do_xcpu_set_thread");
	return;
    }

    tcb_migrate_to_processor (entry->tcb, (cpuid_t) entry->param[0]);
}

bool tcb_migrate_to_processor (tcb_t *self, cpuid_t processor)
{
    // check if the thread is already on that processor
    if (processor == tcb_get_cpu (self))
	return true;

    tcb_t * current = get_current_tcb();

    if (EXPECT_FALSE( tcb_get_cpu (self) != get_current_cpu() ))
    {
	// thread not on current CPU (or migrated away meanwhile)
	xcpu_request_c (tcb_get_cpu (self), do_xcpu_set_thread, self, processor);
    }
    else if ( tcb_get_cpu (self) != processor )
    {
	// thread is on local CPU and should be migrated
	xcpu_release_thread (self);

	// if we migrate ourself we use the idle thread to perform the notification
	if (current == self)
	{
	    tcb_notify_word2 (get_idle_tcb_c(), (void(*)(word_t,word_t)) xcpu_put_thread,
			      (word_t) self, (word_t) processor);
	    space_switch_to_kernel_space (get_current_cpu());
	    sched_schedule (get_idle_tcb_c(), sched_handoff);
	}
	else
	{
	    xcpu_put_thread (self, processor);

	    // schedule if we've been running on the thread's timeslice
	    if (!sched_get_accounted_tcb ())
		sched_schedule_current ();
	}
    }

    return true;
}
#endif /* CONFIG_SMP */

/**
 * Handler invoked when IPC errors (aborts or timeouts) occur.
 */
EXTERN_TRACEPOINT(IPC_ERROR);

void handle_ipc_error (void)
{
    tcb_t * current = get_current_tcb ();

    tcb_release_copy_area (current);
    tcb_flags_remove (current, TCB_FLAG_HAS_XFER_TIMEOUT);

    // We're going to skip the last part of the switch_to() function invoked
    // when switching from the current thread.
    if (EXPECT_FALSE (current->resource_bits.resource_bits.maskvalue != 0))
	tcb_resources_load (&current->resources, current);

    if (current->misc.saved_state[0].state == THREAD_STATE_RUNNING)
    {
	// Thread was doing a pagefault IPC.  Restore thread state and return.
	tcb_restore_state (current);
	current->partner = threadid_nilthread (); // sanity
	tcb_return_from_user_interruption (current);
    }
    else
    {
	current->misc.saved_state[0].state = THREAD_STATE_ABORTED; // sanity
	current->misc.saved_state[0].partner = threadid_nilthread ();
	tcb_return_from_ipc (current);
    }

    /* NOTREACHED */
}


/**
 * Handler invoked when an IPC TCB_UNWIND_TIMEOUT has occured.
 */
void handle_ipc_timeout (word_t state)
{
    tcb_t * current = get_current_tcb ();

    // Restore thread state when TCB_UNWIND_TIMEOUT occured
    tcb_set_state (current, state);

    tcb_unwind (current, TCB_UNWIND_TIMEOUT);
    tcb_set_state (current, THREAD_STATE_RUNNING);
    handle_ipc_error ();
}


void tcb_save_state (tcb_t *self)
{
    ASSERT (self->misc.saved_state[IPC_NESTING_LEVEL-1].partner.raw ==
	    threadid_nilthread ().raw);
    ASSERT (self->misc.saved_state[IPC_NESTING_LEVEL-1].state == THREAD_STATE_ABORTED);

    for (word_t l = 1; l < IPC_NESTING_LEVEL; l++)
    {
	for (word_t i = 0; i < IPC_NUM_SAVED_MRS; i++)
	    self->misc.saved_state[l].mr[i] = self->misc.saved_state[l-1].mr[i];
	self->misc.saved_state[l].br0 = self->misc.saved_state[l-1].br0;
	self->misc.saved_state[l].error = self->misc.saved_state[l-1].error;
	self->misc.saved_state[l].partner = self->misc.saved_state[l-1].partner;
	self->misc.saved_state[l].vsender = self->misc.saved_state[l-1].vsender;
	self->misc.saved_state[l].state = self->misc.saved_state[l-1].state;
    }

    for (word_t i = 0; i < IPC_NUM_SAVED_MRS; i++)
	self->misc.saved_state[0].mr[i] = tcb_get_mr (self, i);

    self->misc.saved_state[0].br0 = tcb_get_br (self, 0);
    self->misc.saved_state[0].error = tcb_get_error_code (self);
    self->misc.saved_state[0].partner = self->partner;
    self->misc.saved_state[0].vsender = tcb_get_virtual_sender (self);
    self->misc.saved_state[0].state = tcb_get_state (self);
}

void tcb_restore_state (tcb_t *self)
{
    for (word_t i = 0; i < IPC_NUM_SAVED_MRS; i++)
	tcb_set_mr (self, i, self->misc.saved_state[0].mr[i]);
    tcb_set_br (self, 0, self->misc.saved_state[0].br0);
    self->partner = self->misc.saved_state[0].partner;
    tcb_set_state (self, self->misc.saved_state[0].state);
    tcb_set_error_code (self, self->misc.saved_state[0].error);
    tcb_set_actual_sender (self, self->misc.saved_state[0].vsender);

    for (word_t l = 1; l < IPC_NESTING_LEVEL; l++)
    {
	for (word_t i = 0; i < IPC_NUM_SAVED_MRS; i++)
	    self->misc.saved_state[l-1].mr[i] = self->misc.saved_state[l].mr[i];
	self->misc.saved_state[l-1].br0 = self->misc.saved_state[l].br0;
	self->misc.saved_state[l-1].error = self->misc.saved_state[l].error;
	self->misc.saved_state[l-1].partner = self->misc.saved_state[l].partner;
	self->misc.saved_state[l-1].state = self->misc.saved_state[l].state;
	self->misc.saved_state[l-1].vsender = self->misc.saved_state[l].vsender;
    }

    self->misc.saved_state[IPC_NESTING_LEVEL-1].partner = threadid_nilthread ();
    self->misc.saved_state[IPC_NESTING_LEVEL-1].state = THREAD_STATE_ABORTED;
}

void tcb_send_pagefault_ipc (tcb_t *self, addr_t addr, addr_t ip, int access)
{
    tcb_save_state (self);

    /* generate pagefault message */
    msg_tag_t tag;
    msg_tag_set (&tag, 0, 2, IPC_MR0_PAGEFAULT |
	    ((access == SPACE_ACCESS_READ)      ? (1 << 2) : 0) |
	    ((access == SPACE_ACCESS_WRITE)     ? (1 << 1) : 0) |
	    ((access == SPACE_ACCESS_EXECUTE)   ? (1 << 0) : 0) |
	    ((access == SPACE_ACCESS_READWRITE) ? (1 << 2)+(1 << 1) : 0));

    /* create acceptor for whole address space */
    acceptor_t acceptor;
    acceptor.raw = 0;
    {
	fpage_t cm = fpage_complete_mem ();
	acceptor_set_rcv_window (&acceptor, cm);
    }

    tcb_set_tag (self, tag);
    tcb_set_mr (self, 1, (word_t)addr);
    tcb_set_mr (self, 2, (word_t)ip);
    tcb_set_br (self, 0, acceptor.raw);

    {
	threadid_t pager = tcb_get_pager (self);
	tag = tcb_do_ipc (self, pager, pager, timeout_never ());
    }
    if (msg_tag_is_error (&tag))
    {
	printf("result tag = %p, ip = %p, addr = %p, errcode = %p\n",
	       tag.raw, ip, addr, tcb_get_error_code (self));
	enter_kdebug("pagefault IPC error");
    }

    tcb_restore_state (self);
}

bool tcb_send_preemption_ipc (tcb_t *self)
{
    u64_t time = sched_get_current_time ();
    threadid_t to;
    acceptor_t acceptor;
    msg_tag_t tag;

    tcb_save_state (self);

    tag = msg_tag_preemption_tag ();

    /* generate preemption message */
    to = sched_ktcb_get_scheduler (&self->sched_state);

    tcb_set_mr (self, 1, (word_t) time);
    tcb_set_mr (self, 2, (word_t)((time >> (BITS_WORD-1)) >> 1)); // Avoid gcc warn

    acceptor.raw = tcb_get_br (self, 0);

    tcb_set_tag (self, tag);
    tcb_set_br (self, 0, acceptor.raw);

    tag = tcb_do_ipc (self, to, sched_ktcb_get_scheduler (&self->sched_state), timeout_never ());

    tcb_restore_state (self);

    if (msg_tag_is_error (&tag))
	enter_kdebug("preemption IPC error");

    return msg_tag_is_error (&tag);
}


#if defined(CONFIG_X_CTRLXFER_MSG)

/* C form of tcb_t::ctrlxfer.  The bitmask operators it used are gone with the
   C++ template: `mask += n' set bit n and `mask -= n' cleared it (see the
   pre-migration generic/bitmask.h), so both become explicit shifts here.  The
   register-transfer members are the glue-supplied get_ctrlxfer_regs /
   set_ctrlxfer_regs tables, called through &tcb->arch instead of C++
   pointer-to-member syntax; they take the MR index by pointer and advance it. */
word_t tcb_ctrlxfer (tcb_t *self, tcb_t *dst, msg_item_t item, word_t src_idx,
		     word_t dst_idx, bool src_mr, bool dst_mr)
{
    word_t num_regs = 0;
    word_t ctrlxfer_item_id;
    msg_item_t ctrlxfer_item;
    ctrlxfer_mask_t ctrlxfer_mask;

    ctrlxfer_mask.maskvalue = 0;

    if (tcb_flags_is_set (self, TCB_FLAG_KERNEL_CTRLXFER_MSG))
    {
	/*
	 * on kernel we have inserted a single dummy ctrlxfer item with the
	 * fault id encoded to reduce the number of saved MRs space; we get the
	 * "real" items by inspecting the fault bitmasks
	 */
	ctrlxfer_mask = tcb_get_fault_ctrlxfer_items (self, msg_item_get_ctrlxfer_id (&item));
	ctrlxfer_item_id = (word_t) lsb (ctrlxfer_mask.maskvalue);
	ctrlxfer_item = ctrlxfer_fault_item (ctrlxfer_item_id);
	TRACE_CTRLXFER_DETAILS( "ctrlxfer kernel msg fault %d mask %x",
				msg_item_get_ctrlxfer_id (&item),
				(word_t) ctrlxfer_mask.maskvalue );
    }
    else
    {
	ctrlxfer_item_id = 1;
	ctrlxfer_mask.maskvalue |= (1UL << ctrlxfer_item_id);
	ctrlxfer_item = item;
    }

    do
    {
	word_t id = msg_item_get_ctrlxfer_id (&ctrlxfer_item);
	word_t num = 0;
	word_t mask = msg_item_get_ctrlxfer_mask (&ctrlxfer_item);

	ctrlxfer_mask_hwregs (id, &mask);
	TRACE_CTRLXFER_DETAILS( "ctrlxfer id %d %s mask %x ", id,
				ctrlxfer_get_idname (id), mask );

	if (src_mr)
	{
	    if (dst_mr)
	    {
		word_t reg;

		tcb_set_mr (dst, dst_idx++, ctrlxfer_item.raw);

		for (reg = (word_t) lsb (mask); mask != 0;
		     mask >>= lsb (mask) + 1, reg += (word_t) lsb (mask) + 1, num++)
		{
		    TRACE_CTRLXFER_DETAILS( "\t (m%06d->m%06d) -> %08x", src_idx+1,
					    dst_idx, tcb_get_mr (self, src_idx+1) );
		    tcb_set_mr (dst, dst_idx++, tcb_get_mr (self, src_idx++ + 1));
		}
	    }
	    else
	    {
		// skip ctrlxfer item
		src_idx++;
		/* transfer from src mrs to dst frame */
		num += set_ctrlxfer_regs[id] (&dst->arch, id, mask, self, &src_idx);
	    }
	}
	else
	{
	    if (dst_mr)
	    {
		tcb_set_mr (dst, dst_idx++, ctrlxfer_item.raw);
		/* transfer from src frame to dst mrs */
		num += get_ctrlxfer_regs[id] (&self->arch, id, mask, dst, &dst_idx);
	    }
	    else
		TRACEF("Ignore frame2frame ctrlxfer");
	}

	num_regs += 1 + num;
	ctrlxfer_mask.maskvalue &= ~(1UL << ctrlxfer_item_id);
	ctrlxfer_item_id = (word_t) lsb (ctrlxfer_mask.maskvalue);
	ctrlxfer_item = ctrlxfer_fault_item (ctrlxfer_item_id);

    } while (ctrlxfer_mask.maskvalue);

    tcb_flags_remove (self, TCB_FLAG_KERNEL_CTRLXFER_MSG);

    return num_regs;
}

#endif /* defined(CONFIG_X_CTRLXFER_MSG) */


/**********************************************************************
 *             global V4 thread management
 **********************************************************************/

static tcb_t * SECTION(".init")
create_root_server (threadid_t dest_tid, threadid_t scheduler_tid,
		    threadid_t pager_tid, fpage_t utcb_area,
		    fpage_t kip_area, word_t utcb_location,
		    word_t ip, word_t sp)
{
    tcb_t * tcb = tcb_allocate (dest_tid);
    space_t * space = space_allocate_space ();

    ASSERT(space);
    ASSERT(tcb);

    tcb_arch_init_root_server (tcb, space, ip, sp);

    tcb_create_inactive (tcb, dest_tid, scheduler_tid, sktcb_root);
    space_init (space, utcb_area, kip_area);

    /* set the space */
    tcb_set_space (tcb, space);
    space_add_tcb (space, tcb, get_current_cpu());

    tcb_set_utcb_location (tcb, utcb_location);

    /* activate the guy */
    if (!tcb_activate (tcb, &thread_return, pager_tid))
	panic("failed to activate root server\n");

    /* set instruction and stack pointer */
    tcb_set_user_ip (tcb, (addr_t) ip);
    tcb_set_user_sp (tcb, (addr_t) sp);

    /* and off we go... */
    tcb_set_state (tcb, THREAD_STATE_RUNNING);

    sched_schedule (tcb, sched_current);
    return tcb;
}


SYS_THREAD_CONTROL (threadid_t dest_tid, threadid_t space_tid,
		    threadid_t scheduler_tid, threadid_t pager_tid,
		    word_t utcb_location)
{
    tcb_t * current = get_current_tcb();

    // Check privilege
    if (EXPECT_FALSE (! is_privileged_space_c (get_current_space_c ())))
    {
	tcb_set_error_code (current, ENO_PRIVILEGE);
	return_thread_control(0);
    }

    // Check for valid thread id
    if (EXPECT_FALSE (! threadid_is_global (&dest_tid)))
    {
	tcb_set_error_code (current, EINVALID_SPACE);
	return_thread_control(0);
    }

    tcb_t * dest_tcb = tcb_get_tcb (dest_tid);

    /* interrupt thread id ? */
    if (threadid_get_threadno (&dest_tid) < thread_info_get_system_base (&get_kip()->thread_info))
    {
	if (EXPECT_TRUE (thread_control_interrupt_c (dest_tid, pager_tid)))
	    return_thread_control (1);
	tcb_set_error_code (current, EINVALID_THREAD);
	return_thread_control (0);
    }

    /* do not allow the user to mess with kernel threads */
    if (EXPECT_FALSE (threadid_get_threadno (&dest_tid) <
		      thread_info_get_user_base (&get_kip()->thread_info)))
    {
	tcb_set_error_code (current, EINVALID_THREAD);
	return_thread_control (0);
    }

    if (threadid_is_nilthread (&space_tid))
    {
	if (dest_tcb == current)
	{
	    // do not allow deletion of ourself
	    tcb_set_error_code (current, EINVALID_THREAD);
	    return_thread_control(0);
	}
	else if (tcb_exists (dest_tcb))
	{
	    space_t * space = dest_tcb->space;
	    cpuid_t cpu = tcb_get_cpu (dest_tcb);

	    tcb_delete_tcb (dest_tcb);

	    if (space_remove_tcb (space, dest_tcb, cpu))
	    {
		// was the last thread
		space_free (space);
		space_free_space (space);
	    }
	    tcb_deallocate (dest_tid);

	    // schedule if we've been running on this thread's timeslice
	    if (!sched_get_accounted_tcb ())
		sched_schedule_current ();
	}

	return_thread_control(1);
    }
    else
    {
	/* thread creation/modification */
	dest_tcb = tcb_allocate (dest_tid);
	// get the tcb of the space
	tcb_t * space_tcb = tcb_get_tcb (space_tid);

	if (tcb_exists (dest_tcb))
	{
	    if (utcb_location != ~0UL)
	    {
		// do not allow modification of UTCB locations of activated threads
		if (tcb_is_activated (dest_tcb) ||
		    ! tcb_check_utcb_location (dest_tcb, utcb_location))
		{
		    tcb_set_error_code (current, EUTCB_AREA);
		    return_thread_control (0);
		}
		tcb_set_utcb_location (dest_tcb, utcb_location);
	    }

	    // the hardest part first - space modifications
	    {
		threadid_t sg = tcb_get_global_id (space_tcb);
		if (EXPECT_FALSE (threadid_get_raw (&sg) != threadid_get_raw (&space_tid)))
		{
		    tcb_set_error_code (current, EINVALID_SPACE);
		    return_thread_control (0);
		}
	    }

	    space_t * space = space_tcb->space;
	    if (dest_tcb->space != space)
	    {
		// space migration
		if (tcb_is_activated (dest_tcb) &&
		    EXPECT_FALSE (! (space_is_initialized (space) &&
				     tcb_check_utcb_location
				     (space_tcb, tcb_get_utcb_location (dest_tcb)))))
		{
		    tcb_set_error_code (current, EUTCB_AREA);
		    return_thread_control (0);
		}
		if (EXPECT_FALSE (! tcb_migrate_to_space (dest_tcb, space)))
		{
		    tcb_set_error_code (current, ENO_MEM);
		    return_thread_control (0);
		}
	    }

	    if (!threadid_is_nilthread (&pager_tid))
	    {
		/* if the thread was inactive, setting the pager activates it */
		if (!tcb_is_activated (dest_tcb))
		{
		    if (! tcb_check_utcb_location (dest_tcb, tcb_get_utcb_location (dest_tcb)))
		    {
			tcb_set_error_code (current, EUTCB_AREA);
			return_thread_control (0);
		    }
		    if (! tcb_activate (dest_tcb, &thread_startup, pager_tid))
		    {
			tcb_set_error_code (current, ENO_MEM);
			return_thread_control (0);
		    }
		    fake_wait_for_startup (dest_tcb, pager_tid);
		}
		else
		    tcb_set_pager (dest_tcb, pager_tid);
	    }

	    if (!threadid_is_nilthread (&scheduler_tid))
		sched_ktcb_set_scheduler (&dest_tcb->sched_state, scheduler_tid);

	    // change global id
	    {
		threadid_t dg = tcb_get_global_id (dest_tcb);
		if (threadid_get_raw (&dg) != threadid_get_raw (&dest_tid))
		    tcb_set_global_id (dest_tcb, dest_tid);
	    }

	    return_thread_control(1);
	}
	else
	{
	    /* on creation of a new space scheduler must not be nilthread */
	    if (EXPECT_FALSE (threadid_is_nilthread (&scheduler_tid)))
	    {
		tcb_set_error_code (current, EINVALID_THREAD);
		return_thread_control (0);
	    }

	    /* if not created in a fresh space make sure the space id is valid */
	    {
		threadid_t sg = tcb_get_global_id (space_tcb);
		if (EXPECT_FALSE ((threadid_get_raw (&dest_tid) != threadid_get_raw (&space_tid)) &&
				  (threadid_get_raw (&sg) != threadid_get_raw (&space_tid))))
		{
		    tcb_set_error_code (current, EINVALID_SPACE);
		    return_thread_control (0);
		}
	    }

	    // For actively created threads, make sure space is initialized.
	    if (! threadid_is_nilthread (&pager_tid))
	    {
		// Check for initialized space
		if (threadid_get_raw (&dest_tid) == threadid_get_raw (&space_tid) ||
		    (! space_is_initialized (space_tcb->space)))
		{
		    tcb_set_error_code (current, EINVALID_SPACE);
		    return_thread_control (0);
		}

		// Check for valid UTCB location
		if (utcb_location == ~0UL ||
		    (! tcb_check_utcb_location (space_tcb, utcb_location)))
		{
		    tcb_set_error_code (current, EUTCB_AREA);
		    return_thread_control (0);
		}
	    }

	    /* ok, we can create the thread */
	    tcb_create_inactive (dest_tcb, dest_tid, scheduler_tid, sktcb_user);

	    if (utcb_location != ~0UL)
		tcb_set_utcb_location (dest_tcb, utcb_location);

	    space_t * space;
	    if (threadid_get_raw (&dest_tid) != threadid_get_raw (&space_tid))
		space = space_tcb->space;
	    else
		space = space_allocate_space ();

	    ASSERT(space);

	    // set the space for the tcb
	    tcb_set_space (dest_tcb, space);
	    space_add_tcb (space, dest_tcb, get_current_cpu());

	    // if pager is not nil the thread directly goes into an IPC
	    if (! threadid_is_nilthread (&pager_tid) )
	    {
		if (!tcb_activate (dest_tcb, thread_startup, pager_tid))
		    UNIMPLEMENTED();
		fake_wait_for_startup (dest_tcb, pager_tid);
	    }
	    return_thread_control(1);
	}
    }

    /* NOTREACHED */
    spin_forever_c (0);
}


static utcb_t kernel_utcb;

void SECTION(".init") init_kernel_threads (void)
{
    // Initialize the user base.  Currently simply leave some space.
    thread_info_set_user_base (&get_kip ()->thread_info,
	thread_info_get_system_base (&get_kip ()->thread_info) + 32);

    // Create a dummy kernel thread.
    threadid_t ktid;
    threadid_set_global_id (&ktid, thread_info_get_system_base (&get_kip ()->thread_info), 1);
    tcb_t * tcb = tcb_get_tcb (ktid);
    tcb_create_kernel_thread (tcb, ktid, &kernel_utcb, sktcb_lo);
    tcb_set_state (tcb, THREAD_STATE_ABORTED);
}


/**
 * initializes the root servers
 */
void SECTION(".init") init_root_servers (void)
{
    TRACE_INIT ("Initializing root servers\n");

    word_t ubase = thread_info_get_user_base (&get_kip()->thread_info);
    tcb_t * tcb;

    fpage_t utcb_area = fpage_nilpage (), kip_area = fpage_nilpage ();
    word_t size_utcb = 0;

    /* calculate size of UTCB area for root servers */
    {
	word_t need = utcb_info_get_utcb_size (&get_kip()->utcb_info) * ROOT_MAX_THREADS;
	word_t minsz = utcb_info_get_minimal_size (&get_kip()->utcb_info);
	if (need < minsz) need = minsz;
	while ((1U << size_utcb) < need)
	    size_utcb++;
    }

    fpage_set (&utcb_area, ROOT_UTCB_START, size_utcb, 0, 0, 0);
    fpage_set (&kip_area, ROOT_KIP_START, kip_area_info_get_size_log2 (&get_kip()->kip_area_info),
	       0, 0, 0);

    TRACE_INIT ("root-servers: utcb_area: %p (%dKB), kip_area: %p (%dKB)\n",
		utcb_area.raw, fpage_get_size (&utcb_area) / 1024,
		kip_area.raw, fpage_get_size (&kip_area) / 1024);

    // stop if system has no sigma0
    if (mem_region_is_empty (&get_kip()->sigma0.mem_region))
	panic ("Sigma0's memory region is empty, "
	       "system will not be functional.  Halting.\n");

    threadid_t sigma0, sigma1, root_server;
    threadid_set_global_id (&sigma0, ubase, ROOT_VERSION);
    threadid_set_global_id (&sigma1, ubase+1, ROOT_VERSION);
    threadid_set_global_id (&root_server, ubase+2, ROOT_VERSION);

    TRACE_INIT ("Creating sigma0 (%t)\n", TID(sigma0));
    tcb = create_root_server (sigma0, root_server, NILTHREAD, utcb_area, kip_area,
			      ROOT_UTCB_START, get_kip()->sigma0.ip, get_kip()->sigma0.sp);

    sigma0_space = tcb->space;

    /* start sigma1 */
    if (!mem_region_is_empty (&get_kip()->sigma1.mem_region))
    {
	TRACE_INIT ("Creating sigma1 (%t)\n", TID(sigma1));
	tcb = create_root_server (sigma1, root_server, sigma0, utcb_area, kip_area,
				  ROOT_UTCB_START, get_kip()->sigma1.ip, get_kip()->sigma1.sp);
	sigma1_space = tcb->space;
    }

    /* start root task */
    if (!mem_region_is_empty (&get_kip()->root_server.mem_region))
    {
	TRACE_INIT ("Creating root server (%t)\n", TID(root_server));
	tcb = create_root_server (root_server, root_server, sigma0, utcb_area, kip_area,
				  ROOT_UTCB_START, get_kip()->root_server.ip, get_kip()->root_server.sp);
	roottask_space = tcb->space;
    }
}
