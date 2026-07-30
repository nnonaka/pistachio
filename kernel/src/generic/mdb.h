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
 * The mapping-database types.  These were classes in this header until
 * f3d2a88 removed them along with the other __cplusplus blocks; that pass
 * assumed every collapsed header had a C form written beside its C++ one, and
 * this file did not -- its whole content was the C++ side.  They are restored
 * here in C (see notes §110).
 */

struct mdb_t;		typedef struct mdb_t mdb_t;
struct mdb_node_t;	typedef struct mdb_node_t mdb_node_t;
struct mdb_tableent_t;	typedef struct mdb_tableent_t mdb_tableent_t;
struct mdb_table_t;	typedef struct mdb_table_t mdb_table_t;


/*
 * mdb_ctrl_t / mdb_range_t were the nested value types mdb_t::ctrl_t and
 * mdb_t::range_t, hoisted to top level so they are usable from C.  Their
 * constructors and static factories become named functions.
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

/* was mdb_ctrl_t::flush() */
INLINE mdb_ctrl_t mdb_ctrl_flush (void)
{
    mdb_ctrl_t ctrl;
    ctrl.raw = 0;
    ctrl.mapctrl_self = ctrl.unmap = ctrl.deliver_status = 1;
    return ctrl;
}

#if defined(CONFIG_DEBUG)
/* was mdb_ctrl_t::string().  Kept verbatim, including its writes into a
   string literal -- see notes §110. */
INLINE char * mdb_ctrl_string (mdb_ctrl_t *self)
{
    static char s[7] = "~~~~~~";

    s[0] = (self->set_attribute ? 'm' : '~');
    s[1] = (self->deliver_status ? 'd' : '~');
    s[2] = (self->reset_status ? 'r' : '~');
    s[3] = (self->set_rights ? 'p' : '~');
    s[4] = (self->unmap ? 'u' : '~');
    s[5] = (self->mapctrl_self ? 'c' : '~');

    return s;
}
#endif

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

/* was mdb_range_t::mdb_range_t (addr_t, word_t) */
INLINE mdb_range_t mdb_range_make (addr_t b, word_t s)
{
    mdb_range_t r;
    if (s >= 64)
	r.raw = ~0UL;
    else
    {
	r.raw = 0;
	r.c = 0;
	r.idx = ((word_t) b >> s) & MDB_BITMASK (BITS_WORD - 7);
	r.size = s & MDB_BITMASK (6);
    }
    return r;
}

/* was mdb_range_t::full() */
INLINE mdb_range_t mdb_range_full (void)
{
    mdb_range_t r; r.raw = ~0UL; return r;
}

INLINE bool   mdb_range_in_range (mdb_range_t *self, word_t addr)
{ return self->raw == ~0UL || (addr >> self->size) == self->idx; }
INLINE word_t mdb_range_get_size (mdb_range_t *self)	{ return self->size; }
INLINE word_t mdb_range_get_low (mdb_range_t *self)
{ return self->raw == ~0UL ? 0 : self->idx << self->size; }
INLINE word_t mdb_range_get_high (mdb_range_t *self)
{ return self->raw == ~0UL ? ~0UL : ((self->idx + 1) << self->size) - 1; }


/**
 * The mdb_t specifies a particular mapping database, e.g., for page frames,
 * I/O ports, etc.  Certain operations (map, mapctrl) are generic; the rest --
 * flushing cached entries, access rights, status bits -- are per-database and
 * were virtual.  In C they are an ops table, and mdb_t holds nothing but the
 * pointer to it, exactly as the C++ object held nothing but its vptr.
 */
