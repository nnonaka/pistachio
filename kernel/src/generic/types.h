/****************************************************************************
 *
 * Copyright (C) 2002, Karlsruhe University
 *
 * File path:	types.h
 * Description:	Defines some basic, global types.
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
 * $Id: types.h,v 1.14 2006/10/18 11:45:55 reichelt Exp $
 *
 ***************************************************************************/

#ifndef __TYPES_H__
#define __TYPES_H__

#if !defined(ASSEMBLY)

#include INC_ARCH(types.h)
/* At this point we should have word_t defined */

/*
 * The kernel's C++ sources use bool/true/false freely.  Headers shared with
 * C translation units (during the C++ -> C conversion) must therefore parse
 * these in C too.  _Bool is a C99/gnu11 keyword; this mirrors <stdbool.h>
 * without depending on the (nostdinc) include search path.
 */
typedef _Bool bool;
#define true  1
#define false 0

#if defined(CONFIG_IS_32BIT)
#define SIZE_T unsigned int
#else
#define SIZE_T long unsigned int
#endif

/**
 * Size type.  For use in new operator, etc.
 */
typedef SIZE_T		size_t;


/**
 *	addr_t - data type to store addresses
 */
typedef void*			addr_t;


/**
 * Type to use when addresses are converted to integers
 */
typedef word_t		addr_word_t;



/**
 * Add offset to address.
 * @param addr		original address
 * @param off		offset to add
 * @return new address
 */
INLINE addr_t addr_offset(addr_t addr, word_t off)
{
    return (addr_t)((word_t)addr + off);
}

/**
 * Apply mask to an address.
 * @param addr		original address
 * @param mask		address mask
 * @return new address
 */
INLINE addr_t addr_mask (addr_t addr, word_t mask)
{
    return (addr_t) ((word_t) addr & mask);
}

/**
 * Align address downwards.  It is assumed that the alignment is a power of 2.
 * @param addr		original address
 * @param align		alignment
 * @return new address
 */
INLINE addr_t addr_align (addr_t addr, word_t align)
{
    return addr_mask (addr, ~(align - 1));
}

/**
 * Align address upwards.  It is assumed that the alignment is a power of 2.
 * @param addr		original address
 * @param align		alignment
 * @return new address
 */
INLINE addr_t addr_align_up (addr_t addr, word_t align)
{
    return addr_mask (addr_offset (addr, align - 1), ~(align - 1));
}

/*
 * The same two operations on a paddr_t.  In C++ they were overloads of the
 * above and the compiler picked between them; in C the caller has to.  Where
 * paddr_t is addr_t -- x86, powerpc64 -- the two are the same function, and
 * these forward.  An architecture whose paddr_t is wider than a pointer
 * (powerpc's is u64_t on ppc44x) defines HAVE_ARCH_PADDR_OPS and supplies its
 * own, because forwarding here would truncate.  Notes §140.
 */
#if !defined(HAVE_ARCH_PADDR_OPS)
INLINE paddr_t paddr_offset (paddr_t addr, word_t off)
{
    return (paddr_t) addr_offset ((addr_t) addr, off);
}

INLINE paddr_t paddr_mask (paddr_t addr, word_t mask)
{
    return (paddr_t) addr_mask ((addr_t) addr, mask);
}
#endif



#ifndef NULL
#define NULL 0
#endif

#define BITS_WORD	(sizeof(word_t)*8)
#define BYTES_WORD	(sizeof(word_t))

#endif /* !defined(ASSEMBLY) */


/*
 * Concrete instantiation of ringlist_t<T> used as a struct member
 * (ringlist_t<tcb_t> in tcb_t / rr_sched_ktcb_t).  In C++ it aliases the
 * template above; in C it is the plain two-pointer struct.
 */
struct tcb_t;
struct ringlist_tcb_t { struct tcb_t *next; struct tcb_t *prev; };
typedef struct ringlist_tcb_t ringlist_tcb_t;

#endif /* !__TYPES_H__ */
