/*********************************************************************
 *
 * Copyright (C) 2002-2004,  Karlsruhe University
 *
 * File path:     kdb/api/v4/tcb.c
 * Description:   tcb dumping
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
 * $Id: tcb.cc,v 1.45 2005/06/03 15:54:04 joshua Exp $
 *
 ********************************************************************/
#include <debug.h>
#include <kdb/kdb.h>
#include <kdb/cmd.h>
#include <kdb/input.h>
#include INC_API(tcb.h)
#include INC_API(schedule.h)
#include INC_API(cpu.h)

#if defined(CONFIG_IS_64BIT)
#define __PADSTRING__ "        "
#else
#define __PADSTRING__ ""
#endif

u16_t dbg_get_current_cpu(void)
{
    return get_current_cpu();
}

word_t dbg_get_current_tcb(void)
{
    return (word_t) get_current_tcb();
}

#if defined(CONFIG_TBUF_PERFMON_ENERGY)
DECLARE_TRACEPOINT(ENERGY_TIMER);
#endif

bool kdebug_check_interrupt(void)
{

#if defined(CONFIG_TBUF_PERFMON_ENERGY)
    scheduler_t *scheduler = get_current_scheduler();

    static u64_t UNIT("cpulocal") last_second_tick = 0;
    
	if (scheduler->get_current_time() > (last_second_tick + 50000000))
	{
	    TRACEPOINT(ENERGY_TIMER, "Energy TIMER @ %d",
		       (word_t) (scheduler->get_current_time() / 1000));
	    
	    last_second_tick = scheduler->get_current_time();
	    
	    if (get_current_cpu() == 0)
		for (cpuid_t cpu = 0; cpu < cpu_count; cpu++)
		{
		    // Energy timer in the last 1.1 seconds
		    tbuf_dump(2, 0, __tracepoint_ENERGY_TIMER.id, (1 << cpu));
		    // Lx syscalls in the last 10 milliseconds
		    //tbuf_dump(10, , 110, (1 << cpu));
		}
	    

	}
#endif

#if defined(CONFIG_KDB_INPUT_HLT)
    if (get_current_tcb() == get_kdebug_tcb())
	return true;
#endif

    kdebug_check_breakin();
    return false;

}


DECLARE_CMD(cmd_show_tcb, root, 't', "showtcb",  "show thread control block");
DECLARE_CMD(cmd_show_tcbext, root, 'T', "showtcbext", "shows thread control block (extended)");

static inline msg_tag_t SECTION(SEC_KDEBUG) get_msgtag(tcb_t* tcb)
{
    msg_tag_t tag;
    tag.raw = tcb_get_mr (tcb, 0);
    return tag;
}

