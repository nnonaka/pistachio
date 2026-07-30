/*********************************************************************
 *                
 * Copyright (C) 2005-2007,  Karlsruhe University
 *                
 * File path:     generic/vrt.h
 * Description:   Generic Variable Radix Tables
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
 * $Id: vrt.h,v 1.9 2006/06/12 17:02:30 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __VRT_H__
#define __VRT_H__

#include INC_API(fpage.h)
#include <mdb.h>

#if !defined(MAX_VRT_DEPTH)
#define MAX_VRT_DEPTH 10
#endif

struct vrt_t;		typedef struct vrt_t vrt_t;
struct vrt_node_t;	typedef struct vrt_node_t vrt_node_t;
struct vrt_table_t;	typedef struct vrt_table_t vrt_table_t;


/**
 * The vrt_t specifies a particular variable radix table.  Certain
 * operations on the VRT (e.g., map and mapctrl) are generic.  Other
 * operations like operating on access rights are defined on a per VRT
 * basis -- those were virtual, and are an ops table in C.  vrt_t's only
 * data member follows the pointer, reproducing the C++ layout (vptr first),
 * so a derived type embeds vrt_t as its first member.
 */
typedef struct vrt_ops_t {
    word_t	 (*get_radix)		(vrt_t *self, word_t objsize);
    word_t	 (*get_next_objsize)	(vrt_t *self, word_t objsize);
    word_t	 (*get_vrt_size)	(vrt_t *self);
    mdb_t *	 (*get_mapdb)		(vrt_t *self);
    const char * (*get_name)		(vrt_t *self);

    /* node specific */
    void	 (*set_object)		(vrt_t *self, vrt_node_t *n, word_t n_sz,
					 word_t paddr, vrt_node_t *o, word_t o_sz,
					 word_t access);
    word_t	 (*get_address)		(vrt_t *self, vrt_node_t *n);
    word_t	 (*make_misc)		(vrt_t *self, vrt_node_t *obj, mdb_node_t *map);
    void	 (*dump)		(vrt_t *self, vrt_node_t *n);
} vrt_ops_t;

struct vrt_t
{
    const vrt_ops_t *	ops;
    vrt_table_t *	root_table;
};

/* Generic operations, defined in generic/vrt.c */
BEGIN_DECLS
bool   vrt_lookup (vrt_t *self, word_t addr, word_t *value, word_t *objsize);
void   vrt_map_fpage (vrt_t *self, fpage_t f_fp, word_t base, vrt_t *t_space,
		      fpage_t t_fp, bool grant);
word_t vrt_mapctrl (vrt_t *self, fpage_t fp, mdb_ctrl_t ctrl,
		    word_t rights, word_t attrib);

/* vrt_table_t::operator new (size_t, word_t) / delete */
vrt_table_t * vrt_table_alloc (word_t radix_log2);
void	      vrt_table_free (vrt_table_t *t);
END_DECLS

INLINE void vrt_set_table (vrt_t *self, vrt_table_t *t)	{ self->root_table = t; }
INLINE vrt_table_t * vrt_get_table (vrt_t *self)	{ return self->root_table; }

INLINE word_t vrt_flush (vrt_t *self, fpage_t fp)
{ return vrt_mapctrl (self, fp, mdb_ctrl_flush (), 0, 0); }


/**
 * The vrt_node_t specfies either an object or table pointer within a
 * VRT table.
 */
struct vrt_node_t
{
    union {
	word_t raw;
	struct {
	    word_t is_table_ptr	: 1;
	    word_t value	: BITS_WORD - 1;
	};
    };
};


/**
 * The vrt_table_t is an array of objects of a given size.  The size
 * of the array is variable and is specified upon creation of the
 * table.  The entries in the table are actually vrt_node_t
 * structures.  They can be an object or a pointer to a sub-table.
 *
 * The table also contains pointers to nodes in the mapping database.
 * These pointers are not stored directly in the table entry.  They
 * are stored in a shadow table directly after the main table.  The
 * reason for doing this is to lower the cache footprint for table
 * lookups and table scans.
 *
 * The table structure also supports short-circuiting the lookup from
 * higher up in the table (i.e., path compression).
 */
struct vrt_table_t
{
    word_t radix	: 6;
    word_t objsize	: 6;
    word_t __pad	: BITS_WORD - 12;
    word_t prefix	: BITS_WORD;
    word_t entries	: BITS_WORD;
};


/*
**
**	vrt_node_t
**
*/

INLINE bool vrt_node_is_valid (vrt_node_t *self)	{ return self->raw != 0; }
INLINE bool vrt_node_is_table (vrt_node_t *self)	{ return self->is_table_ptr; }
INLINE word_t vrt_node_get_object (vrt_node_t *self)	{ return self->value; }

