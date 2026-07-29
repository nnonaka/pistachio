/*********************************************************************
 *                
 * Copyright (C) 2003-2007,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/x64/x32comp/syscalls.c
 * Description:   syscall dispatcher for 32-bit programs
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
 * $Id: syscalls.cc,v 1.5 2006/10/21 02:02:35 reichelt Exp $
 *                
 ********************************************************************/
#include <debug.h>
#include <kdb/tracepoints.h>
#include INC_API(kernelinterface.h)
#include INC_API(thread.h)
#include INC_API(tcb.h)
#include INC_GLUE_SA(offsets.h)
#include INC_GLUE_SA(x32comp/syscalls.h)
#include INC_GLUE_SA(x32comp/user.h)

FEATURESTRING ("compatibility_mode");

#include INC_GLUE_SA(x32comp/kernelinterface.h)

x86_x64_sysret_t syscall_dispatcher_32 (word_t arg1,  /* RDI */
					word_t arg2,  /* RSI */
					word_t arg3,  /* RDX */
					word_t uip,   /* RCX */
					word_t arg4,  /* R08 */
					word_t arg5,  /* R09 */
					word_t arg6,  /* stack (RAX) */
					word_t arg7   /* stack (RBX) */)
{
    x86_x64_sysret_t ret;
    addr_t syscall = (addr_t) ((uip & (SYSCALL_ALIGN * 0xf)) | (addr_word_t) x32_get_kip ());

    if (syscall == (addr_t) user_ipc_32)
    {
	timeout_t timeout;
	x32_threadid_t tid;
	timeout.raw = arg7;
	ret = sys_ipc (timeout,
		       threadid_64 (x32_threadid_from_raw ((x32_word_t) arg6)),
		       threadid_64 (x32_threadid_from_raw ((x32_word_t) arg3)));
	tid = threadid_32 (threadid_from_raw (ret.rax));
	ret.rax = x32_threadid_get_raw (&tid);
	return ret;
    }
    else if (syscall == (addr_t) user_exchange_registers_32)
    {
	utcb_t *utcb = tcb_get_utcb (get_current_tcb ());
	threadid_t dest = threadid_64 (x32_threadid_from_raw ((x32_word_t) arg6));
	sys_exchange_registers (dest, utcb->exreg32.control, arg3, arg2, arg1, arg7,
				threadid_64 (utcb->exreg32.pager), utcb->exreg32.is_local);
    }
    else if (syscall == (addr_t) user_thread_control_32)
    {
	if (((s32_t) arg1) == -1)
	    arg1 = (word_t) -1;
	return sys_thread_control (threadid_64 (x32_threadid_from_raw ((x32_word_t) arg6)),
				   threadid_64 (x32_threadid_from_raw ((x32_word_t) arg2)),
				   threadid_64 (x32_threadid_from_raw ((x32_word_t) arg3)),
				   threadid_64 (x32_threadid_from_raw ((x32_word_t) arg7)), arg1);
    }
    else if (syscall == (addr_t) user_space_control_32)
    {
	fpage_t kip_area;
	fpage_t utcb_area;
	kip_area.raw = arg3;
	utcb_area.raw = arg2;
	return sys_space_control (threadid_64 (x32_threadid_from_raw ((x32_word_t) arg6)),
				  arg7 | (1UL << 63), kip_area, utcb_area,
				  threadid_64 (x32_threadid_from_raw ((x32_word_t) arg1)));
    }
    else if (syscall == (addr_t) user_schedule_32)
    {
	return sys_schedule (threadid_64 (x32_threadid_from_raw ((x32_word_t) arg6)),
			     (s32_t) arg3, (s32_t) arg2, (s32_t) arg7, (s32_t) arg1);
    }
    else if (syscall == (addr_t) user_thread_switch_32)
    {
	sys_thread_switch (threadid_64 (x32_threadid_from_raw ((x32_word_t) arg6)));
    }
    else if (syscall == (addr_t) user_unmap_32)
    {
	threadid_t local = tcb_get_local_id (get_current_tcb ());
	sys_unmap (arg6);
	ret.rax = threadid_get_raw (&local);
	ret.rdx = 0;
	return ret;
    }
    else if (syscall == (addr_t) user_processor_control_32)
    {
	sys_processor_control (arg6, arg7, arg3, arg2);
    }
    else if (syscall == (addr_t) user_system_clock_32)
    {
	procdesc_t * pdesc = processor_info_get_procdesc (&get_kip ()->processor_info, 0);
	ASSERT (pdesc);
	ret.rax = x86_rdtsc () / (pdesc->internal_freq / 1000);
	ret.rdx = ret.rax >> 32;
	return ret;
    }
    else
	printf ("unknown syscall\n");

    ret.rax = 0;
    ret.rdx = 0;
    return ret;
}