typedef struct mdb_ops_t {
    word_t	 (*get_radix)		(mdb_t *self, word_t objsize);
    word_t	 (*get_next_objsize)	(mdb_t *self, word_t objsize);
    const char * (*get_name)		(mdb_t *self);

    /* MDB specific node operations */
    void	 (*clear)		(mdb_t *self, mdb_node_t *node);
    word_t	 (*get_rights)		(mdb_t *self, mdb_node_t *node);
    void	 (*set_rights)		(mdb_t *self, mdb_node_t *node, word_t r);
    void	 (*flush_cached_entry)	(mdb_t *self, mdb_node_t *node, mdb_range_t range);
    bool	 (*allow_attribute_update) (mdb_t *self, mdb_node_t *node);
    void	 (*set_attribute)	(mdb_t *self, mdb_node_t *node, word_t attrib);
    word_t	 (*get_phys_address)	(mdb_t *self, mdb_node_t *node);
    word_t	 (*get_purged_status)	(mdb_t *self, mdb_node_t *node);
    void	 (*reset_purged_status)	(mdb_t *self, mdb_node_t *node);
    void	 (*update_purged_status) (mdb_t *self, mdb_node_t *node, word_t status);
    word_t	 (*get_effective_status) (mdb_t *self, mdb_node_t *node);
    word_t	 (*reset_effective_status) (mdb_t *self, mdb_node_t *node);
    void	 (*update_effective_status) (mdb_t *self, mdb_node_t *node, word_t status);
    void	 (*dump)		(mdb_t *self, mdb_node_t *node);
} mdb_ops_t;

struct mdb_t {
    const mdb_ops_t *	ops;
};

/*
 * Maptree operations -- generic, defined in generic/mdb.c.  These were
 * mdb_t::map / mapctrl / flush / delete_node.  They carry an mdb_tree_ prefix
 * rather than the plain mdb_ one because generic/mapping.c -- the *old*
 * mapping database, which the CONFIG_NEW_MDB=n configs build -- already
 * exports mdb_map() and mdb_flush() with different signatures, and both
 * headers are in scope together.
 */
BEGIN_DECLS
/* NB: parameter order is (out_rights, in_rights).  The C++ declaration said
   (in_rights, out_rights) while its definition said the opposite; names do not
   affect C++ overload resolution, so the mismatch was silent and the
   definition's order is what ran.  Preserved -- see notes §111. */
mdb_node_t * mdb_tree_map (mdb_t *self, mdb_node_t *f_node, void *obj, word_t objsize,
			   word_t addr, word_t out_rights, word_t in_rights);
word_t mdb_tree_mapctrl (mdb_t *self, mdb_node_t *node, mdb_range_t range,
			 mdb_ctrl_t ctrl, word_t rights, word_t attrib);
word_t mdb_tree_flush (mdb_t *self, mdb_node_t *node);
void   mdb_tree_delete_node (mdb_t *self, mdb_node_t *node);

/* mdb_node_t::operator new / delete and its constructor */
mdb_node_t * mdb_node_alloc (void);
void	     mdb_node_free (mdb_node_t *n);
void	     mdb_node_init (mdb_node_t *n);

/* mdb_table_t::operator new (size_t, word_t) / delete */
mdb_table_t * mdb_table_alloc (word_t radix_log2);
void	      mdb_table_free (mdb_table_t *t);
END_DECLS


/**
 * The mdb_node_t is the main structure for keeping track of recursive
 * mappings: a doubly linked list of all mapping nodes within a mapping tree,
 * where every node in the tree refers to objects of the same size.  Partial
 * mappings hang off an mdb_table_t.  Nodes are listed depth first, with a
 * depth field, so the subtree of a node is the run of following nodes with a
 * greater depth -- which lets the algorithms be non-recursive.
 */
struct mdb_node_t {
    word_t in_rights	: 4;
    word_t out_rights	: 4;
    word_t obj_size	: 6;
    word_t depth	: BITS_WORD - 14;
    word_t prev		: BITS_WORD;
    word_t next_is_table : 1;
    word_t next		: BITS_WORD - 1;
    word_t object_ptr	: BITS_WORD;
    word_t misc		: BITS_WORD;
};

/**
 * mdb_tableent_t is an entry in a mapping database table: a pointer to a
 * mapping node, another table, or both.  When it holds a table, that table
 * carries the pointer to any mapping node.
 */
struct mdb_tableent_t {
    word_t ptr_is_table	: 1;
    /*
     * A node or table pointer shifted right by one -- both are at least
     * 2-byte aligned, which buys the bit ptr_is_table uses.
     *
     * Reconstructing it must shift a value that has already been converted to
     * word_t: `(word_t) (self->ptr << 1)' is not the same expression in C as it
     * was in C++.  GCC's C front end gives a bit-field wider than int the
     * bit-field's own precision, so on a 64-bit word `self->ptr << 1' is
     * evaluated modulo 2^63 and loses the top bit -- which every kernel
     * pointer has set.  See notes §136.
     */
    word_t ptr		: BITS_WORD - 1;
};

