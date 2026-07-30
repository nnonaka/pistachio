/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     arch/powerpc/pgent-pghash.h
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

#ifndef __ARCH__POWERPC__PGENT_PGHASH_H__
#define __ARCH__POWERPC__PGENT_PGHASH_H__

#include INC_ARCH(pghash.h)

struct space_t;   typedef struct space_t space_t;
struct mapnode_t; typedef struct mapnode_t mapnode_t;

#define HW_PGSHIFTS		{ 12, 22, 32 }
#define HW_VALID_PGSIZES	(1 << 12)

#define MDB_PGSHIFTS		{ 12, 22, 32 }
#define MDB_NUM_PGSIZES		(2)


struct pgent_t
{
    union {
	word_t		raw;
	struct {
	    word_t subtree	: 20;	// Pointer to subtree
	    word_t __pad	: 11;
	    word_t valid	: 1;	// 1 if a valid subtree.
	} tree;
	struct {
	    /* This is structured so that it can be directly inserted into
	     * the page hash table, while masking out the hardware's reserved
	     * bits.
	     */
	    word_t rpn		: 20;
	    word_t pteg_slot	: 3;	// The index into the pte group.
	    word_t referenced	: 1;
	    word_t changed	: 1;
	    word_t wimg		: 4;
	    word_t second_hash	: 1;	// 1 if used the second hash.
	    word_t pp		: 2;
	} map;
    };
};
typedef struct pgent_t pgent_t;

/* The neutral spelling kdb/generic/linear_ptab_dump.c asks every port for. */
#define PGENT_SIZE_MAX	size_max

enum pgsize_e {
    size_4k	= 0,
    size_4m	= 1,
    size_4g	= 2,
    size_max	= size_4m
};

/* The swtlb pgent declares a cache_e whose values index its three-bit
   `caching' field; the pghash pgent has no such field and upstream declares no
   cache_e at all.  It is needed all the same: space.h's add_mapping carries
   `word_t attrib = pgent_t::cache_standard' as a default argument, and
   glue/v4-powerpc calls map_device with cache_standard and cache_inhibited by
   name -- so master's C++ build of a segment-MMU configuration fails on the
   declaration just as this one failed at the call sites.  The two values are
   not invented: pgent_set_entry treats a nonzero attrib as
   PPC_PAGE_CACHE_INHIBIT and pgent_attributes reads that bit back as 1, so 0
   is the cached case and 1 the inhibited one, which is what the names must
   mean here.  Notes §144. */
enum cache_e {
    cache_standard	= 0,
    cache_inhibited	= 1
};

/* The PP field of a page hash entry.  Class-scoped as pgent_t::read_only and
   so on upstream; at file scope here, which is safe -- arch/powerpc/pgtab.h is
   the only other declarer of these names on this architecture and nothing
   includes it. */
enum permission_e {
    unused1	= 0,	// read/write
    unused2	= 1,	// read/write
    read_write	= 2,
    read_only	= 3
};

/* The operations on pgent_t are INLINE definitions in
   arch/powerpc/pgent-pghash_functions.h, which arch/powerpc/pgent.h includes
   straight after this file.  As in the swtlb pair, they are deliberately not
   prototyped here: a non-static declaration followed by a static-inline
   definition is a conflict in C, and every consumer reaches both headers
   through pgent.h. */

#endif	/* !__ARCH__POWERPC__PGENT_PGHASH_H__ */
