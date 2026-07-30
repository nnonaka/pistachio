/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     generic/asid.h
 * Description:   
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
 * $Id$
 *                
 ********************************************************************/

#ifndef __ASID_H__
#define __ASID_H__


#include <debug.h>
#include <config.h>      /* CONFIG_MAX_NUM_ASIDS, CONFIG_PREEMPT_ASIDS */
#include INC_API(types.h)

struct space_t; typedef struct space_t space_t;

#define ASID_INVALID	(~0U)

struct asid_t
{
    word_t asid;
    word_t timestamp;
};
typedef struct asid_t asid_t;

INLINE void   asid_init (asid_t *self)			{ self->asid = ASID_INVALID; }
INLINE void   asid_init_kernel (asid_t *self, word_t a)	{ self->asid = a; }
INLINE word_t asid_get (asid_t *self)			{ return self->asid; }
INLINE void   asid_set (asid_t *self, word_t a)		{ self->asid = a; }
INLINE bool   asid_is_valid (asid_t *self)		{ return self->asid != ASID_INVALID; }
INLINE void   asid_release (asid_t *self)		{ self->asid = ASID_INVALID; }

/*
 * We use the ASID ref array for (1) referencing the address space
 * which uses the ASID or (2) when free to link unused ASIDs in a
 * list.  When reclaiming a used ASID we can assume that no entry is
 * in the free list which means we don't record if an entry is used or
 * not.
 */

/* Was template <class T, int SIZE> class asid_manager_t.  It is instantiated
   exactly once, as asid_manager_t<space_t, CONFIG_MAX_NUM_ASIDS>, so the C form
   is that one instantiation spelled out.  The space_* operations it drives are
   declared here because space.h includes this header, not the other way round. */
#define ASID_MANAGER_SIZE	CONFIG_MAX_NUM_ASIDS

struct asid_manager_t
{
    word_t *free_list;
    union {
	space_t * asid_user[ASID_MANAGER_SIZE];
	word_t *  list_entry[ASID_MANAGER_SIZE];
    };

    word_t start, end;
    word_t timestamp;
};
typedef struct asid_manager_t asid_manager_t;

void    space_allocate_hw_asid (space_t *self, word_t hw_asid);
void    space_release_hw_asid (space_t *self, word_t hw_asid);
asid_t *space_get_asid (space_t *self);

INLINE void asid_manager_free_asid (asid_manager_t *self, word_t asid)
{
    self->list_entry[asid] = self->free_list;
    word_t **fl = &self->list_entry[asid];
    self->free_list = (word_t *) fl;
}

INLINE void asid_manager_init (asid_manager_t *self, word_t start, word_t end)
{
    word_t asid;

    self->free_list = NULL;
    self->timestamp = 0;

    /* `<=' here would clear one past the end of asid_user[] -- upstream had it
       too (see notes §140); GCC's -Waggressive-loop-optimizations flags the
       last iteration as undefined behaviour. */
    for (asid = 0; asid < ASID_MANAGER_SIZE; asid++)
	self->asid_user[asid] = NULL;

    for (asid = start; asid <= end; asid++)
	asid_manager_free_asid (self, asid);
}

INLINE void asid_manager_recycle_asid (asid_manager_t *self)
{
    word_t idx, oldest;

    for (idx = oldest = self->start; idx < self->end; idx++)
	if (space_get_asid (self->asid_user[idx])->timestamp <
	    space_get_asid (self->asid_user[oldest])->timestamp)
	    oldest = idx;
    printf("recycling ASID %x used by %p\n", oldest, self->asid_user[oldest]);
    space_release_hw_asid (self->asid_user[oldest], oldest);
    asid_release (space_get_asid (self->asid_user[oldest]));
    asid_manager_free_asid (self, oldest);
}

INLINE void asid_manager_allocate_asid (asid_manager_t *self, space_t *space)
{
    word_t *head;
    word_t asid;

    if (EXPECT_FALSE(!self->free_list))
	asid_manager_recycle_asid (self);
    ASSERT(self->free_list);
    head = self->free_list;
    self->free_list = (word_t*)*head;
    asid = ((word_t)head - (word_t)&self->list_entry) / sizeof(word_t);
    space_allocate_hw_asid (space, asid);
    asid_set (space_get_asid (space), asid);
}

INLINE word_t asid_manager_reference (asid_manager_t *self, asid_t *asid)
{
    asid->timestamp = ++self->timestamp;
    return asid->asid;
}

#endif /* !__ASID_H__ */
