/*********************************************************************
 *
 * Copyright (C) 2002-2010,  Karlsruhe University
 *
 * File path:     api/v4/accessors.c
 * Description:   C accessors over the shared api/v4 structures.
 *
 * These were written during the C++ -> C migration and placed in
 * glue/v4-x86/space.c and glue/v4-x86/thread.c, but they touch nothing
 * architecture-specific: fpage_t's mem.x and raw, tcb_t's utcb and queue
 * links, time_t's mantissa/exponent -- all declared in api/v4 headers.  Every
 * port needs them, so they live here rather than being duplicated per
 * architecture.
 *
 * The genuinely architecture-dependent ones stay in the glue: anything
 * touching the kernel stack layout, page directories, ASIDs or the copy area.
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
#include <debug.h>
#include INC_API(types.h)
#include INC_API(fpage.h)
#include INC_API(thread.h)
#include INC_API(tcb.h)
#include INC_API(space.h)
#include INC_API(schedule.h)
#include INC_API(interrupt.h)	/* thread_control_interrupt */
#include INC_GLUE(space.h)
#include INC_GLUE(map.h)	/* arch_map_fpage / arch_unmap_fpage */
#include INC_GLUE(debug.h)	/* DEBUG_SCREEN, for spin_forever_c */

#if defined(CONFIG_DEBUG)
/* Defined per architecture; declared extern in glue/v4-x86/thread.c too. */
extern tcb_t *global_present_list;
extern spinlock_t present_list_lock;
#endif

/* ---- from glue/v4-x86/space.c ---- */
/* The IO-flexpage build overrides this with a complete-arch window; that
   version lives in glue/v4-x86/io_space.c (notes §116). */
#if !defined(CONFIG_X86_IO_FLEXPAGES)
fpage_t acceptor_get_arch_specific_rcvwindow (acceptor_t *self, tcb_t *dest)
{ (void) self; (void) dest; fpage_t fp; fp.raw = 0; return fp; }
#endif

addr_t fpage_address (fpage_t fp, word_t size)
{ return (addr_t) ((word_t) fpage_get_base (&fp) & ~((1UL << size) - 1)); }

word_t fpage_base_mask (fpage_t fp, word_t size)
{ return ((~0UL) >> ((sizeof (word_t) * 8) - fpage_get_size_log2 (&fp))) &
	 ((size == 0 ? (~0UL) : ~((~0UL) >> ((sizeof (word_t) * 8) - size)))); }

fpage_t fpage_complete_mem (void)				{ fpage_t r; r.raw = 0; r.mem.x.size = 1; return r; }

/* fpage_t::complete_arch has no generic caller; glue/v4-x86/io_space.h keeps
   its own INLINE under CONFIG_X86_IO_FLEXPAGES (notes §116). */

/* Every accessor below that the C++ wrote as `is_mempage() ? mem : arch.'
   keeps both branches.  With no architecture-specific flexpages
   arch_fpage_is_valid_page() is a constant false and the arch half folds
   away; under CONFIG_X86_IO_FLEXPAGES it is what routes an IO fpage.  The
   first conversion of this file kept only the mem half, which is why the
   iofp configuration handed an IO fpage to space_map_fpage -- see §131. */

addr_t fpage_get_address (fpage_t *self)
{ return fpage_is_mempage (self)
	? (addr_t) (((word_t) self->mem.x.base << 10) & (~0UL << self->mem.x.size))
	: arch_fpage_get_address (&self->arch); }

addr_t fpage_get_base (fpage_t *self)
{ return fpage_is_mempage (self)
	? (addr_t) ((word_t) self->mem.x.base << 10)
	: arch_fpage_get_base (&self->arch); }

word_t fpage_get_rwx (fpage_t *self)				{ return self->raw & 7; }

word_t fpage_get_size (fpage_t *self)
{ return fpage_is_mempage (self) ? 1UL << self->mem.x.size
				 : arch_fpage_get_size (&self->arch); }

word_t fpage_get_size_log2 (fpage_t *self)
{ return fpage_is_mempage (self)
	? ((self->mem.x.size == 1 && self->mem.x.base == 0) ? sizeof (word_t) * 8 : self->mem.x.size)
	: arch_fpage_get_size_log2 (&self->arch); }

bool fpage_is_addr_in_fpage (fpage_t *self, addr_t addr);

bool   fpage_is_archpage (fpage_t *self)			{ return arch_fpage_is_valid_page (&self->arch); }

bool   fpage_is_complete_fpage (fpage_t *self)
{ return (fpage_is_mempage (self) && self->mem.x.size == 1 && self->mem.x.base == 0) ||
	 (arch_fpage_is_valid_page (&self->arch) && arch_fpage_is_complete_page (&self->arch)); }

bool   fpage_is_execute (fpage_t *self)				{ return self->mem.x.execute; }

bool   fpage_is_mempage (fpage_t *self)				{ return ! arch_fpage_is_valid_page (&self->arch); }

bool   fpage_is_nil_fpage (fpage_t *self)			{ return self->raw == 0; }

