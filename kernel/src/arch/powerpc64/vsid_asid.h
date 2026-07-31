/*********************************************************************
 *                
 * Copyright (C) 2003,  National ICT Australia (NICTA)
 *                
 * File path:     arch/powerpc64/vsid_asid.h
 * Description:   PowerPC64 specific reverse lookup ASID management
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
 * $Id: vsid_asid.h,v 1.5 2004/06/04 02:14:26 cvansch Exp $
 *                
 ********************************************************************/

#ifndef __ARCH__POWERPC64__VSID_ASID_H__
#define __ARCH__POWERPC64__VSID_ASID_H__

#define ASID_INVALID	    (~0ul)

#define ASID_BITS	    (POWERPC64_VIRTUAL_BITS - POWERPC64_USER_BITS)
#define ASID_MAX	    (1ul << ASID_BITS)

#define VSID_REVERSE_SHIFT  (POWERPC64_USER_BITS - POWERPC64_SEGMENT_BITS)

struct space_t;
typedef struct space_t space_t;

struct vce_t
{
    word_t  asid;
    space_t *space;
};
typedef struct vce_t vce_t;

INLINE bool vce_is_valid (vce_t *self) { return self->asid != ASID_INVALID; }


struct vsid_asid_cache_t
{
    vce_t cache[ ASID_MAX ];
    s64_t first_free;
};
typedef struct vsid_asid_cache_t vsid_asid_cache_t;

struct vsid_asid_t
{
    word_t vsid_asid;
};
typedef struct vsid_asid_t vsid_asid_t;

/* Out of line in arch/powerpc64/vsid_asid.c. */
BEGIN_DECLS
word_t vsid_asid_cache_alloc (vsid_asid_cache_t *self, space_t *space);
void   vsid_asid_cache_release (vsid_asid_cache_t *self, word_t asid);
void   vsid_asid_free (vsid_asid_t *self);
END_DECLS

INLINE void vsid_asid_init (vsid_asid_t *self) { self->vsid_asid = ASID_INVALID; }

INLINE vsid_asid_cache_t *get_vsid_asid_cache(void)
{
    extern vsid_asid_cache_t vsid_asid_cache;
    return &vsid_asid_cache;
}

INLINE void vsid_asid_cache_init (vsid_asid_cache_t *self, space_t *kernel_space)
{
    for ( word_t i = 0; i < ( ASID_MAX ); i++ )
    {
	self->cache[i].asid = ASID_INVALID;
	self->cache[i].space = NULL;
    }
    self->cache[0].asid = 0;
    self->cache[0].space = kernel_space;

    self->first_free = 1;
}

INLINE space_t *vsid_asid_cache_lookup (vsid_asid_cache_t *self, word_t vsid)
{
    return self->cache[ (vsid >> VSID_REVERSE_SHIFT) & (ASID_MAX-1) ].space;
}

INLINE word_t vsid_asid_get (vsid_asid_t *self, space_t *space)
{
    if ( self->vsid_asid == ASID_INVALID )
	self->vsid_asid = vsid_asid_cache_alloc (get_vsid_asid_cache(), space);

    return self->vsid_asid;
}

#endif /* __ARCH__POWERPC64__VSID_ASID_H__ */
