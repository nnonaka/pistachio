/*********************************************************************
 *                
 * Copyright (C) 2002-2003,  Karlsruhe University
 *                
 * File path:     api/v4/queuestate.h
 * Description:   queue state
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
 * $Id: queuestate.h,v 1.8 2003/09/24 19:04:24 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __API__V4__QUEUESTATE_H__
#define __API__V4__QUEUESTATE_H__

/* VU:
 * The separation of queue_state_t allows architecture specific
 * optimizations. For example, if a certain hardware architecture 
 * has some cheap bit-flipping operations, these can be used.
 */

#define IS_CONSISTENT (true)

/* Enumerator values, named as macros so C (and assembly via asmsyms) can
   reference them; the C++ state_e enum below aliases these. */
#define QUEUE_STATE_READY	1
#define QUEUE_STATE_WAKEUP	2
#define QUEUE_STATE_LATE_WAKEUP	4
#define QUEUE_STATE_WAIT	8
#define QUEUE_STATE_SEND	16
#define QUEUE_STATE_XCPU	32

struct queue_state_t
{
    word_t state;
#if defined(__cplusplus)
    enum state_e
    {
	ready		= QUEUE_STATE_READY,
	wakeup		= QUEUE_STATE_WAKEUP,
	late_wakeup	= QUEUE_STATE_LATE_WAKEUP,
	wait		= QUEUE_STATE_WAIT,
	send		= QUEUE_STATE_SEND,
	xcpu		= QUEUE_STATE_XCPU,
    };
    void init();
    void clear(state_e state);
    void set(state_e state);
    bool is_set(state_e state);
#endif /* __cplusplus */
};
typedef struct queue_state_t queue_state_t;

#if defined(__cplusplus)
INLINE void queue_state_t::init()
{
    state = 0;
}

INLINE void queue_state_t::clear(state_e state)
{
    this->state &= ~((word_t)state);
    ASSERT(IS_CONSISTENT);
}

INLINE void queue_state_t::set(state_e state)
{
    this->state |= (word_t)state;
    ASSERT(IS_CONSISTENT);
}

INLINE bool queue_state_t::is_set(state_e state)
{
    /* generates better code when checking for the value */
    return (this->state & (word_t)state) == (word_t)state;
}
#else /* !__cplusplus */
/* C forms (state is a plain word here); mirror the C++ methods. The bit is a
   QUEUE_STATE_* value. */
INLINE void queue_state_init (queue_state_t *self)		{ self->state = 0; }
INLINE void queue_state_clear (queue_state_t *self, word_t st)	{ self->state &= ~st; }
INLINE void queue_state_set (queue_state_t *self, word_t st)	{ self->state |= st; }
INLINE bool queue_state_is_set (const queue_state_t *self, word_t st) { return (self->state & st) == st; }
#endif /* __cplusplus */

#endif /* !__API__V4__QUEUESTATE_H__ */

