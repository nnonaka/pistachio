/*********************************************************************
 *                
 * Copyright (C) 2002, 2003, 2005-2007,  Karlsruhe University
 *                
 * File path:     generic/kmemory.h
 * Description:   Kernel Memory Manager
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
 * $Id: kmemory.h,v 1.11 2006/02/21 10:24:13 stoess Exp $
 *                
 ********************************************************************/
#ifndef __KMEMORY_H__
#define __KMEMORY_H__

#include <sync.h>

#if defined(CONFIG_KMEM_TRACE)
#include <kdb/linker_set.h>
#include <debug.h>

/* was `class kmem_group_t' with both members public -- a plain struct.  It sits
   under CONFIG_KMEM_TRACE, which the gate config does not set, so the collapse
   pass never compiled it.  Notes §123. */
struct kmem_group_t
{
    word_t		mem;
    const char *	name;
};
typedef struct kmem_group_t kmem_group_t;

extern linker_set_t __kmem_groups;

#define DECLARE_KMEM_GROUP(name)				\
    static kmem_group_t __kmem_group_##name = { 0, #name };	\
    kmem_group_t * name = &__kmem_group_##name;			\
    PUT_SET (__kmem_groups, __kmem_group_##name)

#else /* !CONFIG_KMEM_TRACE */

typedef word_t kmem_group_t;

/*
 * We only declare pointer as external symbol.  The reference to the
 * symbol will be omitted by GCC when it finds that the inline
 * function does not use the pointer argument.  (Of course, this will
 * not work if we do no inlining.)
 */
#define DECLARE_KMEM_GROUP(name) \
    extern kmem_group_t * name

#endif

#define EXTERN_KMEM_GROUP(name)	\
    extern kmem_group_t * name



#define KMEM_CHUNKSIZE	(word_t) (1024U)

struct kmem_t
{
    word_t *kmem_free_list;
    word_t free_chunks;
    spinlock_t spinlock;
};
typedef struct kmem_t kmem_t;

BEGIN_DECLS
void  kmem_init (kmem_t *self, void * start, void * end);
void  kmem_do_free (kmem_t *self, void * address, word_t size);
void *kmem_do_alloc (kmem_t *self, word_t size);
void *kmem_do_alloc_aligned (kmem_t *self, word_t size, word_t alignment,
			     word_t mask);
END_DECLS


#if defined(CONFIG_KMEM_TRACE)

INLINE void * kmem_alloc (kmem_t *self, kmem_group_t * group, word_t size)
{
    group->mem += size;
    return kmem_do_alloc (self, size);
}

INLINE void * kmem_alloc_aligned (kmem_t *self, kmem_group_t * group, word_t size,
		word_t alignment, word_t mask)
{
    group->mem += size;
    return kmem_do_alloc_aligned (self, size, alignment, mask);
}

INLINE void kmem_free (kmem_t *self, kmem_group_t * group, void * address, word_t size)
{
    ASSERT (group->mem >= size);
    group->mem -= size;
    kmem_do_free (self, address, size);
}

#else /* !CONFIG_KMEM_TRACE */

INLINE void * kmem_alloc (kmem_t *self, kmem_group_t * group, word_t size)
{
    (void) group;
    return kmem_do_alloc (self, size);
}

INLINE void * kmem_alloc_aligned (kmem_t *self, kmem_group_t * group, word_t size,
		word_t alignment, word_t mask)
{
    (void) group;
    return kmem_do_alloc_aligned (self, size, alignment, mask);
}

INLINE void kmem_free (kmem_t *self, kmem_group_t * group, void * address, word_t size)
{
    (void) group;
    kmem_do_free (self, address, size);
}

#endif

INLINE void kmem_add (kmem_t *self, void * address, word_t size)
{
    kmem_do_free (self, address, size);
}


/* THE kernel memory allocator */
extern kmem_t kmem;


#endif /* !__KMEMORY_H__ */