/**
 * The mdb_table_t is an array of objects of a given size, variable in length
 * and specified on creation.  Entries may be nodes, sub-tables, or both.  The
 * count field tracks populated entries so the table can be freed once empty.
 * A prefix supports short-circuiting the lookup (path compression); callers
 * must check it before using the entry accessors.
 */
struct mdb_table_t {
    word_t node		: BITS_WORD;
    word_t radix	: 6;
    word_t objsize	: 6;
    word_t count	: BITS_WORD - 12;
    word_t prefix	: BITS_WORD;
    word_t entries	: BITS_WORD;
};


/*
 * The accessors below are mutually recursive across the three types (a node's
 * `next' may be a table, whose own node is the real next).  The two that only
 * touch mdb_table_t::node come first so the rest can use them.
 */
INLINE mdb_node_t * mdb_table_get_node (mdb_table_t *self)
{ return (mdb_node_t *) self->node; }
INLINE void mdb_table_set_node (mdb_table_t *self, mdb_node_t *n)
{ self->node = (word_t) n; }

INLINE word_t mdb_table_get_addr (mdb_table_t *self, word_t idx)
{ return (1UL << self->objsize) * idx; }
INLINE word_t mdb_table_get_mask (mdb_table_t *self)
{ return ((1UL << self->objsize) << self->radix) - 1; }
INLINE word_t mdb_table_get_radix (mdb_table_t *self)	{ return self->radix; }
INLINE word_t mdb_table_get_count (mdb_table_t *self)	{ return self->count; }
INLINE word_t mdb_table_get_prefix (mdb_table_t *self)	{ return self->prefix; }
INLINE word_t mdb_table_get_objsize (mdb_table_t *self)	{ return self->objsize; }
INLINE bool mdb_table_match_prefix (mdb_table_t *self, word_t addr)
{ return ((addr ^ self->prefix) & ~mdb_table_get_mask (self)) == 0; }
INLINE void mdb_table_set_prefix (mdb_table_t *self, word_t p)	{ self->prefix = p; }
INLINE void mdb_table_set_objsize (mdb_table_t *self, word_t s)
{ self->objsize = s & MDB_BITMASK (6); }

INLINE mdb_tableent_t * mdb_table_get_entry (mdb_table_t *self, word_t addr)
{
    return (mdb_tableent_t *) self->entries +
	((addr >> self->objsize) & ((1UL << self->radix) - 1));
}


/*  mdb_tableent_t  */

INLINE bool mdb_tableent_is_valid (mdb_tableent_t *self)  { return self->ptr != 0; }
INLINE bool mdb_tableent_is_table (mdb_tableent_t *self)  { return self->ptr_is_table != 0; }

INLINE mdb_table_t * mdb_tableent_get_table (mdb_tableent_t *self)
{
    if (! self->ptr_is_table)
	return NULL;
    return (mdb_table_t *) (((word_t) self->ptr) << 1);
}

INLINE mdb_node_t * mdb_tableent_get_node (mdb_tableent_t *self)
{
    if (self->ptr_is_table)
	return mdb_table_get_node (mdb_tableent_get_table (self));
    return (mdb_node_t *) (((word_t) self->ptr) << 1);
}

INLINE void mdb_tableent_set_table (mdb_tableent_t *self, mdb_table_t *t)
{
    word_t p = ((word_t) t) >> 1;
    mdb_table_set_node (t, mdb_tableent_get_node (self));
    if (self->ptr_is_table)
	mdb_table_set_node (mdb_tableent_get_table (self), NULL);
    self->ptr = p & MDB_BITMASK (BITS_WORD - 1);
    self->ptr_is_table = 1;
}

INLINE void mdb_tableent_set_node (mdb_tableent_t *self, mdb_node_t *n)
{
    if (self->ptr_is_table)
	mdb_table_set_node (mdb_tableent_get_table (self), n);
    else
    {
	word_t p = ((word_t) n) >> 1;
	self->ptr = p & MDB_BITMASK (BITS_WORD - 1);
    }
}

