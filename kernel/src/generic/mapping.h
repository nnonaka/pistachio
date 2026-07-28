/*********************************************************************
 *                
 * Copyright (C) 2000, 2001, 2002, 2003, 2005,  Karlsruhe University
 *                
 * File path:     mapping.h
 * Description:   Generic mapping databse structures
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
 * $Id: mapping.h,v 1.7 2005/12/22 12:35:58 stoess Exp $
 *                
 ********************************************************************/
#ifndef __MAPPING_H__
#define __MAPPING_H__

struct space_t;
struct pgent_t;
struct mapnode_t;
struct rootnode_t;

/*
 * ptab.h provides the (architecture specific) MDB_NUM_PGSIZES constant and
 * page-shift tables, which C consumers of this header need.  The pgent_t and
 * fpage_t class machinery, on the other hand, is only touched by the C++
 * method bodies below, so it is guarded off for the C path.
 */
#include INC_ARCH_SA(ptab.h)


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


/**
 * mdb_pgshifts: page sizes supported by the mapping database
 *
 * The mdb_pgshifts[] is an architecture specific arrray defining the
 * page sizes that should be supported by the mapping databse.
 * Actually, it defines the shift numbers rather than the page sizes.
 * The last entry in the array should define the whole address space.
 * E.g, if page sizes of 1MB, 64KB, 4KB, and 1KB are supported, the
 * array would (on a 32 bit address space) contain the values: 10, 12,
 * 16, 20, and 32.
 *
 */
extern word_t mdb_pgshifts[];


struct mdb_mng_t;
struct mdb_buflist_t
{
    word_t	size;
    struct mdb_mng_t	*list_of_lists;
    word_t	max_free;
};
typedef struct mdb_buflist_t mdb_buflist_t;


/**
 * mdbbuflists: allocation sizes for mapping database
 *
 * The mdb_buflists[] array defines the buffer sizes that will be
 * needed by the mapping database.  These sizes depend on the page
 * sizes to be supported, and must also contain the number 3*wordsize
 * (for mapnode_t) and 2*wordsize (for dualnote_t).  The array should
 * be terminated by defining a zero buffer size.
 *
 */
extern mdb_buflist_t mdb_buflists[];


/* Top level mapping node for sigma0. */
extern struct mapnode_t * sigma0_mapnode;



/**
 * dualnode_t: node containing pointers to root array and mapping tree
 */
struct dualnode_t
{
    struct mapnode_t	*map;
    struct rootnode_t	*root;
};
typedef struct dualnode_t dualnode_t;

#ifndef MDB_SPACE_BITS
#define MDB_SPACE_BITS	(BITS_WORD - 11)
#endif

/**
 * mapnode_t: node for holding a mapping databse entry
 */
struct mapnode_t
{
    union {
	struct {
	    word_t is_prev_root		: 1;
	    word_t prev_ptr		: BITS_WORD - 1;
	    word_t tree_depth		: BITS_WORD - MDB_SPACE_BITS - 3;
	    word_t rwx			: 3;
	    word_t space		: MDB_SPACE_BITS;
	    word_t is_next_root		: 1;
	    word_t is_next_map		: 1;
	    word_t next_ptr		: BITS_WORD - 2;
	} x;
	word_t raw[3];
    };


} __attribute__ ((packed));
typedef struct mapnode_t mapnode_t;



/**
 * rootnode_t: root array node representing a physical page frame
 */
struct rootnode_t
{
    union {
	struct {
	    word_t is_next_root		: 1;
	    word_t is_next_map		: 1;
	    word_t next_ptr		: BITS_WORD - 2;
	} x;
	word_t raw;
    };

};
typedef struct rootnode_t rootnode_t;


/*
 * C reimplementations of the mapnode_t/rootnode_t methods for mapping.c (their
 * bitfield data is C-visible; only the methods were C++-only).  The C++
 * overloads become distinctly-named functions; pointer/space typing uses the
 * elaborated structs that this header forward-declares for C.
 */
#define MDB_PGSIZE_MAX	(MDB_NUM_PGSIZES - 1)

/* --- mapnode_t: previous pointer / backlink --- */
INLINE struct pgent_t * mapnode_get_pgent (mapnode_t *self, void *prev)
{ return (struct pgent_t *) (((word_t) self->x.prev_ptr << 1) ^ (word_t) prev); }

INLINE mapnode_t * mapnode_get_prevmap (mapnode_t *self, struct pgent_t *pg)
{ return self->x.is_prev_root ? (mapnode_t *) 0
	: (mapnode_t *) (((word_t) self->x.prev_ptr << 1) ^ (word_t) pg); }

