/****************************************************************************
 *
 * Copyright (C) 2002, Karlsruhe University
 *
 * File path:	sync.h
 * Description:	Synchronization primitives.
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
 * $Id: sync.h,v 1.7 2003/09/24 19:04:24 skoglund Exp $
 *
 ***************************************************************************/

#ifndef __SYNC_H__
#define __SYNC_H__


INLINE void memory_barrier()
{
    __asm__ __volatile__ ("" ::: "memory");
}

#ifndef CONFIG_SMP

#define DEFINE_SPINLOCK(name) spinlock_t name
#define DECLARE_SPINLOCK(name) extern spinlock_t name

struct spinlock_t {
};
typedef struct spinlock_t spinlock_t;

/* Without SMP there is nothing to serialise against, so the operations are
   no-ops -- but they must exist.  The C++ version declared none at all, which
   meant any unconditional lock()/unlock() call (platform/ppc44x/bic.h has
   several) simply did not compile on a uniprocessor build, in either language. */
INLINE void spinlock_init (spinlock_t *self, word_t val) { }
INLINE void spinlock_lock (spinlock_t *self) { }
INLINE void spinlock_unlock (spinlock_t *self) { }
INLINE bool spinlock_is_locked (spinlock_t *self) { return false; }

#else /* CONFIG_SMP */
/* 
 * for smp we need a platform specific file 
 * herewith we avoid rewriting empty spinlocks 
 * for each new platform again and again and again.
 */

#include INC_ARCH(sync.h)


struct lockstate_t {
    union {
	word_t raw;
	struct {
	    BITFIELD3(word_t,
		      enabled : 1,
		      promote : 1,
		      demote  : 1);
	} X;
    } flags;
    word_t rcu_epoch;

};
typedef struct lockstate_t lockstate_t;

/* C forms of the lockstate_t predicates (the flags union is C-visible). */
INLINE bool lock_state_is_enabled (lockstate_t *self)	{ return self->flags.X.enabled; }
INLINE bool lock_state_is_active (lockstate_t *self)	{ return self->flags.raw != 0; }


#endif

#endif /* !__SYNC_H__ */
