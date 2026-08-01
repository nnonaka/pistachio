/*********************************************************************
 *                
 * Copyright (C) 2002, 2003, 2007-2008,  Karlsruhe University
 *                
 * File path:     api/v4/resources.h
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
 * $Id: resources.h,v 1.5 2003/09/24 19:04:24 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __API__V4__RESOURCES_H__
#define __API__V4__RESOURCES_H__

#include <bitmask.h>


struct tcb_t;
typedef struct tcb_t tcb_t;

/* Empty base (no data); EBO makes it 0 bytes in the derived, so C omits it
   entirely (the derived thread_resources_t just carries its own fields). */

#include INC_GLUE(resources.h)


#if !defined(HAVE_RESOURCE_TYPE_E)
typedef word_t	resource_bits_t;

/* Same five entry points over the plain-word form, so callers do not have to
   know which branch their architecture took.  powerpc64 is the one port that
   declares no resource_type_e; upstream reached the word directly (`if
   (current->resource_bits)'), which the struct form below cannot serve. */
INLINE void resource_bits_init (resource_bits_t *self)
    { *self = 0; }
INLINE bool resource_bits_have_resource (resource_bits_t *self, word_t t)
    { return (*self & (1UL << t)) != 0; }
INLINE void resource_bits_add (resource_bits_t *self, word_t t)
    { *self |= (1UL << t); }
INLINE void resource_bits_remove (resource_bits_t *self, word_t t)
    { *self &= ~(1UL << t); }
INLINE bool resource_bits_have_resources (resource_bits_t *self)
    { return *self != 0; }
INLINE word_t resource_bits_raw (const resource_bits_t *self)
    { return *self; }
#else


/**
 * Abstract class for handling resource bit settings.
 */
struct resource_bits_t
{
    bitmask_word_t	resource_bits;

};
typedef struct resource_bits_t resource_bits_t;

/* C accessors for resource_bits_t: the C++ methods (init/have_resource/+=/-=)
   above poke bitmask_word_t::maskvalue, which is private to the C++ bitmask_t,
   so C reaches the plain struct member directly. Semantics match bitmask.h.
   The resource type is taken as word_t (not resource_type_e) because the glue
   header that defines that enum includes this one *before* declaring it. */
INLINE void resource_bits_init (resource_bits_t *self)
    { self->resource_bits.maskvalue = 0; }
INLINE bool resource_bits_have_resource (resource_bits_t *self, word_t t)
    { return (self->resource_bits.maskvalue & (1UL << t)) != 0; }
INLINE void resource_bits_add (resource_bits_t *self, word_t t)
    { self->resource_bits.maskvalue |= (1UL << t); }
INLINE void resource_bits_remove (resource_bits_t *self, word_t t)
    { self->resource_bits.maskvalue &= ~(1UL << t); }
INLINE bool resource_bits_have_resources (resource_bits_t *self)
    { return self->resource_bits.maskvalue != 0; }
INLINE word_t resource_bits_raw (const resource_bits_t *self)
    { return self->resource_bits.maskvalue; }

#endif


#endif /* !__API__V4__RESOURCES_H__ */