bool   fpage_is_overlapping (fpage_t *self, fpage_t other)
{
    addr_t sa, oa;

    if (fpage_is_complete_fpage (self)) return true;
    sa = fpage_get_address (self);
    oa = fpage_get_address (&other);
    if (oa < sa) return addr_offset (oa, fpage_get_size (&other)) > sa;
    return addr_offset (sa, fpage_get_size (self)) > oa;
}

bool   fpage_is_range_in_fpage (fpage_t *self, addr_t start, addr_t end)
{
    addr_t a;

    if (fpage_is_complete_fpage (self)) return true;
    a = fpage_get_address (self);
    return (a <= start && addr_offset (a, fpage_get_size (self)) >= end);
}

bool   fpage_is_range_overlapping (fpage_t *self, addr_t start, addr_t end)
{
    addr_t a;

    if (fpage_is_complete_fpage (self)) return true;
    a = fpage_get_address (self);
    if (start < a) return end > a;
    return addr_offset (a, fpage_get_size (self)) > start;
}

bool   fpage_is_read (fpage_t *self)				{ return self->mem.x.read; }

bool   fpage_is_rwx (fpage_t *self)				{ return self->mem.x.read && self->mem.x.write && self->mem.x.execute; }

bool   fpage_is_write (fpage_t *self)				{ return self->mem.x.write; }

fpage_t fpage_nilpage (void)					{ fpage_t r; r.raw = 0; return r; }

void   fpage_set (fpage_t *self, word_t base, word_t size, bool read, bool write, bool exec)
{
    if (EXPECT_FALSE (arch_fpage_is_valid_page (&self->arch) == false))
    {
	word_t abase = (base & (~0UL << size)) >> 10;
	self->raw = 0;
	self->mem.x.base = abase & (~0UL >> (BITS_WORD - L4_FPAGE_BASE_BITS));
	self->mem.x.size = size & 0x3f;
	self->mem.x.read = read;
	self->mem.x.write = write;
	self->mem.x.execute = exec;
    }
    else
	arch_fpage_set (&self->arch, base, size, read, write, exec);
}

void   fpage_set_rwx (fpage_t *self, word_t rwx)		{ self->raw = (self->raw & ~(word_t) 7) | (rwx & 7); }

void   fpage_set_rwx_all (fpage_t *self)			{ self->mem.x.read = 1; self->mem.x.write = 1; self->mem.x.execute = 1; }


bool mem_region_is_empty (mem_region_t *self)			{ return self->high == 0; }


bool space_is_sigma0 (space_t *space)				{ return is_sigma0_space (space); }


bool space_is_user_area_addr (addr_t addr)			{ return space_is_user_area (addr); }

bool space_is_user_area_fpage (fpage_t fpage)
{ return space_is_user_area (fpage_get_address (&fpage)) &&
	 space_is_user_area (addr_offset (fpage_get_address (&fpage), fpage_get_size (&fpage) - 1)); }


void   tcb_init_saved_state (tcb_t *self)
{
    for (int l = 0; l < IPC_NESTING_LEVEL; l++)
    {
	self->misc.saved_state[l].state = THREAD_STATE_ABORTED;
	self->misc.saved_state[l].partner = threadid_nilthread ();
    }
}

void   tcb_sched_set_timeout (tcb_t *self, time_t t)
{
    if ((t.time.type == 1))
	UNIMPLEMENTED ();
    sched_ktcb_set_timeout_abs (&self->sched_state,
				sched_get_current_time () + time_get_microseconds (&t), true);
}

bool   time_lt (time_t a, time_t b)
{
    u64_t curtime = sched_get_current_time ();
    u64_t l_to, r_to;

    if ((a.time.type == 1))
	UNIMPLEMENTED ();
    else if (time_is_never (&a))
	l_to = ~0UL;
    else
	l_to = curtime + time_get_microseconds (&a);

    if ((b.time.type == 1))
	UNIMPLEMENTED ();
    else if (time_is_never (&b))
	r_to = ~0UL;
    else
	r_to = curtime + time_get_microseconds (&b);

    return l_to < r_to;
}

/* ---- from glue/v4-x86/thread.c ---- */
/* api/v4/ipcx.c cannot see the arch map interface -- it is INC_GLUE(map.h),
   an empty INLINE pair on an architecture without arch-specific flexpages and
   an extern pair in glue/v4-x86/io_space.c under CONFIG_X86_IO_FLEXPAGES -- so
   these forward to whichever is in scope here.  They were written as no-ops,
   which is right only for the first case and left the IO flexpage mapping
   silently dropped in the second; see §131. */
void   arch_map_fpage_c (tcb_t *src, fpage_t snd_fpage, word_t snd_base, tcb_t *dst, fpage_t rcv_fpage, bool grant)
{ arch_map_fpage (src, snd_fpage, snd_base, dst, rcv_fpage, grant); }

void   arch_unmap_fpage_c (tcb_t *from, fpage_t fpage, bool flush)
{ arch_unmap_fpage (from, fpage, flush); }

tcb_t * get_idle_tcb_c (void)			{ extern tcb_t *__idle_tcb; return __idle_tcb; }

