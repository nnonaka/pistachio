/*********************************************************************
 *                
 * Copyright (C) 2006-2007,  Karlsruhe University
 *                
 * File path:     glue/v4-ia32/memcontrol.cc
 * Description:   MemoryControl syscall (temporary)
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
 * $Id: memcontrol.cc,v 1.1 2006/11/16 20:05:17 skoglund Exp $
 *                
 ********************************************************************/

#include INC_API(tcb.h)
#include INC_API(space.h)
#include INC_GLUE(syscalls.h)
#include <kdb/tracepoints.h>

DECLARE_TRACEPOINT (SYSCALL_MEMORY_CONTROL);


/* space_t::mapctrl, in C -- defined in glue/v4-x86/space.c. */
fpage_t space_mapctrl (space_t *self, fpage_t fpage, mdb_ctrl_t ctrl, word_t attribute, bool unmap_all);

SYS_MEMORY_CONTROL (word_t control, word_t attrib0, word_t attrib1,
		    word_t attrib2, word_t attrib3)
{
    tcb_t * current = get_current_tcb ();
    space_t * space = tcb_get_space (current);
    mdb_ctrl_t ctrl;
    word_t idx;

    TRACEPOINT (SYSCALL_MEMORY_CONTROL, 
		"SYS_MEMORY_CONTROL: control=%lx, attribs=[%lx %lx %lx %lx]\n",
		control, attrib0, attrib1, attrib2, attrib3);

    /* Check parameters */

    if (! is_privileged_space (space))
    {
	tcb_set_error_code (current, ENO_PRIVILEGE);
	return_memory_control (0);
    }

    if (control >= IPC_NUM_MR)
    {
	tcb_set_error_code (current, EINVALID_PARAM);
	return_memory_control (0);
    }

    /* MemCtrl is a special call to MapCtrl */

    ctrl.raw = 0;
    ctrl.mapctrl_self = 1;
    ctrl.set_attribute = 1;

    /* Perform MapCtrl on each provided fpage */

    for (idx = 0; idx <= control; idx++)
    {
	fpage_t fpage;
	word_t attr;

	fpage.raw = tcb_get_mr (current, idx);
	if (fpage_is_nil_fpage (&fpage))
	    continue;

	switch (fpage.raw & 0x3)
	{
	    case 0:  attr = attrib0; break;
	    case 1:  attr = attrib1; break;
	    case 2:  attr = attrib2; break;
	    default: attr = attrib3; break;
	}

	/* Transform into PAT value */
	if (attr > 0)
	    attr--;

	/* Check for valid PAT value */
	if (attr > 7 || attr == 5 || attr == 6)
	{
	    tcb_set_error_code (current, EINVALID_PARAM);
	    return_memory_control (0);
	}

	space_mapctrl (space, fpage, ctrl, attr, false);
    }

    return_memory_control (1);
}
