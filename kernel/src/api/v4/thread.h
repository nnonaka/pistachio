/*********************************************************************
 *                
 * Copyright (C) 2002-2008, 2010,  Karlsruhe University
 *                
 * File path:     api/v4/thread.h
 * Description:   thread ids
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
 * $Id: thread.h,v 1.19 2006/10/18 11:24:22 reichelt Exp $
 *                
 ********************************************************************/
#ifndef __API__V4__THREAD_H__
#define __API__V4__THREAD_H__

#include INC_API(config.h)
#include INC_API(kernelinterface.h)

#if !defined(TID_GLOBAL_VERSION_BITS)
#define TID_GLOBAL_VERSION_BITS		L4_GLOBAL_VERSION_BITS
#define TID_GLOBAL_THREADNO_BITS	L4_GLOBAL_THREADNO_BITS
#define TID_LOCAL_ID_ZERO_BITS		L4_LOCAL_ID_ZERO_BITS
#define TID_LOCAL_ID_BITS		L4_LOCAL_ID_BITS
#endif /* !defined(TID_GLOBAL_VERSION_BITS) */

struct threadid_t
{
    /* Data is public: the threadid_* free functions are non-members. */
    union {
	word_t raw;

	struct {
	    BITFIELD2( word_t,
		       zero	: TID_LOCAL_ID_ZERO_BITS,
		       id	: TID_LOCAL_ID_BITS );
	} local;

	struct {
	    BITFIELD2( word_t,
		       version	: TID_GLOBAL_VERSION_BITS,
		       threadno	: TID_GLOBAL_THREADNO_BITS );
	} global;
    };
} __attribute__((packed));
typedef struct threadid_t threadid_t;

/*
 * threadid_t C free-function API -- the single source of truth for the
 * threadid_t operations, usable from both C and C++.  The C++ methods above
 * are thin forwarders to these (defined at the bottom of this file).
 */

INLINE word_t threadid_get_raw (const threadid_t *self)     { return self->raw; }
INLINE void   threadid_set_raw (threadid_t *self, word_t raw){ self->raw = raw; }
INLINE void   threadid_set (threadid_t *self, threadid_t tid){ self->raw = tid.raw; }

INLINE threadid_t threadid_anythread (void)
{
    threadid_t tid;
    tid.raw = (word_t) (-1UL);
    return tid;
}

INLINE threadid_t threadid_anylocalthread (void)
{
    threadid_t tid;
    tid.local.zero = 0;
    tid.local.id = ~0UL >> (BITS_WORD - TID_LOCAL_ID_BITS);
    return tid;
}

INLINE threadid_t threadid_nilthread (void)
{
    threadid_t tid;
    tid.raw = 0;
    return tid;
}

INLINE threadid_t threadid_irqthread (word_t irq)
{
    threadid_t tid;
    tid.global.version = 1;
    tid.global.threadno = irq & (~0UL >> (BITS_WORD - TID_GLOBAL_THREADNO_BITS));
    return tid;
}

INLINE threadid_t threadid_idlethread (void)
{
    threadid_t tid;
    tid.raw = (word_t)0x1d1e1d1e1d1e1d1eULL;
    return tid;
}

INLINE threadid_t threadid_global (word_t threadno, word_t version)
{
    threadid_t tid;
    tid.global.version = version & (~0UL >> (BITS_WORD - TID_GLOBAL_VERSION_BITS));
    tid.global.threadno = threadno & (~0UL >> (BITS_WORD - TID_GLOBAL_THREADNO_BITS));
    return tid;
}

INLINE threadid_t threadid_from_raw (word_t rawid)
{
    threadid_t t;
    t.raw = rawid;
    return t;
}

INLINE bool threadid_is_global (const threadid_t *self)  { return self->local.zero != 0; }
INLINE bool threadid_is_local (const threadid_t *self)   { return self->local.zero == 0; }
INLINE bool threadid_is_nilthread (const threadid_t *self)  { return self->raw == 0; }
INLINE bool threadid_is_anythread (const threadid_t *self)  { return self->raw == (word_t) (-1UL); }
INLINE bool threadid_is_anylocalthread (const threadid_t *self)
{
    return self->raw == threadid_anylocalthread().raw;
}

INLINE word_t threadid_get_threadno (const threadid_t *self) { return self->global.threadno; }
INLINE word_t threadid_get_version (const threadid_t *self)  { return self->global.version; }
INLINE word_t threadid_get_irqno (const threadid_t *self)    { return threadid_get_threadno (self); }

INLINE bool threadid_is_interrupt (const threadid_t *self)
{
    return (self->global.version == 1) &&
	   (self->global.threadno < get_kip()->thread_info.system_base);
}

INLINE void threadid_set_global_id (threadid_t *self, word_t threadno, word_t version)
{
    self->global.threadno = threadno & (~0UL >> (BITS_WORD - TID_GLOBAL_THREADNO_BITS));
    self->global.version = version & (~0UL >> (BITS_WORD - TID_GLOBAL_VERSION_BITS));
}

INLINE bool threadid_equals (const threadid_t *a, const threadid_t *b)     { return a->raw == b->raw; }
INLINE bool threadid_not_equals (const threadid_t *a, const threadid_t *b) { return a->raw != b->raw; }



/* special thread ids */
#define NILTHREAD	(threadid_nilthread())
#define ANYTHREAD	(threadid_anythread())
#define ANYLOCALTHREAD	(threadid_anylocalthread())
#define IDLETHREAD	(threadid_idlethread())


#endif /* !__API__V4__THREAD_H__ */
