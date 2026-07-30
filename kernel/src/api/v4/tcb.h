/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, Jan Stoess, IBM Corporation
 *                
 * File path:     api/v4/tcb.h
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
 * $Id$
 *                
 ********************************************************************/
#ifndef __API__V4__TCB_H__
#define __API__V4__TCB_H__

#include <debug.h>

#include INC_API(cpu.h)
#include INC_API(types.h)
#include INC_API(queuestate.h)
#include INC_API(queueing.h)
#include INC_API(threadstate.h)
#include INC_API(space.h)
#include INC_API(resources.h)
#include INC_API(thread.h)
#include INC_API(preempt.h)
#include INC_API(fpage.h)
#include INC_API(ipc.h)
#include INC_API(sktcb.h)

/* implementation specific functions */
#if defined(CONFIG_X_CTRLXFER_MSG)
#include INC_GLUE(ipc.h)
struct arch_ktcb_t; typedef struct arch_ktcb_t arch_ktcb_t;
/* Were pointers-to-member of arch_ktcb_t; in C they are plain function
   pointers taking the receiver first, and the word_t& out-parameter is a
   pointer. */
typedef word_t (*get_ctrlxfer_regs_t)(struct arch_ktcb_t *self, word_t id, word_t mask, tcb_t *dst_utcb, word_t *dst_mr);
typedef word_t (*set_ctrlxfer_regs_t)(struct arch_ktcb_t *self, word_t id, word_t mask, tcb_t *src_utcb, word_t *src_mr);
#endif

/* implementation specific functions */
#include INC_GLUE(ktcb.h)
#include INC_GLUE(utcb.h)
#if defined(CONFIG_X_CTRLXFER_MSG)
#include INC_GLUE(ipc.h)
#endif


struct space_t;
typedef struct space_t space_t;

/* Hoisted out of tcb_t (C forbids a typedef inside a struct). */
typedef union {
	struct {
	    struct 
	    {
		/* IPC copy */
		word_t		mr[IPC_NUM_SAVED_MRS];
		word_t		br0;
		threadid_t	partner;
		threadid_t	vsender;
		word_t		state;
		word_t		error;
	    } saved_state[IPC_NESTING_LEVEL];

	    struct {
		word_t		copy_length;
		addr_t		copy_start_src;
		addr_t		copy_start_dst;
		addr_t		copy_fault;
	    } ipc_copy;
	};
	struct {
	    /* Exchange registers */
	    word_t		control;
	    word_t		sp;
	    word_t		ip;
	    word_t		flags;
	    threadid_t		pager;
	    word_t		user_handle;
	} exregs;
} misc_tcb_t;

/* handle_ipc_error is defined in C (thread.c) and called from C++ (exregs.cc)
   + used as a tcb_t friend below, so declare it with C linkage first. */
BEGIN_DECLS
void handle_ipc_error (void);
END_DECLS

/**
 * tcb_t: kernel thread control block
 */
struct tcb_t
{
#define TCB_UNWIND_ABORT	1
#define TCB_UNWIND_TIMEOUT	2
    /* do not delete this TCB_START_MARKER */

    // have relatively static values here
    threadid_t		myself_global;
    threadid_t		myself_local;

    cpuid_t		cpu;
    utcb_t *		utcb;
    
    thread_state_t 	thread_state;
    threadid_t		partner;

    resource_bits_t	resource_bits;
    word_t *		stack;
    /* VU: pdir_cache should be architecture-specific!!! */
    word_t		pdir_cache;

    queue_state_t	queue_state;

    /* queues and scheduling state */
    ringlist_tcb_t	present_list;
    ringlist_tcb_t	send_list;
    tcb_t *		send_head;
    sched_ktcb_t	sched_state;

    spinlock_t		tcb_lock;

#if defined(CONFIG_SMP)
    ringlist_tcb_t	xcpu_list;
    cpuid_t		xcpu;
    word_t		xcpu_status;
    lockstate_t		lock_state;
#endif

    /* pager etc */
    space_t *		space;

#if defined(CONFIG_X_CTRLXFER_MSG)
    /* ARCH_KTCB_FAULT_MAX comes from INC_GLUE(ktcb.h), included above. */
    ctrlxfer_mask_t	fault_ctrlxfer[4+ARCH_KTCB_FAULT_MAX];
#endif

    bitmask_word_t	flags;
    arch_ktcb_t		arch;

    misc_tcb_t		misc;
    thread_resources_t	resources;

    word_t		kernel_stack[0];
    /* do not delete this TCB_END_MARKER */
};