INLINE vrt_table_t * vrt_node_get_table (vrt_node_t *self)
/* value is a BITS_WORD-1 bit-field: the cast belongs inside the shift, or C
   evaluates it at 63-bit precision and loses the top bit (notes §136). */
{ return (vrt_table_t *) (self->is_table_ptr ? (((word_t) self->value) << 1) : 0); }

INLINE void vrt_node_clear (vrt_node_t *self)		{ self->raw = 0; }

/* Set raw contents of object. */
INLINE void vrt_node_set_object_raw (vrt_node_t *self, word_t objvalue)
{
    self->value = objvalue & MDB_BITMASK (BITS_WORD - 1);
    self->is_table_ptr = 0;
}

INLINE void vrt_node_set_table (vrt_node_t *self, vrt_table_t *t)
{
    word_t p = (word_t) t >> 1;
    self->value = p & MDB_BITMASK (BITS_WORD - 1);
    self->is_table_ptr = 1;
}

/* Copy contents into object -- dispatches to the VRT's set_object. */
INLINE void vrt_node_set_object (vrt_node_t *self, vrt_t *vrt, word_t this_size,
				 word_t paddr, vrt_node_t *obj, word_t obj_size,
				 word_t access)
{
    vrt->ops->set_object (vrt, self, this_size, paddr, obj, obj_size, access);
    self->is_table_ptr = 0;
}

INLINE word_t vrt_node_get_address (vrt_node_t *self, vrt_t *vrt)
{ return vrt->ops->get_address (vrt, self); }


/*
**
**	vrt_table_t
**
*/

INLINE word_t vrt_table_get_addr (vrt_table_t *self, word_t idx)
{ return (1UL << self->objsize) * idx; }

INLINE bool vrt_table_match_prefix (vrt_table_t *self, word_t addr)
{
    /* Need to do the shift twice instead of adding objsize and radix, or else
       gcc somehow manages to optimize away the operation altogether. */
    return ((addr ^ self->prefix) & ~(((1UL << self->objsize) << self->radix) - 1)) == 0;
}

INLINE vrt_node_t * vrt_table_get_node (vrt_table_t *self, word_t addr)
{
    return (vrt_node_t *) (word_t) self->entries +
	((addr >> self->objsize) & ((1UL << self->radix) - 1));
}

INLINE mdb_node_t * vrt_table_get_mapnode (vrt_table_t *self, word_t addr)
{
    mdb_node_t **map_ptrs = (mdb_node_t **)
	((vrt_node_t *) (word_t) self->entries + (1UL << self->radix));
    return map_ptrs[(addr >> self->objsize) & ((1UL << self->radix) - 1)];
}

INLINE vrt_table_t * vrt_table_get_table (vrt_table_t *self, word_t addr)
{ return vrt_node_get_table (vrt_table_get_node (self, addr)); }

INLINE word_t vrt_table_get_radix (vrt_table_t *self)	{ return self->radix; }
INLINE word_t vrt_table_get_prefix (vrt_table_t *self)	{ return self->prefix; }
INLINE word_t vrt_table_get_objsize (vrt_table_t *self)	{ return self->objsize; }

INLINE word_t vrt_table_get_start_addr (vrt_table_t *self)
{ return self->prefix & (~0UL << (self->objsize + self->radix)); }
INLINE word_t vrt_table_get_end_addr (vrt_table_t *self)
{ return vrt_table_get_start_addr (self) + (1UL << (self->objsize + self->radix)); }

INLINE void vrt_table_clear (vrt_table_t *self, word_t addr)
{ vrt_node_clear (vrt_table_get_node (self, addr)); }

INLINE void vrt_table_set_object (vrt_table_t *self, vrt_t *vrt, word_t addr,
				  word_t paddr, vrt_node_t *obj, word_t obj_size,
				  word_t access)
{
    if (obj == NULL)
	vrt_table_clear (self, addr);
    else
	vrt_node_set_object (vrt_table_get_node (self, addr), vrt,
			     vrt_table_get_objsize (self), paddr, obj, obj_size, access);
}

INLINE void vrt_table_set_mapnode (vrt_table_t *self, word_t addr, mdb_node_t *map)
{
    mdb_node_t **map_ptrs = (mdb_node_t **)
	((vrt_node_t *) (word_t) self->entries + (1UL << self->radix));
    map_ptrs[(addr >> self->objsize) & ((1UL << self->radix) - 1)] = map;
}

INLINE void vrt_table_set_table (vrt_table_t *self, word_t addr, vrt_table_t *t)
{
    if (t == NULL)
	vrt_table_clear (self, addr);
    else
	vrt_node_set_table (vrt_table_get_node (self, addr), t);
}

INLINE void vrt_table_set_prefix (vrt_table_t *self, word_t p)	{ self->prefix = p; }
INLINE void vrt_table_set_objsize (vrt_table_t *self, word_t s)
{ self->objsize = s & MDB_BITMASK (6); }

#endif /* !__VRT_H__ */
