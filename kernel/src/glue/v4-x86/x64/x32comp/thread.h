/*********************************************************************
 *                
 * Copyright (C) 2006, 2008,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/x64/x32comp/thread.h
 * Description:   32-bit twin of threadid_t
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
 * $Id: types.h,v 1.2 2006/10/20 16:18:38 reichelt Exp $
 *                
 ********************************************************************/
#ifndef __GLUE__V4_X86__X64__X32COMP__THREAD_H__
#define __GLUE__V4_X86__X64__X32COMP__THREAD_H__

#include <debug.h>

#include INC_API(thread.h)
#include INC_GLUE_SA(x32comp/types.h)
#include INC_GLUE_SA(x32comp/kernelinterface.h)

#undef TID_GLOBAL_VERSION_BITS
#undef TID_GLOBAL_THREADNO_BITS
#undef TID_LOCAL_ID_ZERO_BITS
#undef TID_LOCAL_ID_BITS

#define TID_GLOBAL_VERSION_BITS		L4_GLOBAL_VERSION_BITS_32
#define TID_GLOBAL_THREADNO_BITS	L4_GLOBAL_THREADNO_BITS_32
#define TID_LOCAL_ID_ZERO_BITS		L4_LOCAL_ID_ZERO_BITS_32
#define TID_LOCAL_ID_BITS		L4_LOCAL_ID_BITS_32

/* threadid_is_interrupt consults the KIP, so the 32-bit KIP has to be in
   scope under the renamed name before this is expanded. */
#include INC_GLUE_SA(x32comp/x32-names.h)

#undef __API__V4__THREAD_H__
#include INC_API(thread.h)

#define X32_UNRENAME
#include INC_GLUE_SA(x32comp/x32-names.h)
#undef X32_UNRENAME

/* thread.h's four special-id macros now name the 32-bit constructors; put
   them back to the 64-bit ones. */
#undef NILTHREAD
#undef ANYTHREAD
#undef ANYLOCALTHREAD
#undef IDLETHREAD
#define NILTHREAD	(threadid_nilthread())
#define ANYTHREAD	(threadid_anythread())
#define ANYLOCALTHREAD	(threadid_anylocalthread())
#define IDLETHREAD	(threadid_idlethread())

INLINE x32_threadid_t threadid_32 (threadid_t id)
{
    if (threadid_is_anythread (&id)) {
	return x32_threadid_anythread ();
    } else if (threadid_is_local (&id)) {
	if (threadid_is_anylocalthread (&id)) {
	    return x32_threadid_anylocalthread ();
	} else {
	    return x32_threadid_from_raw ((x32_word_t) threadid_get_raw (&id));
	}
    } else {
	return x32_threadid_global ((x32_word_t) threadid_get_threadno (&id),
				    (x32_word_t) threadid_get_version (&id));
    }
}

INLINE threadid_t threadid_64 (x32_threadid_t id)
{
    if (x32_threadid_is_anythread (&id)) {
	return threadid_anythread ();
    } else if (x32_threadid_is_local (&id)) {
	if (x32_threadid_is_anylocalthread (&id)) {
	    return threadid_anylocalthread ();
	} else {
	    return threadid_from_raw (x32_threadid_get_raw (&id));
	}
    } else if (x32_threadid_is_interrupt (&id)) {
	return threadid_irqthread (x32_threadid_get_irqno (&id));
    } else {
	return threadid_global (x32_threadid_get_threadno (&id),
				x32_threadid_get_version (&id));
    }
}

/* The same conversion between the raw forms, for the syscall return paths,
   which carry thread ids as plain words. */
INLINE word_t threadid_raw_32 (word_t raw)
{
    x32_threadid_t tid = threadid_32 (threadid_from_raw (raw));
    return x32_threadid_get_raw (&tid);
}

#undef TID_GLOBAL_VERSION_BITS
#undef TID_GLOBAL_THREADNO_BITS
#undef TID_LOCAL_ID_ZERO_BITS
#undef TID_LOCAL_ID_BITS


#endif /* !__GLUE__V4_X86__X64__X32COMP__THREAD_H__ */
