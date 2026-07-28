/*********************************************************************
 *
 * Copyright (C) 2002,  Karlsruhe University
 *
 * File path:    api/v4/schedule.cc 
 * Description:  debugging of scheduling related stuff
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
 * $Id: schedule.cc,v 1.7 2004/12/09 01:27:24 cvansch Exp $
 *
 *********************************************************************/
#include <debug.h>
#include <kdb/kdb.h>
#include <kdb/cmd.h>
#include <sync.h>
#include INC_API(tcb.h)
#include INC_API(smp.h)
#include INC_API(schedule.h)
#include INC_API(cpu.h)

/* get_current_scheduler() was a C++ inline; the cpulocal instance is the global */
extern scheduler_t scheduler;

tcb_t * global_present_list UNIT("kdebug") = NULL;
spinlock_t present_list_lock;

DECLARE_CMD(cmd_show_sched, root, 'q', "showqueue",  "show scheduling queue");

static void depth_indent (int depth)
{
    int i;
    for (i = 0; i < depth; i++)
	printf ("       ");
}

static bool is_subdomain (tcb_t *tcb)
{
    return ((tcb->sched_state.base.flags.maskvalue &
	     (1UL << HS_SCHED_FLAG_IS_SCHEDULE_DOMAIN)) != 0 &&
	    hs_sched_get_prio_queue (&tcb->sched_state.base) != NULL);
}

static void show_prio_queue (int depth, scheduler_t *sched, prio_queue_t *prio_queue,
			     cpuid_t cpu)
{
    tcb_t *domain_tcb;
    s16_t prio;

    (void) sched;
    depth_indent (depth);

    domain_tcb = prio_queue_get_domain_tcb (prio_queue);

    printf ("priority queue %p pass %llu domain tcb %p  depth %d\n",
	    prio_queue, prio_queue_get_global_pass (prio_queue), domain_tcb,
	    prio_queue_get_depth (prio_queue));

    for (prio = MAX_PRIORITY; prio >= 0; prio--)
    {
	/* check whether we have something for this prio */
	tcb_t *walk = global_present_list;
	do {
	    if (hs_sched_get_priority (&walk->sched_state.base) == prio &&
		hs_sched_get_prio_queue (&walk->sched_state.base) == prio_queue &&
		tcb_get_cpu (walk) == cpu)
	    {
		bool subdomain = false;

		/* if so, print */
		depth_indent (depth);
		printf ("[%02x]:\n", hs_sched_get_priority (&walk->sched_state.base));

		do {
		    if (hs_sched_get_priority (&walk->sched_state.base) == prio &&
			hs_sched_get_prio_queue (&walk->sched_state.base) == prio_queue &&
			tcb_get_cpu (walk) == cpu)
		    {
			bool ready = queue_state_is_set (&walk->queue_state, QUEUE_STATE_READY);

			if (is_subdomain (walk))
			    subdomain = true;

			depth_indent (depth + 1);
			printf ("  [%16U][%8u]", hs_sched_get_pass (&walk->sched_state.base),
				hs_sched_get_stride (&walk->sched_state.base));
			printf (ready ? " %t\n" : " (%t)\n", walk);
		    }
		    walk = walk->present_list.next;

		} while (walk != global_present_list);
		printf ("\n");

		if (subdomain)
		{
		    /* print the subdomains */
		    walk = global_present_list;
		    do {
			if (hs_sched_get_priority (&walk->sched_state.base) == prio &&
			    hs_sched_get_prio_queue (&walk->sched_state.base) == prio_queue &&
			    tcb_get_cpu (walk) == cpu && is_subdomain (walk))
			{
			    /* we found a subdomain */
			    show_prio_queue (depth + 1, sched,
					     hs_sched_get_domain_prio_queue (&walk->sched_state.base),
					     cpu);
			}
			walk = walk->present_list.next;
		    } while (walk != global_present_list);
		}
	    }
	    else
		walk = walk->present_list.next;

	} while (walk != global_present_list);
    }
}

CMD(cmd_show_sched, cg)
{
    cpuid_t cpu;

    (void) cg;
    spinlock_lock (&present_list_lock);
    printf ("\n");

    for (cpu = 0; cpu < cpu_count; cpu++)
    {
	/* NB: name the local differently from the global `scheduler' -- a local
	   is in scope inside its own initializer, so `&scheduler' would take
	   the address of this (uninitialised) pointer instead of the global. */
	scheduler_t *sched = (scheduler_t *) get_on_cpu_c (cpu, &scheduler);

	printf ("\n\nCPU %d:  scheduled tcb %t, scheduled queue %p max_prio %d\n",
		cpu, sched->__base.scheduled_tcb, sched->__base.scheduled_queue,
		sched->__base.root_prio_queue.max_prio);
	printf ("\n");

	show_prio_queue (0, sched, &sched->__base.root_prio_queue, cpu);
    }
    printf ("idle : %p\n\n", get_idle_tcb_c ());

    spinlock_unlock (&present_list_lock);
    return CMD_NOQUIT;
}
