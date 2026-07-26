/*********************************************************************
 *
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, Jan Stoess, IBM Corporation
 *
 * File path:     api/v4/space.cc
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

#include <debug.h>
#include <kmemory.h>
#include <kdb/tracepoints.h>
#include INC_API(space.h)
#include INC_API(smp.h)
#include INC_API(tcb.h)
#include INC_API(fpage.h)
#include INC_API(generic-archmap.h)
#include INC_API(kernelinterface.h)
#include INC_API(schedule.h)
#include INC_API(syscalls.h)
#include INC_API(threadstate.h)
#include INC_GLUE(syscalls.h)
#include INC_GLUE(map.h)


space_t * sigma0_space;
space_t * sigma1_space;
space_t * roottask_space;

DECLARE_TRACEPOINT (PAGEFAULT_USER);
DECLARE_TRACEPOINT (PAGEFAULT_KERNEL);
DECLARE_TRACEPOINT (PAGEFAULT_TUNNEL);
DECLARE_TRACEPOINT (SYSCALL_SPACE_CONTROL);
DECLARE_TRACEPOINT (SYSCALL_UNMAP);
EXTERN_TRACEPOINT(IPC_DETAILS);
EXTERN_TRACEPOINT(DEBUG);

DECLARE_KMEM_GROUP (kmem_space);


#if defined(CONFIG_SMP)

void tunnel_pagefault (word_t addr);

/**
 * Handler invoked to resume the IPC copy opearation after a tunneled
 * pagefault has been completed.
 */
static void do_xcpu_tunnel_pf_done (cpu_mb_entry_t * entry)
{
    tcb_t * tcb = entry->tcb;

    if (! tcb_is_local_cpu (tcb))
    {
	xcpu_request_c (tcb_get_cpu (tcb), do_xcpu_tunnel_pf_done, tcb, 0);
	return;
    }

    // Restore state for current thread.
    tcb_set_state (tcb, THREAD_STATE_LOCKED_RUNNING);
    sched_schedule (tcb, sched_default);
}

/**
 * Handler invoked to tunnel a IPC copy pagefault to the destination
 * thread (running on another CPU).
 */
static void do_xcpu_tunnel_pf (cpu_mb_entry_t * entry)
{
    tcb_t * tcb = entry->tcb;
    word_t addr = entry->param[0];

    if (! tcb_is_local_cpu (tcb))
    {
	xcpu_request_c (tcb_get_cpu (tcb), do_xcpu_tunnel_pf, tcb, addr);
	return;
    }

    // Set up a pagefault notify for destination thread.
    tcb_notify_word (tcb, tunnel_pagefault, addr);
    tcb_set_state (tcb, THREAD_STATE_LOCKED_RUNNING_NESTED);
    sched_schedule (tcb, sched_default);
}
#endif


/**
 * Handler invoked when IPC copy page-fault has been tunnelled to
 * current space.  Invoke a page-fault IPC to pager, and resume the
 * IPC copy operation.
 *
 * @param addr			fault address
 */
void tunnel_pagefault (word_t addr)
{
    tcb_t * current = get_current_tcb ();
    tcb_t * partner = tcb_get_partner_tcb (current);

    TRACEPOINT (PAGEFAULT_TUNNEL, "tunnelled page fault @ %p (current=%t, partner=%t)\n",
		addr, current, partner);

    tcb_send_pagefault_ipc (current, (addr_t) addr, (addr_t) ~0UL, SPACE_ACCESS_WRITE);
    tcb_set_state (current, THREAD_STATE_LOCKED_WAITING);

#if defined(CONFIG_SMP)
    if (! tcb_is_local_cpu (partner))
    {
	// Partner on remote CPU.  Need to send wake-up request.
	xcpu_request_c (tcb_get_cpu (partner), do_xcpu_tunnel_pf_done, partner, 0);
	sched_schedule (get_idle_tcb_c (), sched_handoff);
    }
    else
#endif
    {
	// Partner on same CPU.  Switch back directly.
	tcb_set_state (partner, THREAD_STATE_LOCKED_RUNNING);
	sched_schedule (partner, sched_ipcblk);
    }
}


/**
 * Handle transfer timeouts.  We do not set up any timeouts if both
 * xfer timeouts are infinite.  If any xfer timeout is zero, we abort
 * IPC immediately.  Otherwise, we find the earliest timeout and set
 * up a timeout for the sender thread.
 *
 * @param sender		sender thread
 */
