/*********************************************************************
 *                
 * Copyright (C) 2002-2007, 2009,  Karlsruhe University
 *                
 * File path:     api/v4/types.h
 * Description:   General type declarations for V4 API
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
 * $Id: types.h,v 1.26 2006/10/18 11:57:20 reichelt Exp $
 *                
 ********************************************************************/
#ifndef __API__V4__TYPES_H__
#define __API__V4__TYPES_H__

#include INC_API(config.h)

#if !defined(TIME_BITS_WORD)
#if defined(CONFIG_IS_64BIT)
#define TIME_BITS_WORD 64
#elif defined(CONFIG_IS_32BIT)
#define TIME_BITS_WORD 32
#endif
#endif /* !defined(TIME_BITS_WORD) */

/* time */
struct time_t
{

    union {
	u16_t raw;
	struct {
	    BITFIELD3(u16_t,
		mantissa	: 10,
		exponent	: 5,
		type		: 1);
	} __attribute__((packed)) time;
    } __attribute__((packed));
} __attribute__((packed));
typedef struct time_t time_t;

/* C forms of the time_t predicates (raw/bitfields are C-visible); mirrors the
   C++ is_never/is_zero.  get_microseconds and operator< are wrapped in C++
   (time_get_microseconds/time_lt, declared in api/v4/tcb.h). */
INLINE bool time_is_never (const time_t *self) { return self->raw == 0; }
INLINE bool time_is_period (const time_t *self) { return self->time.type == 0; }
INLINE bool time_is_zero (const time_t *self)
{
    time_t z;
    z.raw = 0;
    z.time.mantissa = 0;
    z.time.exponent = 1;
    z.time.type = 0;
    return z.raw == self->raw;
}



struct timeout_t
{
    union {
	struct {
#if TIME_BITS_WORD == 64
	    BITFIELD4( time_t,
		rcv_timeout,
		snd_timeout,
		_rv0,
		_rv1
	    );
#elif TIME_BITS_WORD == 32
	    BITFIELD2( time_t,
		rcv_timeout,
		snd_timeout
	    );
#endif
	} __attribute__((packed)) x;
	word_t raw;
    };

};
typedef struct timeout_t timeout_t;

INLINE timeout_t timeout_never (void) { timeout_t t; t.raw = 0; return t; }
INLINE time_t timeout_get_rcv (const timeout_t *self) { return self->x.rcv_timeout; }
INLINE time_t timeout_get_snd (const timeout_t *self) { return self->x.snd_timeout; }


typedef u16_t cpuid_t;

#endif /* __API__V4__TYPES_H__ */
