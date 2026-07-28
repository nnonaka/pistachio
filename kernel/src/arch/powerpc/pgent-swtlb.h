/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     arch/powerpc/pgent-swtlb.h
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
#ifndef __ARCH__POWERPC__PGENT_SWTLB_H__
#define __ARCH__POWERPC__PGENT_SWTLB_H__


struct space_t;   typedef struct space_t space_t;
struct mapnode_t; typedef struct mapnode_t mapnode_t;

#define HW_PGSHIFTS		{ 12, 22, 32 }

#define MDB_BUFLIST_SIZES	{ {12}, {8}, {4096}, {0} }
#define MDB_PGSHIFTS		{ 12, 22, 32 }
#define MDB_NUM_PGSIZES		(2)

struct pgent_t
{
    union {
	word_t raw;
	struct {
	    word_t subtree	: 20;	// Pointer to subtree
	    word_t is_subtree	: 3;	// cache_subtree if subtree
	    word_t __pad	: 2;
	    word_t erpn		: 4;
	    word_t valid	: 3;	// != 0 if valid
	} tree;
	struct {
	    /* This is structured so that it can be "easily" inserted
	     * into the TLB
	     */
	    word_t epn		: 20;	// we only support 4k
	    word_t caching	: 3;	// cache_e
	    word_t referenced	: 1;	// 23
	    word_t changed	: 1;	// 24
	    word_t erpn		: 4;	// undef, UX, UW, UR, 25-28
	    word_t execute	: 1;	// SX 29
	    word_t write	: 1;	// SW 30
	    word_t read		: 1;	// SR 31
	} map;
    } __attribute__((packed));
};
typedef struct pgent_t pgent_t;

enum cache_e {
    cache_standard	= 0,
    cache_inhibited	= 1,
    cache_coherent	= 2,
    cache_guarded	= 3,
    cache_write_through	= 4,
    cache_subtree	= 7,
};

/* The neutral spelling kdb/generic/linear_ptab_dump.c asks every port for. */
#define PGENT_SIZE_MAX	size_max

enum pgsize_e {
    size_4k	= 0,
    size_4m	= 1,
    size_4g	= 2,
    size_max	= size_4m
};

/* The operations on pgent_t are INLINE definitions in
   arch/powerpc/pgent-swtlb_functions.h, which arch/powerpc/pgent.h includes
   straight after this file.  They are deliberately not prototyped here: a
   non-static declaration followed by a static-inline definition is a conflict
   in C, and every consumer reaches both headers through pgent.h. */


#endif /* !__ARCH__POWERPC__PGENT_SWTLB_H__ */
