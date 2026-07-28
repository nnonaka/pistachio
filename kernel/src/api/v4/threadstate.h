/*********************************************************************
 *
 * Copyright (C) 2002,  Karlsruhe University
 *
 * File path:    api/v4/threadstate.h 
 * Description:  thread state
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
 * $Id: threadstate.h,v 1.19 2004/06/02 18:03:29 stoess Exp $
 *
 *********************************************************************/
#ifndef __API__V4__THREADSTATE_H__
#define __API__V4__THREADSTATE_H__

/**
 * thread state definitions
 *
 * VU: they should go into the architecture specific part to allow
 *     special optimized encoding
 */
#define RUNNABLE_STATE(id)	((id << 1) | 0)
#define BLOCKED_STATE(id)	((id << 1) | 1)

/* Enumerator values, named as macros so C (and assembly via asmsyms) can
   reference them; the C++ thread_state_e enum below aliases these. */
#define THREAD_STATE_RUNNING			RUNNABLE_STATE(1)
#define THREAD_STATE_WAITING_FOREVER		BLOCKED_STATE(~0UL)
#define THREAD_STATE_WAITING_TIMEOUT		BLOCKED_STATE(2)
#define THREAD_STATE_WAITING_TUNNELED_PF	BLOCKED_STATE(10)
#define THREAD_STATE_LOCKED_WAITING		BLOCKED_STATE(3)
#define THREAD_STATE_LOCKED_RUNNING		RUNNABLE_STATE(4)
#define THREAD_STATE_LOCKED_RUNNING_IPC_DONE	RUNNABLE_STATE(9)
#define THREAD_STATE_LOCKED_RUNNING_NESTED	RUNNABLE_STATE(11)
#define THREAD_STATE_POLLING			BLOCKED_STATE(5)
#define THREAD_STATE_HALTED			BLOCKED_STATE(6)
#define THREAD_STATE_ABORTED			BLOCKED_STATE(7)
#define THREAD_STATE_XCPU_WAITING_DELTCB	BLOCKED_STATE(8)
#define THREAD_STATE_XCPU_WAITING_EXREGS	BLOCKED_STATE(12)

/**
 * thread_state_t: current thread state
 */
struct thread_state_t
{
    /* thread_state_e in C++; stored as its word-wide backing type so the
       member is plain C (the enum itself is C++-only, guarded below). */
    word_t state;
};
typedef struct thread_state_t thread_state_t;

/* Single implementation of the state->name mapping, usable from both
   languages; thread_state_t::string() below forwards to it. */
INLINE const char * thread_state_string (word_t state)
{
    switch (state) {
    case THREAD_STATE_RUNNING:			return "RUNNING ";
    case THREAD_STATE_WAITING_FOREVER:		return "WAIT_FE ";
    case THREAD_STATE_WAITING_TIMEOUT:		return "WAIT_TO ";
    case THREAD_STATE_WAITING_TUNNELED_PF:	return "WAIT_TP ";
    case THREAD_STATE_LOCKED_WAITING:		return "LOCK_WT ";
    case THREAD_STATE_LOCKED_RUNNING:		return "LOCK_RU ";
    case THREAD_STATE_LOCKED_RUNNING_IPC_DONE:	return "LOCK_RD ";
    case THREAD_STATE_LOCKED_RUNNING_NESTED:	return "LOCK_RN ";
    case THREAD_STATE_POLLING:			return "POLLING ";
    case THREAD_STATE_HALTED:			return "HALTED  ";
    case THREAD_STATE_ABORTED:			return "ABORTED ";
    case THREAD_STATE_XCPU_WAITING_DELTCB:	return "XCPU_DT ";
    case THREAD_STATE_XCPU_WAITING_EXREGS:	return "XCPU_EX ";
    default:					return "UNKNOWN ";
    }
}


/* C predicates on thread_state_t (state is a plain word here); mirror the
   like-named C++ methods for api/v4/thread.c and friends. */
INLINE bool thread_state_is_runnable (const thread_state_t *self)
    { return !(self->state & 1); }
INLINE bool thread_state_is_sending (const thread_state_t *self)
    { return self->state == THREAD_STATE_POLLING || self->state == THREAD_STATE_LOCKED_RUNNING; }
INLINE bool thread_state_is_receiving (const thread_state_t *self)
    { return self->state == THREAD_STATE_WAITING_FOREVER || self->state == THREAD_STATE_WAITING_TIMEOUT ||
	     self->state == THREAD_STATE_LOCKED_WAITING; }
INLINE bool thread_state_is_halted (const thread_state_t *self)
    { return self->state == THREAD_STATE_HALTED; }
INLINE bool thread_state_is_aborted (const thread_state_t *self)
    { return self->state == THREAD_STATE_ABORTED; }
INLINE bool thread_state_is_running (const thread_state_t *self)
    { return self->state == THREAD_STATE_RUNNING; }
INLINE bool thread_state_is_waiting (const thread_state_t *self)
    { return self->state == THREAD_STATE_WAITING_FOREVER || self->state == THREAD_STATE_WAITING_TIMEOUT; }
INLINE bool thread_state_is_polling (const thread_state_t *self)
    { return self->state == THREAD_STATE_POLLING; }
INLINE bool thread_state_is_waiting_forever (const thread_state_t *self)
    { return self->state == THREAD_STATE_WAITING_FOREVER; }
INLINE bool thread_state_is_waiting_with_timeout (const thread_state_t *self)
    { return self->state == THREAD_STATE_WAITING_TIMEOUT; }
INLINE bool thread_state_is_locked_running (const thread_state_t *self)
    { return self->state == THREAD_STATE_LOCKED_RUNNING; }
INLINE bool thread_state_is_locked_waiting (const thread_state_t *self)
    { return self->state == THREAD_STATE_LOCKED_WAITING; }
INLINE bool thread_state_is_xcpu_waiting (const thread_state_t *self)
    { return self->state == THREAD_STATE_XCPU_WAITING_DELTCB || self->state == THREAD_STATE_XCPU_WAITING_EXREGS; }

#endif /* __API__V4__THREADSTATE_H__ */