INLINE void mdb_tableent_clear (mdb_tableent_t *self)
{ self->ptr = self->ptr_is_table = 0; }


/*  mdb_table_t, the part that needs mdb_tableent_t  */

INLINE mdb_node_t * mdb_table_get_node_at (mdb_table_t *self, word_t addr)
{ return mdb_tableent_get_node (mdb_table_get_entry (self, addr)); }
INLINE mdb_table_t * mdb_table_get_table (mdb_table_t *self, word_t addr)
{ return mdb_tableent_get_table (mdb_table_get_entry (self, addr)); }

INLINE void mdb_table_remove_node (mdb_table_t *self, word_t addr)
{
    mdb_tableent_t *e = mdb_table_get_entry (self, addr);
    if (mdb_tableent_is_valid (e) && ! mdb_tableent_is_table (e))
	self->count--;
    mdb_tableent_set_node (e, NULL);
}

INLINE void mdb_table_remove_table (mdb_table_t *self, word_t addr)
{
    mdb_tableent_t *e = mdb_table_get_entry (self, addr);
    mdb_node_t *n;
    word_t p;

    if (! mdb_tableent_is_table (e))
	return;
    n = mdb_tableent_get_node (e);
    p = ((word_t) n) >> 1;
    if (n == NULL)
	self->count--;
    e->ptr_is_table = 0;
    e->ptr = p & MDB_BITMASK (BITS_WORD - 1);
}

INLINE void mdb_table_set_node_at (mdb_table_t *self, word_t addr, mdb_node_t *n)
{
    if (n == NULL)
	mdb_table_remove_node (self, addr);
    else
    {
	mdb_tableent_t *e = mdb_table_get_entry (self, addr);
	if (! mdb_tableent_is_valid (e))
	    self->count++;
	mdb_tableent_set_node (e, n);
    }
}

INLINE void mdb_table_set_table (mdb_table_t *self, word_t addr, mdb_table_t *t)
{
    if (t == NULL)
	mdb_table_remove_table (self, addr);
    else
    {
	mdb_tableent_t *e = mdb_table_get_entry (self, addr);
	if (! mdb_tableent_is_valid (e))
	    self->count++;
	mdb_tableent_set_table (e, t);
    }
}


/*  mdb_node_t  */

INLINE mdb_node_t * mdb_node_get_prev (mdb_node_t *self)
{ return (mdb_node_t *) self->prev; }

INLINE mdb_table_t * mdb_node_get_table (mdb_node_t *self)
{
    if (! self->next_is_table)
	return NULL;
    return (mdb_table_t *) (((word_t) self->next) << 1);
}

INLINE mdb_node_t * mdb_node_get_next (mdb_node_t *self)
{
    if (self->next_is_table)
	return mdb_table_get_node (mdb_node_get_table (self));
    return (mdb_node_t *) (((word_t) self->next) << 1);
}

INLINE word_t mdb_node_get_depth (mdb_node_t *self)	{ return self->depth; }
INLINE void * mdb_node_get_object (mdb_node_t *self)	{ return (void *) self->object_ptr; }
INLINE word_t mdb_node_get_objsize (mdb_node_t *self)	{ return self->obj_size; }
INLINE word_t mdb_node_get_misc (mdb_node_t *self)	{ return self->misc; }
INLINE word_t mdb_node_get_inrights (mdb_node_t *self)	{ return self->in_rights; }
INLINE word_t mdb_node_get_outrights (mdb_node_t *self)	{ return self->out_rights; }

INLINE bool mdb_node_is_my_parent (mdb_node_t *self, mdb_node_t *n)
{ return mdb_node_get_depth (self) == mdb_node_get_depth (n) + 1; }

INLINE void mdb_node_set_prev (mdb_node_t *self, mdb_node_t *p)
{ self->prev = (word_t) p; }

INLINE void mdb_node_set_next (mdb_node_t *self, mdb_node_t *n)
{
    if (self->next_is_table)
	mdb_table_set_node (mdb_node_get_table (self), n);
    else
    {
	word_t p = ((word_t) n) >> 1;
	self->next = p & MDB_BITMASK (BITS_WORD - 1);
    }
}

