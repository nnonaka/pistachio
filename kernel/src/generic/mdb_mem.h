/*********************************************************************
 *                
 * Copyright (C) 2005, 2007,  Karlsruhe University
 *                
 * File path:     generic/mdb_mem.h
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
 * $Id: mdb_mem.h,v 1.6 2007/01/08 14:08:10 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __MDB_MEM_H__
#define __MDB_MEM_H__

#include <mdb.h>
#include INC_GLUE(mdb.h)

/*
 * The memory mapping database: the mdb_t implementation for physical page
 * frames.  class mdb_mem_t was removed from this header by f3d2a88 along with
 * the mdb_t it derived from; restored here in C (see notes §110).
 *
 * mdb_mem_t derived from mdb_t and added no data, so in C it is an mdb_t whose
 * ops table is mdb_mem_ops -- no wrapper struct is needed, and the one
 * instance keeps its name.
 */

struct space_t; typedef struct space_t space_t;

BEGIN_DECLS
extern const mdb_ops_t mdb_mem_ops;
extern mdb_t	     mdb_mem;
extern mdb_node_t *  sigma0_memnode;
extern word_t	     mdb_mem_sizes[];
extern word_t	     mdb_mem_num_sizes;
END_DECLS


/* was class mdb_mem_misc_t; only ever built and read as a raw word */
typedef union mdb_mem_misc_t {
    word_t	raw;
    struct {
	word_t	pgsize		: 5;
	word_t	purged_status	: 3;
	word_t	space		: BITS_WORD - 8;
    };
} mdb_mem_misc_t;

/* The C++ default argument (stat = 0) becomes a second entry point. */
INLINE word_t mdb_mem_misc_stat (space_t *spc, word_t pgsz, word_t stat)
{
    mdb_mem_misc_t misc;
    word_t sp = ((word_t) spc) >> 8;

    misc.raw = 0;
    misc.pgsize = pgsz & MDB_BITMASK (5);
    misc.purged_status = stat & MDB_BITMASK (3);
    misc.space = sp & MDB_BITMASK (BITS_WORD - 8);
    return misc.raw;
}

INLINE word_t mdb_mem_misc (space_t *spc, word_t pgsz)
{ return mdb_mem_misc_stat (spc, pgsz, 0); }

#endif /* !__MDB_MEM_H__ */