/* union to allow allocation of tcb including stack */
typedef union _whole_tcb_t {
    u8_t pad[KTCB_SIZE];
} whole_tcb_t __attribute__((aligned(KTCB_SIZE)));


__attribute__ ((const)) INLINE tcb_t * addr_to_tcb (addr_t addr)
{
    return (tcb_t *) ((word_t) addr & KTCB_MASK);
}

#if defined(CONFIG_STATIC_TCBS)
/* Static TCBs: the linear pointer array lives in api/v4/thread.c. */
extern tcb_t * tcb_array[TOTAL_KTCBS];

INLINE bool tcb_is_tcb (addr_t addr)
{
    tcb_t *tcb = addr_to_tcb (addr);
    word_t i;

    for (i = 0; i < TOTAL_KTCBS; i++)
	if (tcb_array[i] == tcb)
	    return true;
    return false;
}
#else
INLINE bool tcb_is_tcb (addr_t addr)
{
    return space_is_tcb_area (addr);
}
#endif



/*
 * include glue header file -- included in both C and C++ so that C files can
 * reach its C-safe parts (get_current_tcb() etc.); its C++-only tcb_t methods
 * are guarded within the glue header itself.
 */
#include INC_GLUE(tcb.h)

/* C free-function accessors (mirror the like-named tcb_t methods, which stay
   for C++ callers). Add more here as C files come to need them. */
INLINE threadid_t tcb_get_local_id (const tcb_t *self) { return self->myself_local; }

/* Mirror of tcb_t::get_tcb. */
#if defined(CONFIG_STATIC_TCBS)
INLINE tcb_t * tcb_get_tcb (threadid_t tid)
{
    return tcb_array[threadid_get_threadno (&tid) & VALID_THREADNO_MASK];
}
#else
INLINE tcb_t * tcb_get_tcb (threadid_t tid)
{
    return (tcb_t *) ((KTCB_AREA_START) +
	((threadid_get_threadno (&tid) & VALID_THREADNO_MASK) * KTCB_SIZE));
}
#endif

/* C accessors for tcb_t data members (private in C++, but plain fields in C).
   The non-trivial methods (get_mr, notify, send_pagefault_ipc, ...) are wrapped
   in thread.cc; scheduler/xcpu wrappers live in schedule.cc/smp.h. */
INLINE cpuid_t    tcb_get_cpu (const tcb_t *self)		{ return self->cpu; }
INLINE threadid_t tcb_get_global_id (const tcb_t *self)		{ return self->myself_global; }
INLINE threadid_t tcb_get_partner (const tcb_t *self)		{ return self->partner; }
INLINE space_t *  tcb_get_space (const tcb_t *self)		{ return self->space; }
INLINE utcb_t *   tcb_get_utcb (const tcb_t *self)		{ return self->utcb; }
/* get_saved_state's default argument was level 0. */
INLINE word_t     tcb_get_saved_state (const tcb_t *self)	{ return self->misc.saved_state[0].state; }
/* Matching setters; saved_state[0] is the nesting level the non-nested IPC
   paths use. */
INLINE void       tcb_set_saved_state (tcb_t *self, word_t state)
	{ self->misc.saved_state[0].state = state; }
INLINE void       tcb_set_saved_partner (tcb_t *self, threadid_t tid)
	{ self->misc.saved_state[0].partner = tid; }
/* preempt_flags/cop_flags live in the UTCB; go through the accessors so that
   x86 compatibility mode can dispatch between the 32- and 64-bit UTCBs. */
INLINE word_t     tcb_get_cop_flags (const tcb_t *self)		{ return utcb_get_cop_flags (self->utcb); }
INLINE threadid_t tcb_get_intended_receiver (const tcb_t *self)	{ return utcb_get_intended_receiver (self->utcb); }
INLINE preempt_flags_t tcb_get_preempt_flags (const tcb_t *self)
{
    preempt_flags_t flags;
    flags.raw = utcb_get_preempt_flags (self->utcb);
    return flags;
}
INLINE word_t     tcb_get_state (const tcb_t *self)		{ return self->thread_state.state; }
INLINE void       tcb_set_state (tcb_t *self, word_t s)		{ self->thread_state.state = s; }
INLINE tcb_t *    tcb_get_partner_tcb (const tcb_t *self)	{ return tcb_get_tcb (self->partner); }
INLINE void       tcb_set_partner (tcb_t *self, threadid_t tid)	{ self->partner = tid; }
/* IRQ handler is stored in the scheduler field of the sched-ktcb. */
INLINE void       tcb_set_irq_handler (tcb_t *self, threadid_t tid) { sched_ktcb_set_scheduler (&self->sched_state, tid); }
INLINE threadid_t tcb_get_irq_handler (tcb_t *self)		{ return sched_ktcb_get_scheduler (&self->sched_state); }