INLINE rootnode_t * mapnode_get_prevroot (mapnode_t *self, struct pgent_t *pg)
{ return (! self->x.is_prev_root) ? (rootnode_t *) 0
	: (rootnode_t *) (((word_t) self->x.prev_ptr << 1) ^ (word_t) pg); }

INLINE void mapnode_set_backlink_map (mapnode_t *self, mapnode_t *prev, struct pgent_t *pg)
{
    word_t p = ((word_t) prev ^ (word_t) pg) >> 1;
    self->x.prev_ptr = p & MDB_BITMASK (BITS_WORD - 1);
    self->x.is_prev_root = 0;
}

INLINE void mapnode_set_backlink_root (mapnode_t *self, rootnode_t *prev, struct pgent_t *pg)
{
    word_t p = ((word_t) prev ^ (word_t) pg) >> 1;
    self->x.prev_ptr = p & MDB_BITMASK (BITS_WORD - 1);
    self->x.is_prev_root = 1;
}

INLINE bool mapnode_is_prev_root (mapnode_t *self)	{ return self->x.is_prev_root; }

/* --- mapnode_t: next pointer --- */
INLINE mapnode_t * mapnode_get_nextmap (mapnode_t *self)
{
    if (! self->x.is_next_map)
	return (mapnode_t *) 0;
    else if (self->x.is_next_root)
	return (mapnode_t *) ((dualnode_t *) ((word_t) self->x.next_ptr << 2))->map;
    else
	return (mapnode_t *) ((word_t) self->x.next_ptr << 2);
}

INLINE rootnode_t * mapnode_get_nextroot (mapnode_t *self)
{
    if (! self->x.is_next_root)
	return (rootnode_t *) 0;
    else if (self->x.is_next_map)
	return (rootnode_t *) ((dualnode_t *) ((word_t) self->x.next_ptr << 2))->root;
    else
	return (rootnode_t *) ((word_t) self->x.next_ptr << 2);
}

INLINE dualnode_t * mapnode_get_nextdual (mapnode_t *self)
{
    return (self->x.is_next_root && self->x.is_next_map)
	? (dualnode_t *) ((word_t) self->x.next_ptr << 2) : (dualnode_t *) 0;
}

INLINE void mapnode_set_next_map (mapnode_t *self, mapnode_t *map)
{
    word_t p = ((word_t) map) >> 2;
    self->x.next_ptr = p & MDB_BITMASK (BITS_WORD - 2);
    self->x.is_next_root = 0;
    self->x.is_next_map = 1;
}

INLINE void mapnode_set_next_root (mapnode_t *self, rootnode_t *root)
{
    word_t p = ((word_t) root) >> 2;
    self->x.next_ptr = p & MDB_BITMASK (BITS_WORD - 2);
    self->x.is_next_root = 1;
    self->x.is_next_map = 0;
}

INLINE void mapnode_set_next_dual (mapnode_t *self, dualnode_t *dual)
{
    word_t p = ((word_t) dual) >> 2;
    self->x.next_ptr = p & MDB_BITMASK (BITS_WORD - 2);
    self->x.is_next_root = 1;
    self->x.is_next_map = 1;
}

INLINE bool mapnode_is_next_root (mapnode_t *self)
{ return self->x.is_next_root && !self->x.is_next_map; }
INLINE bool mapnode_is_next_map (mapnode_t *self)
{ return self->x.is_next_map && !self->x.is_next_root; }
INLINE bool mapnode_is_next_both (mapnode_t *self)
{ return self->x.is_next_root && self->x.is_next_map; }

/* --- mapnode_t: space / rwx / depth --- */
INLINE struct space_t * mapnode_get_space (mapnode_t *self)
{ return (struct space_t *) ((word_t) self->x.space << (BITS_WORD - MDB_SPACE_BITS)); }

INLINE void mapnode_set_space (mapnode_t *self, struct space_t *space)
{
    word_t s = ((word_t) space) >> (BITS_WORD - MDB_SPACE_BITS);
    self->x.space = s & MDB_BITMASK (MDB_SPACE_BITS);
}

INLINE word_t mapnode_get_rwx (mapnode_t *self)		{ return self->x.rwx; }
INLINE void mapnode_set_rwx (mapnode_t *self, word_t rwx)  { self->x.rwx = rwx & MDB_BITMASK (3); }
INLINE void mapnode_update_rwx (mapnode_t *self, word_t rwx) { self->x.rwx |= rwx & MDB_BITMASK (3); }

