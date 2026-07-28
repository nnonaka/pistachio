/*********************************************************************
 *                
 * Copyright (C) 2004, 2007-2010, 2012,  Karlsruhe University
 *                
 * File path:     kdb/api/v4/thread.c
 * Description:   Kdebug stuff for V4 threads
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
 * $Id: thread.cc,v 1.5 2005/06/03 15:54:04 joshua Exp $
 *                
 ********************************************************************/
#include <kdb/tid_format.h>
#include INC_API(thread.h)
#include INC_API(tcb.h)


/* A local copy of addr_to_tcb, which was declared only in tcb.h's C++ branch
   and went with the guard collapse.  Keep it in step with KTCB_MASK. */
INLINE tcb_t * to_tcb (addr_t addr) { return (tcb_t *) ((word_t) addr & KTCB_MASK); }

/* From generic/print.c (C now -- no linkage decoration needed here) */

int print_hex (const word_t val,
	       int width,
	       int precision,
	       bool adjleft,
	       bool nullpad,
               bool uppercase);
int print_string (const char * s, const int width, const int precision);
int print_hex_sep (const word_t val,
		   const int bits,
		   const char *sep);
int print_dec (const word_t val, const int width, const char pad);
    


int print_tid (word_t val, word_t width, word_t precision, bool adjleft)
{
    tcb_t * tcb;
    threadid_t tid;

#if 0
    print_string ("<", 0, 0);
    print_dec (width);
    print_string (":", 0, 0);
    print_dec (precision);
    print_string (">", 0, 0);
#endif

    // If val is within TCB area, treat it as a tcb address.  If not,
    // treat it as a thread ID.

    if (space_is_tcb_area ((addr_t) val) || 
	to_tcb ((addr_t) val) == get_idle_tcb_c() || 
	to_tcb ((addr_t) val) == get_kdebug_tcb())
    {
        tcb = to_tcb ((addr_t) val);
	tid = tcb_get_global_id (tcb);
    }
    else
    {
	threadid_set_raw (&tid, val);
	tcb = tcb_get_tcb (tid);
    }

    if (kdb_tid_format.X.human)
    {
	// Convert special thread IDs to human readable form
	threadid_t ktid;
	threadid_set_global_id (&ktid, thread_info_get_system_base (&get_kip ()->thread_info), 1);

	if (threadid_equals (&tid, &ktid))
	    return print_string ("KRN_THRD", (int) width, (int) precision);

	{ threadid_t idl = IDLETHREAD; if (threadid_equals (&tid, &idl))
	    return print_string ("IDLETHRD", (int) width, (int) precision); }

	if (threadid_is_nilthread (&tid))
	    return print_string ("NIL_THRD", (int) width, (int) precision);

	if (threadid_is_anythread (&tid))
	    return print_string ("ANY_THRD", (int) width, (int) precision);

	if (threadid_is_interrupt (&tid))
	{
	    print_string ("IRQ_", 0, 0);
	    return 4 + print_dec (threadid_get_irqno (&tid), (int) (width - 4), '0');
	}
	word_t base_id = threadid_get_threadno (&tid) -
	    thread_info_get_user_base (&get_kip()->thread_info);
	if (base_id < 3)
	{
	    const char *names[3] = { "SIGMA0", "SIGMA1", "ROOTTASK" };
	    return print_string (names[base_id], (int) width, (int) precision);
	}

        if (tcb == get_kdebug_tcb())
	    return print_string ("KDBTHRD", (int) width, (int) precision);

    }

    // We're dealing with something which is not a special thread ID.

    word_t n = 0;
    bool f_both = TID_FORMAT_VALUE_BOTH == kdb_tid_format.X.value;
    bool f_gid = TID_FORMAT_VALUE_GID == kdb_tid_format.X.value;
    bool f_tcb = TID_FORMAT_VALUE_TCB == kdb_tid_format.X.value;
    bool f_ver = TID_FORMAT_VERSION_OFF != kdb_tid_format.X.version;

    if (f_gid || f_both)
    {
	// Getting a consistent output width with separators is pretty
	// much hopeless depending on the position of the separator,
	// additional hex characters become necessary

	if (TID_FORMAT_VERSION_INLINE == kdb_tid_format.X.version)
	{
	    // Do not separate version from threadno
	    if (kdb_tid_format.X.sep != 0)
		// Insert a separator into threadno
		n = print_hex_sep (threadid_get_raw (&tid),
				   kdb_tid_format.X.sep +
				   L4_GLOBAL_VERSION_BITS, ".");
	    else
		// No separator at all
		n = print_hex (threadid_get_raw (&tid), 0, sizeof (word_t) * 2, false, false, false);
	}
	else
	{
	    if (kdb_tid_format.X.sep != 0)
		// Insert a separator into threadno
		n = print_hex_sep (threadid_get_threadno (&tid),
				   kdb_tid_format.X.sep, ".");
	    else
		// Print threadno without separator
		n = print_hex (threadid_get_threadno (&tid),
			       f_both || f_ver ? 0 : (int) width,
			       0, adjleft, 0, 0);

	    if (f_ver)
	    {
		// Add a separator between threadno and version
		n += print_string ("v", 0, 0);
//		print_dec (width); print_string (">", 0, 0);
		width -= width > n ? n : 0;
		n += print_hex (threadid_get_version (&tid),
				f_both ? 0 : (int) width, 0, true, 0, 0);
	    }
	}
    }

    if (f_both)
	n += print_string ("/", 0, 0);

    if (f_tcb || f_both)
	// Print plain TCB address
	n += print_hex ((word_t) tcb, 0, sizeof (word_t) * 2, false, false, false);

    return (int) n;
}