#if defined(CONFIG_STATIC_TCBS)
/* Static-KTCB forms; defined in api/v4/thread.c, which owns tcb_array. */
tcb_t *    tcb_allocate (threadid_t dest);
void       tcb_deallocate (threadid_t dest);
#else
/* Dynamic-KTCB allocate/deallocate: the TCB area is demand-paged, so
   allocation just touches the page and clears the stack. */
INLINE tcb_t * tcb_allocate (threadid_t dest)
    { tcb_t *tcb = tcb_get_tcb (dest); tcb->kernel_stack[0] = 0; return tcb; }
INLINE void tcb_deallocate (threadid_t dest)			{ (void) dest; }
#endif

/* flags is a bitmask_word_t; poke its maskvalue directly (see bitmask.h). */
#define TCB_FLAG_HAS_XFER_TIMEOUT	0	/* tcb_t::has_xfer_timeout */
#define TCB_FLAG_SCHEDULE_IN_PROGRESS	1	/* tcb_t::schedule_in_progress */
#define TCB_FLAG_KERNEL_CTRLXFER_MSG	2	/* tcb_t::kernel_ctrlxfer_msg */
#if defined(CONFIG_X_CTRLXFER_MSG)
/* tcb_ctrlxfer was tcb_t::ctrlxfer; the definition lives in api/v4/thread.c,
   inside the same CONFIG_X_CTRLXFER_MSG guard as this declaration. */
word_t tcb_ctrlxfer (tcb_t *self, tcb_t *dst, msg_item_t item, word_t src_idx,
		     word_t dst_idx, bool src_mr, bool dst_mr);
void   tcb_set_fault_ctrlxfer_items (tcb_t *self, word_t fault, ctrlxfer_mask_t mask);
ctrlxfer_mask_t tcb_get_fault_ctrlxfer_items (tcb_t *self, word_t fault);
/* tcb_append_ctrlxfer_item is declared per-architecture: powerpc defines it
   INLINE in glue/v4-powerpc/tcb.h, x86 out of line in glue/v4-x86/thread.c
   (declared in x32/ktcb.h).  A declaration here would clash with the former. */
#if defined(CONFIG_DEBUG)
void   tcb_dump_ctrlxfer_state (tcb_t *self, bool extended);
#endif
#endif
INLINE bool tcb_flags_is_set (const tcb_t *self, word_t bit)
    { return (self->flags.maskvalue & (1UL << bit)) != 0; }
INLINE void tcb_flags_add (tcb_t *self, word_t bit)
    { self->flags.maskvalue |= (1UL << bit); }
INLINE void tcb_flags_remove (tcb_t *self, word_t bit)
    { self->flags.maskvalue &= ~(1UL << bit); }
INLINE bool tcb_is_activated (const tcb_t *self)		{ return self->utcb != 0; }
INLINE bool tcb_exists (const tcb_t *self)			{ return self->space != 0; }

/* Wrappers for the non-trivial tcb_t methods (defined in thread.cc), so C
   files (api/v4/space.c, ...) can drive them. access is space_t::access_e
   (a signed int enum), passed as int. */
BEGIN_DECLS
word_t tcb_get_mr (tcb_t *self, word_t index);
void   tcb_set_mr (tcb_t *self, word_t index, word_t value);
void   tcb_notify_word (tcb_t *self, void (*func)(word_t), word_t arg);
void   tcb_send_pagefault_ipc (tcb_t *self, addr_t addr, addr_t ip, int access);
addr_t tcb_copy_area_real_address (tcb_t *self, addr_t addr);
void   tcb_set_error_code (tcb_t *self, word_t err);
bool   tcb_is_local_cpu (tcb_t *self);
time_t tcb_get_xfer_timeout_snd (tcb_t *self);
time_t tcb_get_xfer_timeout_rcv (tcb_t *self);
void   tcb_sched_set_timeout (tcb_t *self, time_t t);
/* time_t helpers wrapped in C++ (get_microseconds/operator<), plus a few free
   functions that are C++-only inlines; all defined in thread.cc. */