void   handle_ipc_timeout_c (word_t state)	{ handle_ipc_timeout (state); }

void   spin_forever_c (int pos)
{
#if defined(CONFIG_SPIN_WHEELS) && defined(CONFIG_DEBUG)
    while (1)
	((u16_t *) (DEBUG_SCREEN))[pos] += 1;
#else
    int dummy = 0;
    while (1)
	dummy = (dummy + 1) % 32;
#endif
}

void tcb_dequeue_present (tcb_t *self)
{
#if defined(CONFIG_DEBUG)
    spinlock_lock (&present_list_lock);
    DEQUEUE_LIST (global_present_list, self, present_list);
    spinlock_unlock (&present_list_lock);
#endif
}

void tcb_dequeue_send (tcb_t *self, tcb_t *t)
{
    ASSERT (queue_state_is_set (&self->queue_state, QUEUE_STATE_SEND));
    DEQUEUE_LIST (t->send_head, self, send_list);
    queue_state_clear (&self->queue_state, QUEUE_STATE_SEND);
}

void tcb_enqueue_present (tcb_t *self)
{
#if defined(CONFIG_DEBUG)
    spinlock_lock (&present_list_lock);
    ENQUEUE_LIST_TAIL (global_present_list, self, present_list);
    spinlock_unlock (&present_list_lock);
#endif
}

/* Queue / present-list / lock wrappers. */
void tcb_enqueue_send (tcb_t *self, tcb_t *t)
{
    ASSERT (!queue_state_is_set (&self->queue_state, QUEUE_STATE_SEND));
    ENQUEUE_LIST_TAIL (t->send_head, self, send_list);
    queue_state_set (&self->queue_state, QUEUE_STATE_SEND);
}

word_t tcb_get_error_code (tcb_t *self)			{ return utcb_get_error_code (self->utcb); }

threadid_t tcb_get_exception_handler (tcb_t *self)	{ return utcb_get_exception_handler (self->utcb); }

threadid_t tcb_get_pager (tcb_t *self)			{ return utcb_get_pager (self->utcb); }

threadid_t tcb_get_saved_partner (tcb_t *self)		{ return self->misc.saved_state[0].partner; }

msg_tag_t tcb_get_tag (tcb_t *self)			{ msg_tag_t tag; tag.raw = utcb_get_mr (self->utcb, 0); return tag; }

word_t tcb_get_user_handle (tcb_t *self)		{ return utcb_get_user_defined_handle (self->utcb); }

threadid_t tcb_get_virtual_sender (tcb_t *self)		{ return utcb_get_virtual_sender (self->utcb); }

time_t tcb_get_xfer_timeout_rcv (tcb_t *self)		{ timeout_t t = utcb_get_xfer_timeout (self->utcb); return timeout_get_rcv (&t); }

time_t tcb_get_xfer_timeout_snd (tcb_t *self)		{ timeout_t t = utcb_get_xfer_timeout (self->utcb); return timeout_get_snd (&t); }

bool   tcb_is_local_cpu (tcb_t *self)			{ return get_current_cpu () == tcb_get_cpu (self); }

void tcb_lock (tcb_t *self)				{ spinlock_lock (&self->tcb_lock); }

void tcb_lock_init (tcb_t *self)			{ spinlock_init (&self->tcb_lock, 0); }

void   tcb_set_actual_sender (tcb_t *self, threadid_t tid) { utcb_set_virtual_sender (self->utcb, tid); }

void   tcb_set_error_code (tcb_t *self, word_t err)	{ utcb_set_error_code (self->utcb, err); }

void   tcb_set_exception_handler (tcb_t *self, threadid_t tid) { utcb_set_exception_handler (self->utcb, tid); }

void   tcb_set_global_id (tcb_t *self, threadid_t tid)
{ self->myself_global = tid; ASSERT (self->utcb); utcb_set_my_global_id (self->utcb, tid); }

void   tcb_set_pager (tcb_t *self, threadid_t tid)	{ utcb_set_pager (self->utcb, tid); }

void   tcb_set_tag (tcb_t *self, msg_tag_t tag)		{ utcb_set_mr (self->utcb, 0, tag.raw); }

void   tcb_set_user_handle (tcb_t *self, word_t handle)	{ utcb_set_user_defined_handle (self->utcb, handle); }

void tcb_unlock (tcb_t *self)				{ spinlock_unlock (&self->tcb_lock); }

/* Free-function C wrappers used by the C api/v4 files. */
bool thread_control_interrupt_c (threadid_t irq_tid, threadid_t handler_tid)
{ return thread_control_interrupt (irq_tid, handler_tid); }

/* time_t helper (time_t is C-visible; time_lt lives in space.cc as it needs
   the scheduler clock). */
u64_t  time_get_microseconds (time_t *self)	{ return (1 << self->time.exponent) * self->time.mantissa; }

bool   fpage_is_addr_in_fpage (fpage_t *self, addr_t addr)
{ return fpage_is_range_in_fpage (self, addr, (addr_t) ((word_t) addr + sizeof (addr_t))); }
