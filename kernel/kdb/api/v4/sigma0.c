/*********************************************************************
 *                
 * Copyright (C) 2002, 2006-2007, 2009-2010,  Karlsruhe University
 *                
 * File path:     kdb/api/v4/sigma0.c
 * Description:   Sigma0 interaction
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
 * $Id: sigma0.cc,v 1.3 2006/12/05 15:23:15 skoglund Exp $
 *                
 ********************************************************************/
#include <debug.h>
#include <kdb/cmd.h>
#include <kdb/kdb.h>
#include <kdb/input.h>

#include INC_API(kernelinterface.h)
#include INC_API(thread.h)
#include INC_API(schedule.h)
#include INC_API(space.h)
#include INC_API(ipc.h)
#include INC_API(tcb.h)


/*
 * Sigma0 extended protocol definitions.
 */
#define SIGMA0_EXTPROT_ID	(-1001)

enum sigma0_request_e {
    s0_verbose =	1,
    s0_dumpmem =	2,
};
typedef enum sigma0_request_e sigma0_request_e;


static void sigma0_send (sigma0_request_e type, word_t arg);


DECLARE_CMD_GROUP (s0_interact);


/**
 * Sigma0 interaction.
 */
DECLARE_CMD (cmd_sigma0, root, '0', "sigma0", "sigma0 interaction");

CMD (cmd_sigma0, cg)
{
    return cmd_group_interact (&s0_interact, cg, "sigma0");
}


/**
 * Change sigma0 verboseness.
 */
DECLARE_CMD (cmd_s0_verbose, s0_interact, 'v', "verbose",
	     "change sigma0 verboseness");

CMD (cmd_s0_verbose, cg)
{
    sigma0_send (s0_verbose, get_dec ("Verbose level", 1, NULL));
    return CMD_NOQUIT;
}


/**
 * Dump sigma0 memory pools.
 */
DECLARE_CMD (cmd_s0_dumpmem, s0_interact, 'm', "dumpmem",
	     "dump sigma0 memory pools");

CMD (cmd_s0_dumpmem, cg)
{
    sigma0_send (s0_dumpmem, 0);
    return CMD_NOQUIT;
}


static void sigma0_ipc (word_t type, word_t arg)
{
    tcb_t * current = get_current_tcb ();

    // Create message.
    msg_tag_t tag;
    msg_tag_set (&tag, 0, 2, (word_t) SIGMA0_EXTPROT_ID << 4);
    tcb_set_mr (current, 0, tag.raw);
    tcb_set_mr (current, 1, type);
    tcb_set_mr (current, 2, arg);

    // Send to sigma0.
    threadid_t s0id;
    threadid_set_global_id (&s0id, thread_info_get_user_base (&get_kip ()->thread_info), 1);
    tag = tcb_do_ipc (current, s0id, NILTHREAD, timeout_never ());

    // Abort kernel thread execution.
    tcb_set_space (current, NULL);
    tcb_set_state (current, THREAD_STATE_ABORTED);
    sched_deschedule (current);
    sched_ktcb_init (&current->sched_state, sktcb_lo);
    sched_schedule (get_idle_tcb_c (), sched_handoff);
}


/**
 * Send a two word IPC message from a kernel thread to sigma0 using an
 * extended sigma0 protocol.
 *
 * @param type		type of message to send
 * @param arg		argument of message
 */
static void sigma0_send (sigma0_request_e type, word_t arg)
{
    threadid_t ktid;
    threadid_set_global_id (&ktid, thread_info_get_system_base (&get_kip ()->thread_info), 1);

    // Make kernel thread invoke IPC sending stub.
    tcb_t * tcb = tcb_get_tcb (ktid);
    tcb_init_stack (tcb);
    tcb_notify_word2 (tcb, sigma0_ipc, (word_t) type, arg);

    // Make kernel thread run on highest prio.
    sched_ktcb_init (&tcb->sched_state, sktcb_hi);
    tcb_set_space (tcb, get_kernel_space_c ());
    tcb_set_state (tcb, THREAD_STATE_RUNNING);
    sched_schedule (tcb, sched_current);
}