u64_t  time_get_microseconds (time_t *self);
bool   time_lt (time_t a, time_t b);
tcb_t * get_idle_tcb_c (void);
tcb_t * get_dummy_tcb_c (void);
void   handle_ipc_timeout_c (word_t state);
/* arch/utcb accessor wrappers for api/v4/thread.c (defined in
   glue/v4-x86/thread.cc, which stays C++). */
msg_tag_t  tcb_do_ipc (tcb_t *self, threadid_t to, threadid_t from, timeout_t timeout);
void       tcb_return_from_ipc (tcb_t *self);
void       tcb_return_from_user_interruption (tcb_t *self);
addr_t     tcb_get_user_ip (tcb_t *self);
addr_t     tcb_get_user_sp (tcb_t *self);
void       tcb_set_user_ip (tcb_t *self, addr_t ip);
void       tcb_set_user_sp (tcb_t *self, addr_t sp);
word_t     tcb_get_user_flags (tcb_t *self);
void       tcb_set_user_flags (tcb_t *self, word_t flags);
word_t     tcb_get_user_handle (tcb_t *self);
void       tcb_set_user_handle (tcb_t *self, word_t handle);
void       tcb_arch_init_root_server (tcb_t *self, space_t *space, word_t ip, word_t sp);
void       tcb_init_stack (tcb_t *self);
void       tcb_create_startup_stack (tcb_t *self, void (*func)(void));
msg_tag_t  tcb_get_tag (tcb_t *self);
void       tcb_set_tag (tcb_t *self, msg_tag_t tag);
word_t     tcb_get_br (tcb_t *self, word_t index);
void       tcb_set_br (tcb_t *self, word_t index, word_t value);
threadid_t tcb_get_pager (tcb_t *self);
void       tcb_set_pager (tcb_t *self, threadid_t tid);
void       tcb_set_exception_handler (tcb_t *self, threadid_t tid);
threadid_t tcb_get_virtual_sender (tcb_t *self);
void       tcb_set_actual_sender (tcb_t *self, threadid_t tid);
word_t     tcb_get_utcb_location (tcb_t *self);
void       tcb_set_global_id (tcb_t *self, threadid_t tid);
word_t     tcb_get_error_code (tcb_t *self);
bool       thread_control_interrupt_c (threadid_t irq_tid, threadid_t handler_tid);
void       tcb_set_cpu (tcb_t *self, cpuid_t cpu);
void       tcb_set_utcb_location (tcb_t *self, word_t loc);
void       tcb_set_space (tcb_t *self, space_t *space);
void       tcb_init_saved_state (tcb_t *self);
void       tcb_dequeue_send (tcb_t *self, tcb_t *t);
void       tcb_enqueue_send (tcb_t *self, tcb_t *t);
void       tcb_copy_mrs (tcb_t *self, tcb_t *dest, word_t start, word_t count);
threadid_t tcb_get_saved_partner (tcb_t *self);
word_t *   tcb_get_stack_top (tcb_t *self);
threadid_t tcb_get_exception_handler (tcb_t *self);
void       tcb_switch_to (tcb_t *self, tcb_t *current);
void       tcb_init_tcbs (void);
void       tcb_enqueue_present (tcb_t *self);
void       tcb_dequeue_present (tcb_t *self);
void       tcb_lock_init (tcb_t *self);
void       tcb_lock_state_init (tcb_t *self);
void       tcb_lock (tcb_t *self);
void       tcb_unlock (tcb_t *self);
void       tcb_notify (tcb_t *self, void (*func)(void));
void       tcb_notify_word2 (tcb_t *self, void (*func)(word_t, word_t), word_t a1, word_t a2);
void       tcb_release_copy_area (tcb_t *self);
void       tcb_adjust_for_copy_area (tcb_t *self, tcb_t *dst, addr_t *saddr, addr_t *daddr);
void       migrate_interrupt_start_c (tcb_t *tcb);
bool   is_privileged_space_c (space_t *space);
void   spin_forever_c (int pos);
/* asm-named tcb_t methods (the C++ decls above carry the __asm__ labels). */
bool   tcb_migrate_to_processor (tcb_t *self, cpuid_t processor);
bool   tcb_is_interrupt_thread (tcb_t *self);
END_DECLS


/* Defined in C (api/v4/thread.c) but called from C++ (init.cc) or used as
   function pointers, so they need C linkage.  handle_ipc_error keeps its
   friend declaration above (it is used only within thread.c). */
BEGIN_DECLS
void handle_ipc_timeout (word_t state);
void thread_return (void);
void init_root_servers (void);
void init_kernel_threads (void);
END_DECLS

#endif /* !__API__V4__TCB_H__ */