INLINE word_t mapnode_get_depth (mapnode_t *self)	{ return self->x.tree_depth; }
INLINE void mapnode_set_depth (mapnode_t *self, word_t depth)
{ self->x.tree_depth = depth & MDB_BITMASK (BITS_WORD - MDB_SPACE_BITS - 3); }

/* --- rootnode_t --- */
INLINE mapnode_t * rootnode_get_map (rootnode_t *self)
{
    if (! self->x.is_next_map)
	return (mapnode_t *) 0;
    else if (self->x.is_next_root)
	return (mapnode_t *) ((dualnode_t *) ((word_t) self->x.next_ptr << 2))->map;
    else
	return (mapnode_t *) ((word_t) self->x.next_ptr << 2);
}

INLINE rootnode_t * rootnode_get_root (rootnode_t *self)
{
    if (! self->x.is_next_root)
	return (rootnode_t *) 0;
    else if (self->x.is_next_map)
	return (rootnode_t *) ((dualnode_t *) ((word_t) self->x.next_ptr << 2))->root;
    else
	return (rootnode_t *) ((word_t) self->x.next_ptr << 2);
}

INLINE dualnode_t * rootnode_get_dual (rootnode_t *self)
{
    return (self->x.is_next_root && self->x.is_next_map)
	? (dualnode_t *) ((word_t) self->x.next_ptr << 2) : (dualnode_t *) 0;
}

INLINE void rootnode_set_ptr_map (rootnode_t *self, mapnode_t *map)
{
    word_t p = (word_t) map >> 2;
    self->x.next_ptr = p & MDB_BITMASK (BITS_WORD - 2);
    self->x.is_next_root = 0;
    self->x.is_next_map = 1;
}

INLINE void rootnode_set_ptr_root (rootnode_t *self, rootnode_t *root)
{
    word_t p = (word_t) root >> 2;
    self->x.next_ptr = p & MDB_BITMASK (BITS_WORD - 2);
    self->x.is_next_root = 1;
    self->x.is_next_map = 0;
}

INLINE void rootnode_set_ptr_dual (rootnode_t *self, dualnode_t *dual)
{
    word_t p = (word_t) dual >> 2;
    self->x.next_ptr = p & MDB_BITMASK (BITS_WORD - 2);
    self->x.is_next_root = 1;
    self->x.is_next_map = 1;
}

INLINE bool rootnode_is_next_root (rootnode_t *self)
{ return self->x.is_next_root && !self->x.is_next_map; }
INLINE bool rootnode_is_next_map (rootnode_t *self)
{ return self->x.is_next_map && !self->x.is_next_root; }
INLINE bool rootnode_is_next_both (rootnode_t *self)
{ return self->x.is_next_root && self->x.is_next_map; }


/**
 * mdb_pgshifts: array of bit-shifts for mapping db page tables
 *
 * Array containing the actual page sizes (as bit-shifts) for the
 * various mapping databage page size numbers.  Array is indexed by
 * page size number.  Last entry must be the bit-shift for the
 * complete mappable address space.
 */
extern word_t mdb_pgshifts[];


/*
 * Define some operators on the pgsize enum to make code more
 * readable.
 */


/* init_mdb is called from init.cc (C++) but defined in mapping.c (C). */
BEGIN_DECLS
void init_mdb (void);
END_DECLS

/* From generic/mapping_alloc.cc */
BEGIN_DECLS
addr_t mdb_alloc_buffer (word_t size);
void mdb_free_buffer (addr_t addr, word_t size);
END_DECLS

/* C-linkage wrappers for mdb_map/mdb_flush (defined in mapping.cc), so
   linear_ptab_walker.c can drive the mapping database. The hwpgsize args are
   word_t (an X86_PGSIZE_* value); all struct types are elaborated and fpage_t
   passes by pointer so this header needs no complete pgent_t/fpage_t (it is
   also included by plain C files such as mapping_alloc.c). */
struct fpage_t;
BEGIN_DECLS
struct mapnode_t * mdb_map_c (struct mapnode_t * f_map, struct pgent_t * f_pg,
			      word_t f_hwpgsize, addr_t f_addr,
			      struct pgent_t * t_pg, word_t t_hwpgsize,
			      struct space_t * t_space, bool grant);
word_t mdb_flush_c (struct mapnode_t * f_map, struct pgent_t * f_pg,
		    word_t f_hwpgsize, addr_t f_addr,
		    word_t t_hwpgsize, struct fpage_t * fp, bool unmap_self);
END_DECLS


#endif /* !__MAPPING_H__ */
