/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2006, 2008-2011,  Karlsruhe University
 *                
 * File path:     api/v4/smp.cc
 * Description:   Multiprocessor handling for cross-processor 
 *		  operations
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
 * $Id: smp.cc,v 1.10 2006/06/16 10:28:44 stoess Exp $
 *                
 ********************************************************************/
#include <sync.h>
#include <kdb/tracepoints.h>
#include INC_API(smp.h)
#include INC_API(schedule.h)
#include INC_API(queueing.h)

cpuid_t	current_cpu UNIT("cpulocal");;

#if defined(CONFIG_SMP)

#ifdef CONFIG_SMP_IDLE_POLL
void processor_sleep()
{
    x86_sleep();
    process_xcpu_mailbox();
}
#endif



//#define TRACE_IPI(x...) do { printf("CPU %d: ", get_current_cpu()); printf(x); } while(0)

#if defined(CONFIG_SMP_SYNC_REQUEST)

/*
 * VU: Synchronous XCPU handling
 * 
 * Each CPU has a dedicated synchronous sender-based mailbox.  When
 * sending a request to another CPU, the parameters are filled in and
 * a pending bit is flipped in the destination CPU's mailbox.
 * Afterwards, the CPU polls for an ack on the local mailbox but still
 * handles incoming synchronous requests.  This scheme allows to send
 * remote requests even with disabled interrupts. The handler MUST NOT
 * preempt the current thread.  Synchronous XCPU handling has to be
 * enabled explicitly by defining CONFIG_SMP_SYNC_REQUEST in
 * INC_GLUE(config.h) */

static sync_entry_t sync_xcpu_entry[CONFIG_SMP_MAX_CPUS];

void sync_entry_handle_sync_requests(sync_entry_t *self)
{
    while (self->pending_mask)
    {
	for (cpuid_t cpu = 0; cpu < CONFIG_SMP_MAX_CPUS; cpu++)
	    if (self->pending_mask & (1 << cpu))
	    {
		sync_xcpu_entry[cpu].base.handler(&sync_xcpu_entry[cpu].base);
		sync_entry_clear_pending(self, cpu);
		sync_entry_ack(&sync_xcpu_entry[cpu], get_current_cpu());
	    }
    }
}

void sync_xcpu_request(cpuid_t dstcpu, xcpu_handler_t handler, tcb_t * tcb, 
		       word_t param0, word_t param1, word_t param2)
{
#if defined(CONFIG_DEBUG)
    if (get_current_tcb() == get_kdebug_tcb())
	// Avoid KDB deadlock
	return;
#endif

    sync_entry_t * entry = &sync_xcpu_entry[get_current_cpu()];

    entry->ack_mask = 0;
    cpu_mb_entry_set (&entry->base, handler, tcb, param0, param1, param2);

    // now signal the other CPU
    sync_entry_set_pending(&sync_xcpu_entry[dstcpu], get_current_cpu());

    // trigger other side to make sure it gets processed ASAP
    smp_xcpu_trigger(dstcpu);

    // now we poll till the CPU has finished the request
    while (entry->ack_mask == 0)
    {
	spin(70, 0);
	sync_entry_handle_sync_requests(&sync_xcpu_entry[get_current_cpu()]);
    }
}
#endif /* CONFIG_SMP_SYNC_REQUEST */


/*
 * VU: Asynchronous handling
 *
 * Each CPU has a mailbox with multiple entries. When performing an
 * asynchronous XCPU request, an entry is allocated triggering a
 * remote handler with the corresponding parameters.  To free entries
 * quickly they are copied onto the current stack. Async requests are
 * receiver based and therefore require locking.  To avoid contention
 * a sender-receiver matrix could be used with n*(n-1) mailboxes. */

cpu_mb_t cpu_mailboxes[CONFIG_SMP_MAX_CPUS];

void cpu_mb_walk_mailbox(cpu_mb_t *self)
{
    while(self->first_free != self->first_alloc)
    {
	spinlock_lock(&self->lock);
	cpu_mb_entry_t entry = self->entries[self->first_alloc];
	self->entries[self->first_alloc].handler = NULL;
	self->first_alloc = (self->first_alloc + 1) % MAX_MAILBOX_ENTRIES;
	spinlock_unlock(&self->lock);
	ASSERT(entry.handler);

	//printf("CPU%d: XCPU-entry (handler: %t)\n", get_current_cpu(), entry.handler);
	entry.handler(&entry);
    }
}


void cpu_mb_dump_mailbox(cpu_mb_t *self, word_t cpu)
{
    printf("CPU%d Mailbox first alloc %d first free %d\n", cpu, self->first_alloc, self->first_free);
    for (word_t e=0; e < MAX_MAILBOX_ENTRIES; e++)
    {
        cpu_mb_entry_t entry = self->entries[e];
	if (entry.handler != NULL)
            printf("\tXCPU-entry %d\n\t\thandler:%t,"
		   "tcb %t\n\t\tparams %x:%x:%x:%x:%x:%x:%x:%x\n",
                   cpu, e, entry.handler, entry.tcb,
                   entry.param[0], entry.param[1], entry.param[2], entry.param[3],
                   entry.param[4], entry.param[5], entry.param[6], entry.param[7]);
    }
}


void process_xcpu_mailbox()
{
#if defined(CONFIG_SMP_SYNC_REQUEST)
    sync_entry_handle_sync_requests(&sync_xcpu_entry[get_current_cpu()]);
#endif
    cpu_mb_walk_mailbox(get_cpu_mailbox (get_current_cpu()));
}


#endif