void SECTION(SEC_KDEBUG) dump_tcb(tcb_t * tcb, bool extended)
{
    sched_ktcb_t *sched_state = &tcb->sched_state;
    
    printf("=== TCB: %p === ID: %p = %p/%p",
	   tcb, tcb_get_global_id (tcb).raw,
	   tcb_get_local_id (tcb).raw, tcb_get_utcb (tcb));
    sched_ktcb_dump_priority (sched_state);
#if !defined(CONFIG_SMP)
    printf("=====");
#else
    printf(" CPU: %d ===", tcb_get_cpu (tcb));
#endif
    printf(" ===\n");

    printf("UIP: %p   queues: %c%c%c%c%s      ",
	   tcb_get_user_ip (tcb),
	   queue_state_is_set (&tcb->queue_state, QUEUE_STATE_READY)	? 'R' : 'r',
	   queue_state_is_set (&tcb->queue_state, QUEUE_STATE_SEND)	? 'S' : 's',
	   queue_state_is_set (&tcb->queue_state, QUEUE_STATE_WAKEUP)	? 'W' : 'w',
	   queue_state_is_set (&tcb->queue_state, QUEUE_STATE_LATE_WAKEUP)	? 'L' : 'l',
	   __PADSTRING__);
    sched_ktcb_dump_list1 (sched_state);
    printf("space: %p\n", tcb_get_space (tcb));
    printf("USP: %p   tstate: %ws  ", tcb_get_user_sp (tcb), thread_state_string (tcb_get_state (tcb)));
    sched_ktcb_dump_list2 (sched_state);
    printf("pdir : %p\n", tcb->pdir_cache);
    printf("KSP: %p   sndhd : %-wt  send : %wt:%-wt   pager: %t\n",
	   tcb->stack, tcb->send_head, tcb->send_list.next, tcb->send_list.prev,
	   TID(tcb_get_utcb (tcb) ? tcb_get_pager (tcb) : threadid_nilthread ()));
    sched_ktcb_dump (sched_state, sched_get_current_time ());
    printf("resources: %p [", tcb->resource_bits.resource_bits.maskvalue);
    tcb_resources_dump (&tcb->resources, tcb);
    printf("]");
    printf("   flags: %p [", tcb->flags.maskvalue);
    printf("%c", tcb_flags_is_set (tcb, TCB_FLAG_HAS_XFER_TIMEOUT)	? 'T' : 't');
    printf("%c", tcb_flags_is_set (tcb, TCB_FLAG_SCHEDULE_IN_PROGRESS) ? 'S' : 's');
#if defined(CONFIG_X_CTRLXFER_MSG)
    printf("%c", tcb_flags_is_set (tcb, TCB_FLAG_KERNEL_CTRLXFER_MSG) ? 'K' : 'k');
#endif
    printf("]\n");
#if defined(CONFIG_X_CTRLXFER_MSG)
    tcb_dump_ctrlxfer_state (tcb, extended);
#endif
    printf("partner: %t, saved partner: %t, saved state: %s, scheduler: %t\n",
	   TID(tcb_get_partner (tcb)), TID(tcb_get_saved_partner (tcb)),
	   thread_state_string (tcb_get_saved_state (tcb)),
	   TID(sched_ktcb_get_scheduler (&tcb->sched_state)));
}


void SECTION (SEC_KDEBUG) dump_utcb (tcb_t * tcb)
{
    preempt_flags_t pflags = tcb_get_preempt_flags (tcb);

    printf ("\nuser handle:       %p  "
	    "cop flags:      %02x%s  "
	    "preempt flags:     %02x [%c%c%c]\n"
	    "exception handler: %t  "
	    "virtual sender: %t  "
	    "intended receiver: %t\n",
	    tcb_get_user_handle (tcb), tcb_get_cop_flags (tcb),
	    sizeof (word_t) == 8 ? "              " : "      ",
	    tcb_get_preempt_flags (tcb).raw,
	    preempt_flags_is_pending (&pflags)  ? 'I' : '~',
	    preempt_flags_is_delayed (&pflags)  ? 'd' : '~',
	    preempt_flags_is_signaled (&pflags) ? 's' : '~',
	    TID (tcb_get_exception_handler (tcb)),
	    TID (tcb_get_virtual_sender (tcb)),
	    TID (tcb_get_intended_receiver (tcb)));

    printf ("xfer timeouts:     snd (");
    time_t xfer = tcb_get_xfer_timeout_snd (tcb);
    printf (time_is_never (&xfer) ? "never" : "%s: %12dus",
	    time_is_period (&xfer) ? "rel" : "abs",
	    time_get_microseconds (&xfer));
    printf (")\n                   rcv (");
    xfer = tcb_get_xfer_timeout_rcv (tcb);
    printf (time_is_never (&xfer) ? "never" : "%s: %12dus",
	    time_is_period (&xfer) ? "rel" : "abs",
	    time_get_microseconds (&xfer));
    printf (")\n");
}


/**
 * Dumps a message and buffer registers of a thread in human readable form
 * @param tcb	pointer to thread control block
 */