static void handle_xfer_timeouts (tcb_t * sender)
{
#warning Handle priority inversion for xfer timeouts

    // Skip timeout calculation if already in wakeup queue.
    if (tcb_flags_is_set (sender, TCB_FLAG_HAS_XFER_TIMEOUT))
	return;

    tcb_t * partner = tcb_get_partner_tcb (sender);

    threadid_t snd_partner = tcb_get_partner (sender);
    ASSERT (! threadid_is_nilthread (&snd_partner));
    ASSERT (tcb_get_state (sender) == THREAD_STATE_LOCKED_RUNNING);
    ASSERT (tcb_get_state (partner) == THREAD_STATE_LOCKED_WAITING);

    time_t snd_to = tcb_get_xfer_timeout_snd (sender);
    time_t rcv_to = tcb_get_xfer_timeout_rcv (partner);

    if (time_is_zero (&snd_to) || time_is_zero (&rcv_to))
    {
	// Timeout immediately.
	handle_ipc_timeout_c (THREAD_STATE_LOCKED_RUNNING);
    }
    else if (time_is_never (&snd_to) && time_is_never (&rcv_to))
    {
	// No timeouts.
	return;
    }

    time_t min = time_lt (snd_to, rcv_to) ? snd_to : rcv_to;

    TRACEPOINT(IPC_DETAILS, "ipc setting xfer timeout %dus current time %ld",
	       (word_t) time_get_microseconds (&min),
	       (word_t) sched_get_current_time());

    // Set timeout on the sender side.
    tcb_sched_set_timeout (sender, min);
    tcb_flags_add (sender, TCB_FLAG_HAS_XFER_TIMEOUT);
}

void space_handle_pagefault (space_t * self, addr_t addr, addr_t ip, int access, bool kernel)
{
    tcb_t * current = get_current_tcb();
    bool user_area = space_is_user_area_addr (addr);

    if (user_area || (!kernel))
    {
        if (!kernel)
            TRACEPOINT (PAGEFAULT_USER, "user %s pagefault at %x, ip=%p\n",
                        access == SPACE_ACCESS_WRITE     ? "write" :
                        access == SPACE_ACCESS_READ	   ? "read"  :
                        access == SPACE_ACCESS_EXECUTE   ? "execute" :
                        access == SPACE_ACCESS_READWRITE ? "read/write" :
                        "unknown",
                        addr, ip);
        else
            TRACEPOINT (PAGEFAULT_KERNEL, "kernel %s pagefault in user area at %x, ip=%p\n",
                        access == SPACE_ACCESS_WRITE     ? "write" :
                        access == SPACE_ACCESS_READ	   ? "read"  :
                        access == SPACE_ACCESS_EXECUTE   ? "execute" :
                        access == SPACE_ACCESS_READWRITE ? "read/write" :
                        "unknown",
                        addr, ip);

#warning sigma0-check in default pagefault handler.
	/* VU: with software loaded tlbs that could be handled elsewhere...*/
	if (EXPECT_TRUE( !space_is_sigma0 (tcb_get_space (current)) ))
	{
	    if (kernel &&
		tcb_get_state (current) != THREAD_STATE_LOCKED_RUNNING)
	    {
		printf("kernel access raised user pagefault @ %p, ip=%p, "
		       "space=%p\n", addr, ip, self);
		enter_kdebug("kpf");
	    }

	    if (!user_area)
	    {
		printf("%t pf @ %p, ip=%p\n", current, addr, ip);
		enter_kdebug("user touches kernel area");
	    }

	    if (tcb_get_state (current) == THREAD_STATE_LOCKED_RUNNING)
	    {
		// Pagefault during IPC copy.  Initiate xfer timeout
		// counters before handling pagefault.
		current->misc.ipc_copy.copy_fault = addr;
		handle_xfer_timeouts (current);
	    }

	    tcb_send_pagefault_ipc (current, addr, ip, access);
	}
	else
	{
	    if (user_area)
		space_map_sigma0 (self, addr);
	    else
	    {
		printf("sigma0 accessed kernel space @ %p, ip=%p - deny\n",
		       addr, ip);
		enter_kdebug("sigma0 kpf");
	    }
	}
	return;
    }
    else
    {
	/* fault in kernel area */
        TRACEPOINT (PAGEFAULT_KERNEL, "kernel %s pagefault at %x, ip=%p\n",
                        access == SPACE_ACCESS_WRITE     ? "write" :
                        access == SPACE_ACCESS_READ	   ? "read"  :
                        access == SPACE_ACCESS_EXECUTE   ? "execute" :
                        access == SPACE_ACCESS_READWRITE ? "read/write" :
                        "unknown",
                        addr, ip);

	if (space_sync_kernel_space (self, addr))
	    return;

	if (space_is_tcb_area (addr))
	{
	    /* write access to tcb area? */
	    if (access == SPACE_ACCESS_WRITE)
		space_allocate_tcb (self, addr);
	    else
		space_map_dummy_tcb (self, addr);
	    return;

	}
	else if (space_is_copy_area (addr))
	{
	    // Fault in copy area.  Tunnel pagefault through partner.
	    current->misc.ipc_copy.copy_fault = addr;
	    handle_xfer_timeouts (current);

	    // On PF tunneling we temporarily set the current thread
	    // into waiting for partner.
	    tcb_set_state (current, THREAD_STATE_WAITING_TUNNELED_PF);

	    tcb_t * partner = tcb_get_tcb (tcb_get_partner (current));
	    word_t faddr = (word_t) tcb_copy_area_real_address (current, addr);

#if defined(CONFIG_SMP)
	    if (! tcb_is_local_cpu (partner))
	    {
		// Partner on remote CPU.  Need to send xcpu request.
		xcpu_request_c (tcb_get_cpu (partner), do_xcpu_tunnel_pf,
				partner, faddr);
		sched_schedule (get_idle_tcb_c (), sched_handoff);
	    }
	    else
#endif
	    {
		// Partner on same CPU.  Just switch directly.
		tcb_set_state (partner, THREAD_STATE_LOCKED_RUNNING_NESTED);
		tcb_notify_word (partner, tunnel_pagefault, faddr);
		sched_schedule (partner, sched_handoff);
	    }
	    return;
	}
    }
    TRACEF("tcb %t cpu %d unhandled pagefault @ %p, %p\n", current, get_current_cpu(), addr, ip);

    enter_kdebug("unhandled pagefault");

    tcb_set_state (current, THREAD_STATE_HALTED);
    sched_schedule (get_idle_tcb_c (), sched_handoff);
    printf("wrong access - unable to recover\n");
    spin_forever_c (1);

}

