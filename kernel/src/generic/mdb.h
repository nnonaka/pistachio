/*********************************************************************
 *                
 * Copyright (C) 2004-2007,  Karlsruhe University
 *                
 * File path:     generic/mdb.h
 * Description:   Classes and access methods for the generic mapping
 *		  database.
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
 * $Id: mdb.h,v 1.10 2007/01/08 14:08:10 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __MDB_H__
#define __MDB_H__



/**
 * MDB_BITMASK: mask covering the low @a bits bits of a word.
 *
 * The mapping database packs pointers, sizes and depths into narrow
 * bitfields.  Masking a value with the width of its destination field
 * makes the (intended) truncation explicit rather than implicit.
 */
#ifndef MDB_BITMASK
#define MDB_BITMASK(bits)	(~(word_t) 0 >> (BITS_WORD - (bits)))
#endif


/*
 * mdb_ctrl_t / mdb_range_t were the nested value types mdb_t::ctrl_t and
 * mdb_t::range_t.  They are hoisted to top-level structs so they are usable
 * from C (the whole virtual mdb_t is C++-only), and typedef'd back inside
 * mdb_t below so mdb_t::ctrl_t / mdb_t::range_t keep working in C++.
 */
struct mdb_ctrl_t {
    union {
	struct {
	    word_t __pad1		: 6;
	    word_t mapctrl_self	: 1;
	    word_t unmap		: 1;
	    word_t set_rights	: 1;
	    word_t reset_status	: 1;
	    word_t deliver_status	: 1;
	    word_t set_attribute	: 1;
	    word_t __pad2		: BITS_WORD - 12;
	};
	word_t raw;
    };
};
typedef struct mdb_ctrl_t mdb_ctrl_t;

struct mdb_range_t {
    union {
	struct {
	    word_t size		: 6;
	    word_t idx		: BITS_WORD - 7;
	    word_t c		: 1;
	};
	word_t raw;
    };
};
typedef struct mdb_range_t mdb_range_t;


/**
 * The mdb_t specifies a particular mapping database, e.g., for page
 * frames, I/O ports, etc.  Certain operations on the mapping database
 * (e.g., map and mapctrl) are generic.  Other operations like
 * flushing cached entries for mappings, e.g., TLB entries in the case
 * of memory, must be defined on a per mapping database basis in
 * derived classes.
 */


/* From generic/mapping_alloc.cc */
BEGIN_DECLS
addr_t mdb_alloc_buffer (word_t size);
void mdb_free_buffer (addr_t addr, word_t size);
END_DECLS



/**
 * Data structure for holding MDB init functions.
 */
typedef struct {
    word_t priority;
    void (*function)(void);
} mdb_init_func_t;


/**
 * Declare an MDB init function.  The MDB init functions are called in
 * the order according to their priorities.
 *
 * @param prio		priority
 * @param func		function pointer
 */
#define MDB_INIT_FUNCTION(prio, func)					\
    void __attribute__ ((__section__ (".init")))			\
	__mdb_init__##prio##_##func (void);				\
    mdb_init_func_t __mdb_init__##prio##_##func##_ent			\
	__attribute__ ((__section__ (".mdb_funcs."#prio), __unused__)) 	\
	= { prio, __mdb_init__##prio##_##func };			\
    void __attribute__ ((__section__ (".init")))			\
	__mdb_init__##prio##_##func (void)


#endif /* !__MDB_H__ */
