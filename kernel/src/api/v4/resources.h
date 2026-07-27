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


#if defined(__cplusplus)
class tcb_t;
#else
struct tcb_t;
typedef struct tcb_t tcb_t;
#endif

/* Empty base (no data); EBO makes it 0 bytes in the derived, so C omits it
   entirely (the derived thread_resources_t just carries its own fields). */
#if defined(__cplusplus)
class generic_thread_resources_t
{
public:
    void dump(tcb_t * tcb) { }
    void save(tcb_t * tcb) { }
    void load(tcb_t * tcb) { }
    void purge(tcb_t * tcb) { }
    void init(tcb_t * tcb) { }
    void free(tcb_t * tcb) { }
};
#endif

#include INC_GLUE(resources.h)


#if !defined(HAVE_RESOURCE_TYPE_E)
typedef word_t	resource_bits_t;
#else


/**
 * Abstract class for handling resource bit settings.
 */
struct resource_bits_t
{
    bitmask_word_t	resource_bits;

#if defined(__cplusplus)
public:

    /**
     * Intialize resources (i.e., clear all resources).
     */
    inline void init (void)
	{ resource_bits.clear(); }

    /**
     * Clear all resources.
     */
    inline void clear (void)
	{ resource_bits.clear(); }

    /**
     * Add resource to resource bits.
     * @param t		type of resource
     * @return new resource bits
     */
    inline resource_bits_t operator += (resource_type_e t)
	{ 
	    resource_bits += (int) t;
	    return *this;
	}

    /**
     * Remove resource from resource bits.
     * @param t		type of resource
     * @return new resource bits
     */
    inline resource_bits_t operator -= (resource_type_e t)
	{
	    resource_bits -= (int) t;
	    return *this;
	}

    /**
     * Check if any resouces are registered.
     * @return true if any resources are registered, false otherwise
     */
    bool have_resources (void)
	{
	    return (word_t) resource_bits != 0;
	}

    /**
     * Check if indicated resource is registered.
     * @param t		type of resource
     * @return true if resource is registered, false otherwise
     */
    bool have_resource (resource_type_e t)
	{
	    return resource_bits.is_set ((int) t);
	}

    /**
     * Convert resource bits to a word (e.g., for printing).
     * @return the resource mask
     */
    inline operator word_t (void)
	{
	    return (word_t) resource_bits;
	}
#endif /* __cplusplus */
};
typedef struct resource_bits_t resource_bits_t;

#if !defined(__cplusplus)
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
#endif /* !__cplusplus */

#endif


#endif /* !__API__V4__RESOURCES_H__ */