SYS_SPACE_CONTROL (threadid_t space_tid, word_t control, fpage_t kip_area,
		   fpage_t utcb_area, threadid_t redirector_tid)
{
    TRACEPOINT (SYSCALL_SPACE_CONTROL,
		"SYS_SPACE_CONTROL: space=%t, control=%p, kip_area=%p, "
		"utcb_area=%p, redir=%t\n",  TID (space_tid),
		control, kip_area.raw, utcb_area.raw, TID (redirector_tid));

    // Check privilege
    if (EXPECT_FALSE (! is_privileged_space_c (get_current_space_c ())))
    {
	tcb_set_error_code (get_current_tcb (), ENO_PRIVILEGE);
	return_space_control(0, 0);
    }

    // Check for valid space id
    if (EXPECT_FALSE (! threadid_is_global (&space_tid)))
    {
	tcb_set_error_code (get_current_tcb (), EINVALID_SPACE);
	return_space_control(0, 0);
    }

    tcb_t * space_tcb = tcb_get_tcb (space_tid);
    threadid_t gid = tcb_get_global_id (space_tcb);
    if (EXPECT_FALSE (threadid_get_raw (&gid) != threadid_get_raw (&space_tid)))
    {
	tcb_set_error_code (get_current_tcb (), EINVALID_SPACE);
	return_space_control(0, 0);
    }

    space_t * space = tcb_get_space (space_tcb);

    if (! space_is_initialized (space))
    {
	/*
	 * Space does not exist.  Create it.
	 */

	if ((utcb_info_get_minimal_size (&get_kip()->utcb_info) > fpage_get_size (&utcb_area)) ||
	    (! space_is_user_area_fpage (utcb_area)))
	{
	    // Invalid UTCB area
	    tcb_set_error_code (get_current_tcb (), EUTCB_AREA);
	    return_space_control(0, 0);
	}
	else if ((kip_area_info_get_size (&get_kip()->kip_area_info) > fpage_get_size (&kip_area)) ||
		 (! space_is_user_area_fpage (kip_area)) ||
		 fpage_is_overlapping (&kip_area, utcb_area))
	{
	    // Invalid KIP area
	    tcb_set_error_code (get_current_tcb (), EKIP_AREA);
	    return_space_control(0, 0);
	}
	else
	{
	    /* ok, everything seems fine, now setup the space */
	    space_init (space, utcb_area, kip_area);
	}
    }

    word_t old_control = space_space_control (space, control, kip_area, utcb_area, redirector_tid);

    return_space_control (1, old_control);

    spin_forever_c (0);
}

SYSCALL_ATTR("unmap") void sys_unmap(word_t control)
{
    fpage_t fpage;
    word_t num = control & 0x3f;
    bool flush = control & (1 << 6) ? true : false;

    TRACEPOINT(SYSCALL_UNMAP, "SYS_UNMAP: control=0x%x (num=%d, flush=%d)\n",
	       control, num, flush);

#warning VU: adapt to new msg_tag scheme
    for (word_t i = 0; i <= num; i++)
    {
	fpage.raw = tcb_get_mr (get_current_tcb (), i);

	if (fpage_is_mempage (&fpage))
	{
	    space_t *space = get_current_space_c ();
	    fpage = space_unmap_fpage (space, fpage, flush, false);
	}
	else if (fpage_is_archpage (&fpage))
	    arch_unmap_fpage_c (get_current_tcb (), fpage, flush);

	tcb_set_mr (get_current_tcb (), i, fpage.raw);
    }
    return_unmap();
}

void space_free (space_t * self)
{
#if defined(HAVE_ARCH_FREE_SPACE)
    space_arch_free (self);
#endif

    // Unmap everything, including KIP and UTCB
    fpage_t fp = fpage_complete_mem ();
    fpage_set_rwx_all (&fp);
    space_unmap_fpage (self, fp, true, true);
}