/* ctrlxfer_item_t in api/v4/ipc.h is C now, so the names below are the real
   ones: ctrlxfer_get_idname and ctrlxfer_fault_item.  ctrlxfer_mask_t is a
   bitmask_u16_t struct, so the bit arithmetic goes through .maskvalue. */
static void SECTION(SEC_KDEBUG) dump_message_registers(tcb_t * tcb)
{
    msg_tag_t tag = get_msgtag (tcb);

    for (int i = 0; i < IPC_NUM_MR; i++)
    {
	if (!(i % 8)) printf("\nmr(%02d):", i);
	printf(" %p", tcb_get_mr (tcb, i));
    }

    printf("\nMessage Tag: %d untyped, %d typed, label = %x, flags = %c%c%c%c\n",
           msg_tag_get_untyped (&tag), msg_tag_get_typed (&tag),
           tag.x.label,
           msg_tag_is_error (&tag) ? 'E' : '-',
           msg_tag_is_xcpu (&tag) ? 'X' : '-',
           msg_tag_is_redirected (&tag) ? 'r' : '-',
           msg_tag_is_propagated (&tag) ? 'p' : '-'
        );

    for (word_t i = 0; i < msg_tag_get_typed (&tag);)
    {
	word_t offset = msg_tag_get_untyped (&tag) + 1;
	msg_item_t item;

	item.raw = tcb_get_mr (tcb, offset + i);
	if (msg_item_is_map_item (&item) || msg_item_is_grant_item (&item))
	{
	    fpage_t fpage;
	    fpage.raw = tcb_get_mr (tcb, offset + i + 1);
	    printf("%s item: snd base=%p, fpage=%p (addr=%p, sz=%x), %c%c%c\n",
                   msg_item_is_map_item (&item) ? "map" : "grant",
                   msg_item_get_snd_base (&item),
                   fpage.raw, fpage_get_base (&fpage), fpage_get_size (&fpage),
                   fpage.mem.x.write	? 'W' : 'w',
                   fpage.mem.x.read	? 'R' : 'r',
                   fpage.mem.x.execute	? 'X' : 'x');
	    i+=2;
	}
	else if (msg_item_is_string_item (&item))
	{
	    printf("string item: len=%x, num=%d, cont=%d, cache=%d\n  ( ",
                   msg_item_get_string_length (&item), msg_item_get_string_ptr_count (&item),
                   msg_item_is_string_compound (&item), msg_item_get_string_cache_hints (&item));
	    i++;

	    for (word_t j = 0; j < msg_item_get_string_ptr_count (&item); j++, i++)
                printf("%p ", tcb_get_mr (tcb, offset + i));
	    printf(")\n");
	}
#if defined(CONFIG_X_CTRLXFER_MSG)
	else if (msg_item_is_ctrlxfer_item (&item))
	{
            
            if (tcb_flags_is_set (tcb, TCB_FLAG_KERNEL_CTRLXFER_MSG))
            {
                ctrlxfer_mask_t mask = tcb_get_fault_ctrlxfer_items (tcb, msg_item_get_ctrlxfer_id (&item));
                word_t id = msg_item_get_ctrlxfer_id (&item);

                printf( "ctrlxfer kernel msg fault %d mask %x\n", msg_item_get_ctrlxfer_id (&item), (word_t) mask.maskvalue);

                id = lsb(mask.maskvalue);

                do {
                    printf("\t id %d %s mask %x %x\n ", id, ctrlxfer_get_idname (id),
                           ctrlxfer_fault_item (id).mask, (word_t) mask.maskvalue);
                    mask.maskvalue &= ~(1UL << id);	/* was mask -= id */
                    id = lsb(mask.maskvalue);
                } while (mask.maskvalue);
                
                i+=1;
                
            }
            else
            {
                word_t mask = msg_item_get_ctrlxfer_mask (&item);
                word_t id = msg_item_get_ctrlxfer_id (&item);
                word_t num = 1, reg = 0;
                
                printf("ctrlxfer item: mask=%x, id=%d",  mask, id);
                
                while (mask && num < IPC_NUM_MR)
                {
                    if ((num-1) % 4 == 0) printf("\n\t");
                    while ((mask & 1) == 0) { mask >>= 1; reg++; } 
                    printf("%s: %p ", ctrlxfer_get_hwregname (id, reg), tcb_get_mr (tcb, offset + i + num));
                    mask >>= 1; reg++; num++;
                }
                i += num;
            }
            printf("\n");
        }
#endif
	else
	{
	    printf("unknown item type (%p)\n", item.raw);
	    i++;
	}
    }
}