INLINE void mdb_node_set_table (mdb_node_t *self, mdb_table_t *t)
{
    word_t p = ((word_t) t) >> 1;
    mdb_table_set_node (t, mdb_node_get_next (self));
    if (self->next_is_table)
	mdb_table_set_node (mdb_node_get_table (self), NULL);
    self->next = p & MDB_BITMASK (BITS_WORD - 1);
    self->next_is_table = 1;
}

INLINE void mdb_node_remove_table (mdb_node_t *self)
{
    word_t p = ((word_t) mdb_table_get_node (mdb_node_get_table (self))) >> 1;
    self->next = p & MDB_BITMASK (BITS_WORD - 1);
    self->next_is_table = 0;
}

INLINE void mdb_node_set_depth (mdb_node_t *self, word_t d)
{ self->depth = d & MDB_BITMASK (BITS_WORD - 14); }
INLINE void mdb_node_set_object (mdb_node_t *self, void *o)
{ self->object_ptr = (word_t) o; }
INLINE void mdb_node_set_objsize (mdb_node_t *self, word_t s)
{ self->obj_size = s & MDB_BITMASK (6); }
INLINE void mdb_node_set_misc (mdb_node_t *self, word_t m)	{ self->misc = m; }
INLINE void mdb_node_set_inrights (mdb_node_t *self, word_t r)
{ self->in_rights = r & MDB_BITMASK (4); }
INLINE void mdb_node_set_outrights (mdb_node_t *self, word_t r)
{ self->out_rights = r & MDB_BITMASK (4); }

/* mdb_node_t::get_parent is out of line in generic/mdb.c */
BEGIN_DECLS
mdb_node_t * mdb_node_get_parent (mdb_node_t *self);
END_DECLS

/*
 * Per-database node operations.  These were non-virtual members of
 * mdb_node_t that forwarded to the virtual mdb_t ones, deliberately: a vtable
 * per mdb_node_t would cost a word on every mapping.  The C form keeps that
 * property -- the node carries no ops pointer, the database does.
 */
INLINE void mdb_node_clear (mdb_node_t *self, mdb_t *mdb)
{ mdb->ops->clear (mdb, self); }
INLINE word_t mdb_node_get_rights (mdb_node_t *self, mdb_t *mdb)
{ return mdb->ops->get_rights (mdb, self); }
INLINE void mdb_node_set_rights (mdb_node_t *self, mdb_t *mdb, word_t r)
{ mdb->ops->set_rights (mdb, self, r); }
INLINE void mdb_node_flush_cached_entry (mdb_node_t *self, mdb_t *mdb, mdb_range_t range)
{ mdb->ops->flush_cached_entry (mdb, self, range); }
INLINE bool mdb_node_allow_attribute_update (mdb_node_t *self, mdb_t *mdb)
{ return mdb->ops->allow_attribute_update (mdb, self); }
INLINE void mdb_node_set_attribute (mdb_node_t *self, mdb_t *mdb, word_t attrib)
{ mdb->ops->set_attribute (mdb, self, attrib); }
INLINE word_t mdb_node_get_phys_address (mdb_node_t *self, mdb_t *mdb)
{ return mdb->ops->get_phys_address (mdb, self); }
INLINE word_t mdb_node_get_purged_status (mdb_node_t *self, mdb_t *mdb)
{ return mdb->ops->get_purged_status (mdb, self); }
INLINE void mdb_node_reset_purged_status (mdb_node_t *self, mdb_t *mdb)
{ mdb->ops->reset_purged_status (mdb, self); }
INLINE void mdb_node_update_purged_status (mdb_node_t *self, mdb_t *mdb, word_t status)
{ mdb->ops->update_purged_status (mdb, self, status); }
INLINE word_t mdb_node_get_effective_status (mdb_node_t *self, mdb_t *mdb)
{ return mdb->ops->get_effective_status (mdb, self); }
INLINE word_t mdb_node_reset_effective_status (mdb_node_t *self, mdb_t *mdb)
{ return mdb->ops->reset_effective_status (mdb, self); }
INLINE void mdb_node_update_effective_status (mdb_node_t *self, mdb_t *mdb, word_t status)
{ mdb->ops->update_effective_status (mdb, self, status); }


/* From generic/mapping_alloc.c */
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
