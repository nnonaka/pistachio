/*********************************************************************
 *
 * Copyright (C) 2002-2003, 2006-2010,  Karlsruhe University
 *
 * File path:     glue/v4-x86/debug.c
 * Description:   Debugging support
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
 * $Id: debug.cc,v 1.12 2006/06/19 08:01:59 stoess Exp $
 *
 ********************************************************************/

#if defined(CONFIG_DEBUG)

#define X86_EXC_KDB

#include <debug.h>
#include <ctors.h>
#include <kdb/tracepoints.h>
#include INC_API(kernelinterface.h)
#include INC_API(tcb.h)
#include INC_API(smp.h)
#include INC_API(cpu.h)
#include INC_ARCH(traps.h)
#include INC_ARCH(trapgate.h)
#include INC_ARCH(apic.h)
#include INC_GLUE(debug.h)
#include INC_PLAT(nmi.h)


static void do_return_from_kdb(void);

/* asm-named tcb_t method (its C++ decl in tcb.h is invisible to C). */
void tcb_create_kernel_thread (tcb_t *, threadid_t, utcb_t *, sktcb_type_e);

#if defined(DEBUG_LOCK)
DECLARE_SPINLOCK(printf_spin_lock);
DECLARE_TRACEPOINT(DEBUG_LOCK);
static bool sync_dbg_enter = false;

void sync_debug (word_t address)
{

    if (get_current_tcb() == get_kdebug_tcb())
	ENABLE_TRACEPOINT(DEBUG_LOCK, ~0U, 0U);

    if (!sync_dbg_enter)
    {
	spinlock_unlock (&printf_spin_lock);
	sync_dbg_enter = true;
 	TRACEPOINT(DEBUG_LOCK, "CPU %d, tcb %t, spinlock BUG (lock %x) @ %x\n",
		   get_current_cpu(), get_current_tcb(),
		   address, __builtin_return_address((0)));
	enter_kdebug("spinlock BUG");
    }
    sync_dbg_enter = false;
}
#endif


/* debug_param_t now comes from glue/v4-x86/debug.h (dual-repped there). */

/* Per-CPU KDB control block (was class cpu_kdb_t). */
typedef struct cpu_kdb_t
{
    whole_tcb_t __kdb_tcb;
    utcb_t	__kdb_utcb;

    debug_param_t param;
    tcb_t *user_tcb, *kdb_tcb;
} cpu_kdb_t;

cpu_kdb_t cpu_kdb UNIT("cpulocal");

/* Was the cpu_kdb_t constructor with CTORPRIO(CTORPRIO_CPU, 1); in C we place
   the init function in the same .init_array priority slot (init_priority
   65535-(CTORPRIO_CPU+1) = 35534) so it runs at the identical point. */
static void cpu_kdb_ctor (void) __attribute__((constructor(35534)));
static void cpu_kdb_ctor (void)
{
    cpu_kdb.user_tcb = NULL;
    cpu_kdb.kdb_tcb = (tcb_t *) &cpu_kdb.__kdb_tcb;
    tcb_create_kernel_thread (get_idle_tcb_c(), NILTHREAD, &cpu_kdb.__kdb_utcb, sktcb_hi);
    tcb_set_cpu (cpu_kdb.kdb_tcb, get_current_cpu());
    tcb_set_space (cpu_kdb.kdb_tcb, get_kernel_space_c());
}

static void cpu_kdb_do_enter_kdebug (x86_exceptionframe_t *frame, const word_t exception)
{
    if (get_current_tcb() == get_kdebug_tcb())
	return;

    void (*entry)(word_t) = (void (*)(word_t)) get_kip()->kdebug_entry;
    void (*exit)(void) = do_return_from_kdb;

    cpu_kdb.kdb_tcb->stack = tcb_get_stack_top (cpu_kdb.kdb_tcb);
    tcb_notify (cpu_kdb.kdb_tcb, exit);
    tcb_notify_word (cpu_kdb.kdb_tcb, entry, (word_t) &cpu_kdb.param);

    cpu_kdb.param.exception = exception;
    cpu_kdb.param.frame = frame;
    cpu_kdb.param.space = (get_current_space_c() ? get_current_space_c() : get_kernel_space_c());
    cpu_kdb.param.tcb = get_current_tcb();

    cpu_kdb.user_tcb = get_current_tcb();

    tcb_switch_to (get_current_tcb(), cpu_kdb.kdb_tcb);
}

tcb_t *get_kdebug_tcb() { return cpu_kdb.kdb_tcb; }


void do_enter_kdebug(x86_exceptionframe_t *frame, const word_t exception)
{
    cpu_kdb_do_enter_kdebug (frame, X86_EXC_DEBUG);
}

void do_return_from_kdb(void)
{
    ASSERT(get_current_tcb() == get_kdebug_tcb());
    tcb_switch_to (get_current_tcb(), cpu_kdb.user_tcb);
}

X86_EXCNO_ERRORCODE(exc_breakpoint, X86_EXC_BREAKPOINT)
{
     cpu_kdb_do_enter_kdebug (frame, X86_EXC_BREAKPOINT);
}

X86_EXCNO_ERRORCODE(exc_debug, X86_EXC_DEBUG)
{
    cpu_kdb_do_enter_kdebug (frame, X86_EXC_DEBUG);
}

X86_EXCNO_ERRORCODE(exc_nmi, X86_EXC_NMI)
{
    cpu_kdb_do_enter_kdebug (frame, X86_EXC_NMI);
}
#if defined(CONFIG_SMP)
X86_EXCNO_ERRORCODE(exc_debug_ipi, 0)
{

}
#endif

#undef X86_EXC_KDB

#endif /* CONFIG_DEBUG */