static void SECTION(SEC_KDEBUG) dump_buffer_registers(tcb_t * tcb)
{
    acceptor_t acc;
    fpage_t fpage;
    msg_item_t item;

    acc.raw = tcb_get_br (tcb, 0);
    fpage.raw = tcb_get_br (tcb, 0);
    fpage.raw &= ~0xf; // mask out lowermost bits.

    for (word_t i = 0; i < IPC_NUM_BR; i++)
    {
	if (!(i % 8)) printf("\nbr(%02d):", i);
	printf(" %p", tcb_get_br (tcb, i));
    }

    printf("\nAcceptor: %p (%c)\n", acc.raw, acceptor_accept_strings (&acc) ? 'S' : 's');
    printf("  fpage :");
    if (fpage_is_nil_fpage (&fpage))
	printf(" (NIL-FPAGE)\n");
    else if (fpage_is_complete_fpage (&fpage))
	printf(" (COMPLETE-FPAGE)\n");
    else
	printf("  fpage=%p (addr=%p, sz=%p)\n",
	    fpage.raw, fpage_get_base (&fpage), fpage_get_size (&fpage));

    if (acceptor_accept_strings (&acc))
    {
	word_t idx = 1;
	do
	{
	    item.raw = tcb_get_br (tcb, idx);
	    printf("string item: len=%x, num=%d, compound=%d, "
		   "cache=%d, more_strings=%d\n  ( ",
		   msg_item_get_string_length (&item), msg_item_get_string_ptr_count (&item),
		   msg_item_is_string_compound (&item), msg_item_get_string_cache_hints (&item),
		   msg_item_more_strings (&item));
	    idx++;

	    for (word_t j = 0; j < msg_item_get_string_ptr_count (&item); j++, idx++)
		    printf("%p ", tcb_get_br (tcb, idx));
	    printf(")\n");
	} while(msg_item_more_strings (&item) || msg_item_is_string_compound (&item));
    }
}

tcb_t SECTION(SEC_KDEBUG) * kdb_get_tcb()
{
    debug_param_t * param = (debug_param_t*)kdb.kdb_param;
    space_t *space = param->space;
    word_t val = get_hex ("tcb/tid", (word_t) space, "current");

    if (val == ABORT_MAGIC)
	return NULL;

    if (!tcb_is_tcb ((addr_t)val) &&
	(val != (word_t)get_idle_tcb_c()))
    {
	threadid_t tid;
	threadid_set_raw (&tid, val);
	val = (word_t)tcb_get_tcb (tid);
    }
    return (tcb_t*) addr_to_tcb ((addr_t) val);

}

CMD(cmd_show_tcb, cg)
{
    tcb_t * tcb = get_thread ("tcb/tid/name");
    if (tcb)
	dump_tcb(tcb, false);
    return CMD_NOQUIT;
}



CMD(cmd_show_tcbext, cg)
{
    tcb_t * tcb = get_thread ("tcb/tid/name");
    if (tcb)
    {
	dump_tcb(tcb, true);
	if (tcb_get_utcb (tcb))
	{
	    dump_utcb(tcb);
	    dump_message_registers(tcb);
	    dump_buffer_registers(tcb);
	}
	else
	    printf("no valid UTCB\n");
    }
    return CMD_NOQUIT;
}
