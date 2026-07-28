/*********************************************************************
 *                
 * Copyright (C) 2004-2007,  Karlsruhe University
 *                
 * File path:     generic/mdb.c
 * Description:   Generic mapping database
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
 * $Id: mdb.cc,v 1.16 2007/01/08 14:08:10 skoglund Exp $
 *                
 ********************************************************************/
#include <mdb.h>
#include <sync.h>
#include INC_GLUE(mdb.h)

#include <kdb/tracepoints.h>
#include <debug.h>

#if !defined(MAX_MDB_RECURSION)
#define MAX_MDB_RECURSION 10
#endif


DECLARE_TRACEPOINT (MDB_MAP);
DECLARE_TRACEPOINT (MDB_MAPCTRL);

DEFINE_SPINLOCK (mdb_lock);

/* dispatch shorthands for the mdb_t ops table */
#define MDB_NAME(m)		((m)->ops->get_name (m))
#define MDB_RADIX(m, sz)	((m)->ops->get_radix ((m), (sz)))
#define MDB_NEXT_OBJSIZE(m, sz)	((m)->ops->get_next_objsize ((m), (sz)))


/**
 * Initialize mapping databases.  Invoke the init function for each
 * mapping database type.
 */
void SECTION (".init") init_mdb (void)
{
    extern mdb_init_func_t _start_mdb_funcs[];
    extern mdb_init_func_t _end_mdb_funcs[];
    mdb_init_func_t *fdesc;

    for (fdesc = _start_mdb_funcs; fdesc < _end_mdb_funcs; fdesc++)
	(*fdesc->function) ();
}


/**
 * Allocate a new mapping node.  Was mdb_node_t::operator new.
 */
mdb_node_t * mdb_node_alloc (void)
{
    return (mdb_node_t *) mdb_alloc_buffer (sizeof (mdb_node_t));
}

/**
 * Was the mdb_node_t constructor.
 */
void mdb_node_init (mdb_node_t *n)
{
    n->next = n->next_is_table = 0;
}

void mdb_node_free (mdb_node_t *n)
{
    mdb_free_buffer (n, sizeof (mdb_node_t));
}


/**
 * Remove mapping node.  Any tables that falls empty as a result of
 * the delete operation are not deleted.  They have to be deleted
 * explicitly.  Further, the operation assumes that all mapping nodes
 * below the current one have already been deleted.  We can't use the
 * delete operator instead of this function since we need to know
 * which mapping database the node resides in (i.e., we need to know
 * how to retrieve the physical address the node refers to).
 *
 * @param node		node to remove
 */
void mdb_tree_delete_node (mdb_t *self, mdb_node_t *node)
{
    mdb_node_t *p;

    if (node == NULL)
	return;

    p = mdb_node_get_prev (node);

    ASSERT (mdb_node_get_next (node) == NULL ||
	    mdb_node_get_depth (mdb_node_get_next (node)) <= mdb_node_get_depth (node));

    if (mdb_node_get_next (p) == node)
    {
	/* We can simply remove entry directly from linked list. */

	mdb_node_set_next (p, mdb_node_get_next (node));
    }
    else
    {
	word_t addr = mdb_node_get_phys_address (node, self);
	mdb_table_t *table = mdb_node_get_table (p);
	ASSERT (table);

	/* Find table containing pointer to mapping node and remove
	   corresponding table entry. */

	while (mdb_table_get_objsize (table) > mdb_node_get_objsize (node))
	{
	    table = mdb_table_get_table (table, addr);
	    ASSERT (table);
	}

	ASSERT (mdb_table_get_node_at (table, addr) == node);
	mdb_table_set_node_at (table, addr, mdb_node_get_next (node));
    }

    if (mdb_node_get_next (node))
	mdb_node_set_prev (mdb_node_get_next (node), p);

    mdb_node_clear (node, self);
    mdb_free_buffer (node, sizeof (mdb_node_t));
}


/**
 * Allocate a new mapping table.  Allocates both the table control
 * structure and the table entries.  Further, the table control
 * structure is partly initialized.  Was mdb_table_t::operator new.
 *
 * @param radix_log2	size of table
 *
 * @return new mapping table
 */
mdb_table_t * mdb_table_alloc (word_t radix_log2)
{
    mdb_table_t *t = (mdb_table_t *) mdb_alloc_buffer (sizeof (mdb_table_t));
    mdb_tableent_t *e;
    word_t k;

    t->node = 0;
    t->radix = radix_log2 & MDB_BITMASK (6);
    t->count = 0;
    t->entries = (word_t) mdb_alloc_buffer (sizeof (mdb_tableent_t) *
					    (1UL << radix_log2));

    e = mdb_table_get_entry (t, 0);
    for (k = 0; k < (1UL << radix_log2); k++, e++)
	mdb_tableent_clear (e);

    return t;
}


/**
 * Remove table.  The operation assumes that the table is empty.
 * Was mdb_table_t::operator delete.
 *
 * @param t		table to remove
 */
void mdb_table_free (mdb_table_t *t)
{
    if (t == NULL)
	return;

    ASSERT (mdb_table_get_count (t) == 0);

    mdb_free_buffer ((void *) t->entries,
		     sizeof (mdb_tableent_t) * (1UL << mdb_table_get_radix (t)));
    mdb_free_buffer (t, sizeof (mdb_table_t));
}


/**
 * Flush a whole mapping tree.  Was the INLINE mdb_t::flush.
 */
word_t mdb_tree_flush (mdb_t *self, mdb_node_t *node)
{
    return mdb_tree_mapctrl (self, node, mdb_range_full (), mdb_ctrl_flush (), 0, 0);
}


/**
 * Create new mapping node entry in the mapping database.
 *
 * @param f_node	mapping node to map from
 * @param obj		object to map
 * @param objsize	size of object (log2)
 * @param addr		physical address of object
 * @param out_rights	outbound access rights of mapping
 * @param in_rights	inbound access rights of mapping
 *
 * @return pointer to new mapping node
 *
 * Create new mapping node entry for specified object and hook it into
 * the mapping databse with F_NODE as the parent.  New mapping tables
 * will be created when necessary.
 *
 * NB: the C++ declaration in mdb.h named the last two parameters
 * (in_rights, out_rights) while the definition here named them
 * (out_rights, in_rights).  Names do not participate in C++ overload
 * resolution so the mismatch was silent, and what actually ran is this
 * order -- the fifth argument becomes the *outbound* rights.  Preserved
 * verbatim; see notes §111.
 */
mdb_node_t * mdb_tree_map (mdb_t *self, mdb_node_t *f_node,
			   void *obj, word_t objsize, word_t addr,
			   word_t out_rights, word_t in_rights)
{
    mdb_node_t *newnode;
    mdb_table_t *table, *prev_table;

    TRACEPOINT (MDB_MAP, "%s::map (%p, %p, %d, %p, %x, %x)",
		MDB_NAME (self), f_node, obj, objsize, addr, (out_rights & 7UL),
		(in_rights & 7UL));

    spinlock_lock (&mdb_lock);

    /* Allocate and initialize new mapping node. */

    newnode = mdb_node_alloc ();
    ASSERT (newnode);
    mdb_node_init (newnode);
    mdb_node_set_depth (newnode, mdb_node_get_depth (f_node) + 1);
    mdb_node_set_objsize (newnode, objsize);
    mdb_node_set_object (newnode, obj);
    mdb_node_set_prev (newnode, f_node);
    mdb_node_set_inrights (newnode, in_rights);
    mdb_node_set_outrights (newnode, out_rights);
    mdb_node_set_rights (newnode, self,
			 mdb_node_get_rights (f_node, self) & out_rights & in_rights);
    mdb_node_reset_purged_status (newnode, self);

    if (objsize == mdb_node_get_objsize (f_node))
    {
	/* New node is of the same object size as the current node.
	   We can simply insert the node directly below current node. */

	mdb_node_set_next (newnode, mdb_node_get_next (f_node));
	if (mdb_node_get_next (f_node))
	    mdb_node_set_prev (mdb_node_get_next (f_node), newnode);
	mdb_node_set_next (f_node, newnode);
	spinlock_unlock (&mdb_lock);
	return newnode;
    }

    /* We can not simply put new mapping node below current node because the
       new node has a smaller object size.  We either need to create a
       sub-table for holding the mapping, or use an existing table. */

    table = mdb_node_get_table (f_node);
    prev_table = NULL;

    for (;;)
    {
	mdb_table_t *newtable;
	mdb_node_t *auxnode;

	if (table != NULL && mdb_table_match_prefix (table, addr))
	{
	    if (mdb_table_get_objsize (table) == objsize)
	    {
		/* New mapping belongs within the current mapping table.  We
		   can simply insert the new mapping node directly into the
		   subtree below the appropriate mapping table entry. */

		mdb_node_t *n = mdb_table_get_node_at (table, addr);
		mdb_node_set_next (newnode, n);
		if (n)
		    mdb_node_set_prev (n, newnode);
		mdb_table_set_node_at (table, addr, newnode);
		spinlock_unlock (&mdb_lock);
		return newnode;
	    }
	    else if (mdb_table_get_objsize (table) > objsize)
	    {
		/* Encountered a mapping table which should act as a parent
		   table for a table containing the new object. */

		prev_table = table;
		table = mdb_table_get_table (table, addr);
		if (table)
		    /* Recurse into sub-table. */
		    continue;
	    }
	    else if (mdb_table_get_objsize (table) < objsize)
	    {
		/* The new table should contain the current mapping table
		   within one of the table entries.  This will be taken care
		   of below (table != NULL). */
	    }

	    /* Create a new mapping table just below mapping node or mapping
	       table.  We perform path compression to avoid unnecessary lookup
	       and memory overhead.  The radix for the new table is based on
	       the object size of the entries within the table. */

	    newtable = mdb_table_alloc (MDB_RADIX (self, objsize));

	    mdb_table_set_prefix (newtable, addr);
	    mdb_table_set_objsize (newtable, objsize);
	    mdb_table_set_node_at (newtable, addr, newnode);
	    mdb_node_set_next (newnode, NULL);

	    if (prev_table)
		mdb_table_set_table (prev_table, addr, newtable);
	    else
		mdb_node_set_table (f_node, newtable);

	    if (table != NULL)
	    {
		/* There already existed a table which did path compression
		   and containing objects of smaller sizes than the current
		   one.  Make sure that this table is inserted into the newly
		   created table.  Do this insertion after inserting newtable
		   above so as to avoid overwriting the auxiliary mapnode
		   pointer in table. */

		mdb_table_set_table (newtable, mdb_table_get_prefix (table), table);
	    }

	    spinlock_unlock (&mdb_lock);
	    return newnode;
	}

	/* The address prefix for the looked up table (if present) does not
	   match that of the current object.  This indicates that there is no
	   overlap whatsoever, or that we must create a new table which
	   contains the old table in one of its entries.  If there was no
	   table below mapping node we simply create one. */

	newtable = mdb_table_alloc (MDB_RADIX (self, objsize));
	auxnode = mdb_node_get_next (f_node);

	mdb_table_set_prefix (newtable, addr);
	mdb_table_set_objsize (newtable, objsize);
	mdb_table_set_node_at (newtable, addr, newnode);
	mdb_node_set_next (newnode, NULL);

	if (table != NULL)
	{
	    auxnode = mdb_table_get_node (table);
	    mdb_table_set_node (table, NULL);

	    /* Remove old table now to avoid propagating auxiliary mapping
	       node upwards during the set_table operations below. */

	    if (prev_table)
		mdb_table_remove_table (prev_table, mdb_table_get_prefix (table));
	    else
		mdb_node_remove_table (f_node);

	    if (mdb_table_match_prefix (newtable, mdb_table_get_prefix (table)))
	    {
		/* There is an overlap between an existing table and the newly
		   created table.  The overlap occured because the old table
		   did path compression, and should be treated as a sub-table
		   for the newly created table. */

		ASSERT (mdb_table_get_objsize (table) < objsize);
		mdb_table_set_table (newtable, mdb_table_get_prefix (table), table);
	    }
	    else
	    {
		/* We have two sub-tables that do not overlap, but that
		   resides under the same parent table entry or mapping node.
		   We need to create an intermediate table that can help us
		   hold both tables.

		   The intermediate table will hold objects of sizes which is
		   the maximum valid object size that does not generate a
		   mapping table where the two subtables map to the same table
		   entry. */

		word_t imask;
		word_t isize = prev_table ?
		    mdb_table_get_objsize (prev_table) : mdb_node_get_objsize (f_node);
		mdb_table_t *itable;

		do {
		    isize = MDB_NEXT_OBJSIZE (self, isize);
		    imask = MDB_RADIX (self, isize) + isize;
		    imask = ((imask == sizeof (word_t) * 8) ? ~0UL :
			     ((1UL << imask) - 1)) & ~((1UL << isize) - 1);
		} while ((addr & imask) == (mdb_table_get_prefix (table) & imask));

		ASSERT (isize > objsize);
		ASSERT (isize > mdb_table_get_objsize (table));

		itable = mdb_table_alloc (MDB_RADIX (self, isize));

		mdb_table_set_prefix (itable, addr);
		mdb_table_set_objsize (itable, isize);

		mdb_table_set_table (itable, addr, newtable);
		mdb_table_set_table (itable, mdb_table_get_prefix (table), table);

		/* It's ok to just operate on the intermediate table from here
		   on. */

		newtable = itable;
	    }
	}

	if (prev_table)
	    mdb_table_set_table (prev_table, addr, newtable);
	else
	    mdb_node_set_table (f_node, newtable);

	mdb_table_set_node (newtable, auxnode);
	spinlock_unlock (&mdb_lock);
	return newnode;
    }
}


/* Was the anonymous struct in mapctrl's r_values[] array. */
typedef struct mdb_recurse_t {
    word_t idx : BITS_WORD - 1;
    word_t mod : 1;
} mdb_recurse_t;


/**
 * Modify or delete mapping tree.
 *
 * @param node		mapping node to start operation on
 * @param range		range in which mapctrl is performed
 * @param ctrl		type of operation
 * @param rights	new rights for mapping tree
 * @param attrib	new attribute for mapping tree
 *
 * @return old status rights
 *
 * Modify or delete mapping tree rooted at NODE.  Deletion occurs if
 * the unmap field in CTRL is set.  If the set_rights field is set,
 * the in-rights or out-rights of the mapping node is updated, and the
 * new rights (specified in RIGHTS) are propagater throughout the
 * tree.  The reset_status and deliver_status resets and/or delivers
 * the status bits in the mapping tree.  The set_attribute field
 * indicates that the attributes for the mapping tree should be
 * updated according to ATTRIB.  The RANGE parameter can be used to
 * limit the operation to only parts of the affected area (only
 * applicable if mapctrl_self is not specified).
 */
word_t mdb_tree_mapctrl (mdb_t *self, mdb_node_t *node, mdb_range_t range,
			 mdb_ctrl_t ctrl, word_t rights, word_t attrib)
{
    /* All locals are declared up front: the goto below jumps into the main
       do-loop, past any declaration placed inside it. */
    bool revoke_rights = false;
    bool extend_rights = false;
    word_t revoke_mask = 0;
    mdb_node_t *startnode = node;
    mdb_node_t *parent = node;
    mdb_node_t *prev = NULL;
    word_t status_bits = 0;
    mdb_table_t *r_table[MAX_MDB_RECURSION];
    mdb_recurse_t r_values[MAX_MDB_RECURSION];
    word_t recurse_level = 0;
    mdb_table_t *table = NULL;
    word_t tableidx = 0;
    bool do_modify = false;

    TRACEPOINT (MDB_MAPCTRL,
		"%s::mapctrl (%p, <%x,%x>, %x [%s], %p) phys=%p\n",
		(word_t) MDB_NAME (self), (word_t) node,
		mdb_range_get_low (&range), mdb_range_get_high (&range),
		ctrl.raw, (word_t) mdb_ctrl_string (&ctrl),
		rights & 0x7, mdb_node_get_phys_address (node, self));

    spinlock_lock (&mdb_lock);

    /* If we are unmapping the whole node there is no need to perform any
       updates on it first.  We do need to read out the status bits, though,
       or else the contents of these bits will be lost to nodes higher up in
       the mapping tree. */

    if (ctrl.unmap)
    {
	ctrl.set_rights = false;
	ctrl.set_attribute = false;
	ctrl.reset_status = false;
	ctrl.deliver_status = true;
    }

    /* If we reset the status bits we also need to read out the current bits
       so that nodes higher up in the tree can read them out later. */

    else if (ctrl.reset_status)
	ctrl.deliver_status = true;

    if (ctrl.set_attribute && ! mdb_node_allow_attribute_update (node, self))
	ctrl.set_attribute = false;

    /* When updating permissions we seperately keep track of whether we revoke
       rights and/or extend rights.  This is because of implications on
       correctness and algorithm efficiency.
       
       If we revoke rights we must also make sure that any cached effective
       access rights (e.g., in the TLB) are flushed immediately.  Such flushing
       is not required for rights extension.  When extending the access rights
       updates to caches can happen lazily.
       
       If we are only revoking access rights it is sufficient to apply a fixed
       bitmask to the effective access rights within a whole subtree.  When
       extending the access rights, however, such a scheme is not possible.
       Instead, the effective access rights of the parent must be looked up to
       determine the effective access rigths of the current node.  Since we
       avoid recursive algorithms, the lookup requires searching backwards in
       the linked list of mapping nodes until parent is found.  This can be a
       relatively expensive operation, and is as such avoided if possible. */

    if (ctrl.set_rights && ctrl.mapctrl_self)
    {
	if (ctrl.mapctrl_self)
	{
	    /* Changing the inbound rights for the mapping. */

	    word_t old_rights = mdb_node_get_inrights (node);
	    if (old_rights == rights)
	    {
		/* Permissions not changed.  No need to perform any updates
		   throughout the subtree. */
		ctrl.set_rights = false;
	    }
	    else
	    {
		mdb_node_set_inrights (node, rights);
		revoke_rights = (old_rights & rights) != old_rights;
		extend_rights = (old_rights | rights) != old_rights;
		revoke_mask = ~((old_rights & rights) ^ old_rights);
	    }
	}
	else
	{
	    /* Changing outbound rights for the mapping.  We need to change
	       the rights in every mapping node which is a child of the
	       current one (i.e., with a depth count one more than the start
	       depth). */
	}
    }

    if (! ctrl.mapctrl_self)
	goto Skip_node_modifications;

    if (ctrl.deliver_status)
	status_bits |= mdb_node_get_purged_status (node, self);

    /* We never do partial mapctrl if operating on self. */

    range = mdb_range_full ();

    /* Tha map control algorithm works as follows.
       
       A main loop parses through every mapping node in the subtree.  The
       termination criteria is when we either reach the end of the linked list
       representing the subtree, or that the current node in the linked list
       has a depth counter that indicates that it is a sibling or is higher up
       in the mapping hiearchy than the start node.  After the main loop
       terminates, the alorithm potentially backs up towards the start node
       again to remove mappings or reset reference bits.
       
       The main loop can be divided into three major parts:
       
         1. Process the current mapping node (setting attributes, setting
            permissions, or delivering status information).
         2. Go to next mapping node.
         3. Select new mapping node to process in case we've reached the end of
            a subtree.  Step 3 can again be divided into 4 steps.
              a) Perform unmap: Parse linked list backwards to remove mapping
                 nodes.  Remove any mapping tables that fall empty.
              b) Perform status reset: Parse linked list backwards to update
                 status bits.
              c) Recurse down into subtable, skip to next table entry in
                 current table, or parse up from previous subtable.
              d) Handle the case where we still have not found a new mapping
                 node to process.
       
       The different steps are marked as "--- PART X ---" in the code below. */

    do {
	/* --- PART 1 --- */

	do_modify = true;

	if (ctrl.deliver_status)
	    status_bits |= mdb_node_get_effective_status (node, self);

	if (ctrl.set_attribute)
	{
	    mdb_node_set_attribute (node, self, attrib);
	    mdb_node_flush_cached_entry (node, self, range);
	}

	if (ctrl.set_rights)
	{
	    if ((! ctrl.mapctrl_self) && mdb_node_is_my_parent (node, startnode))
	    {
		/* We need to update the outbound rights for the current
		   mapping.  Also determine whether we need to revoke or
		   extend the access rights within the subtree. */

		word_t old_rights = mdb_node_get_outrights (node);
		mdb_node_set_outrights (node, rights);
		revoke_rights = (old_rights & rights) != old_rights;
		extend_rights = (old_rights | rights) != old_rights;
		revoke_mask = ~((old_rights & rights) ^ old_rights);
	    }

	    /* Update the effective access rights for the mapping.  Try to
	       perform as little works as possible while doing so. */

	    if (revoke_rights && (! extend_rights))
	    {
		mdb_node_set_rights (node, self,
				     mdb_node_get_rights (node, self) & revoke_mask);
	    }
	    else if (extend_rights)
	    {
		if (! mdb_node_is_my_parent (node, parent))
		    parent = mdb_node_get_parent (node);

		mdb_node_set_rights (node, self,
				     mdb_node_get_rights (parent, self) &
				     mdb_node_get_outrights (node) &
				     mdb_node_get_inrights (node));
	    }

	    /* If rights were revoked we mush make sure that any cached
	       entries (e.g., in TLB) are flushed immediately. */

	    if (revoke_rights)
		mdb_node_flush_cached_entry (node, self, range);
	}

    Skip_node_modifications:

	/* --- PART 2 --- */

	/* Finished operating on the mapping node.  Find next node to operate
	   on. */

	prev = node;

	if (mdb_node_get_table (node))
	{
	    /* We need to recurse into a subtable.  Record the current table
	       and table entry we are working on.  Initially, these will be
	       NULL-entries.  Continue recursion if selected entry in new
	       table is another subtable. */

	    mdb_table_t *nexttab = mdb_node_get_table (node);

	    do {
		ASSERT (recurse_level < MAX_MDB_RECURSION);
		r_table[recurse_level] = table;
		r_values[recurse_level].idx = tableidx & MDB_BITMASK (BITS_WORD - 1);
		r_values[recurse_level++].mod = do_modify;
		table = nexttab;

		if (! do_modify &&
		    mdb_table_match_prefix (nexttab, mdb_range_get_low (&range)) &&
		    (mdb_table_get_radix (nexttab) + mdb_table_get_objsize (nexttab)) >
		    mdb_range_get_size (&range))
		{
		    /* We should only operate on a subset of table entries. */

		    word_t a = mdb_range_get_low (&range);
		    node = mdb_table_get_node_at (table, a);
		    nexttab = mdb_table_get_table (table, a);
		    tableidx = ((word_t) mdb_table_get_entry (table, a) -
				(word_t) mdb_table_get_entry (table, 0))
			/ sizeof (mdb_tableent_t);
		}
		else
		{
		    /* We should operate on all table entries. */

		    node = mdb_table_get_node_at (table, 0);
		    tableidx = 0;
		    nexttab = mdb_table_get_table (table, 0);
		}
	    } while (nexttab);
	}
	else
	{
	    node = mdb_node_get_next (node);
	}

	/* --- PART 3 --- */

	if (node == NULL)
	{
	    /* We have reached the end of a subtree, or alternatively, chosen
	       an emptry subtree from a mapping table.

	       If we have not recursed into any subtables it indicates that we
	       have only operated on a simple tree of a single object size.
	       Just exit the main mapctrl loop and perform potential cleanup at
	       the end of the mapctrl function. */

	    if (recurse_level == 0)
		break;

	    /* --- PART 3a ---- */

	    while (ctrl.unmap)
	    {
		/* We must keep a well defined mapping tree at all times.
		   That is, we don't want to get to the point where a mapping
		   tree A->B->C turns into A->C (e.g., due to a preempted
		   unmap operation).  Such a transformation is conceptually
		   wrong (A never mapped to C) and will cause problems if A
		   attempts to perform a tagged mapctrl on the mapping which
		   used to go to B.  Further, we can get into problems since
		   the depth counts are no longer contigous, causing subtrees
		   to suddenly hook themselves into other subtrees, etc.

		   To prevent not well-defined mapping trees, we always delete
		   mapping trees from bottom up.  That is, when we encounter
		   the end of a linked list we start deleting nodes from the
		   end of the list.

		   Since we terminate the mapctrl loop above if the recursion
		   depth is zero, we know that the backwards delete operation
		   will terminate in a table entry.  The termination criteria
		   in the while loop below detects this. */

		ASSERT (recurse_level > 0);

		if (mdb_node_get_table (prev) != NULL)
		    break;

		do {
		    node = prev;
		    prev = mdb_node_get_prev (prev);
		    mdb_tree_delete_node (self, node);
		} while (mdb_node_get_table (prev) == NULL);

		mdb_table_remove_node (table, mdb_table_get_addr (table, tableidx));
		node = NULL;

		/* Table may fall empty due to delete operation.  If so,
		   delete it and recurse up to the parent table.  Continue
		   deleting tables if they also happen to fall empty. */

		while (table != NULL && mdb_table_get_count (table) == 0)
		{
		    mdb_table_t *t = table;

		    ASSERT (recurse_level > 0);
		    recurse_level--;
		    table = r_table[recurse_level];
		    tableidx = r_values[recurse_level].idx;
		    do_modify = r_values[recurse_level].mod;

		    if (mdb_node_get_table (prev) == t)
			mdb_node_remove_table (prev);
		    else
		    {
			ASSERT (table);
			ASSERT (mdb_table_get_table
				(table, mdb_table_get_addr (table, tableidx)) == t);
			mdb_table_remove_table (table,
						mdb_table_get_addr (table, tableidx));
		    }

		    /* The deleted table may have had an auxiliary mapping
		       node that ended up in the parent table or parent node.
		       If so, continue working on that node. */

		    node = mdb_table_get_node (t);
		    mdb_table_free (t);
		}

		if (table == NULL) ASSERT (recurse_level == 0);
		else ASSERT (recurse_level > 0);

		if (mdb_node_get_table (prev))
		{
		    /* If the previous node still contains a table it must
		       mean that we still have a number of subtrees within the
		       table that we (potientially) need to delete.  Make sure
		       that we don't continue with the regular mapping node
		       before processing the table. */

		    ASSERT (table != NULL);
		    ASSERT (mdb_table_get_count (table) > 0);
		    ASSERT (mdb_table_get_count (mdb_node_get_table (prev)) > 0);
		    node = NULL;
		    break;
		}
		else
		{
		    /* The node no longer contains a table.  This indicates
		       that all the subtrees beneath the mapping table have
		       been deleted. */

		    if (mdb_node_get_next (prev) != NULL)
		    {
			/* Continue performing mapcontrol on next node. */
			node = mdb_node_get_next (prev);
			break;
		    }
		    else if (recurse_level == 0)
		    {
			/* Exit from main loop. */
			ASSERT (table == NULL);
			break;
		    }
		    else
			/* Deleting current subtree. */
			continue;
		}
	    }

	    /* --- PART 3b --- */

	    if (ctrl.reset_status && mdb_node_get_table (prev) == NULL)
	    {
		/* Reset the reference status bits and propagate the old bit
		   contents upwards to the parent of the table we are
		   currently processing.  We don't have to deal with startnode
		   and mapctr_self here since we know that we are operating
		   within a subtable. */

		word_t status = 0;
		mdb_node_t *p = mdb_node_get_prev (prev);
		node = prev;

		do {
		    status |= mdb_node_reset_effective_status (node, self);
		    mdb_node_flush_cached_entry (node, self, range);
		    mdb_node_update_purged_status (node, self, status);

		    if (! mdb_node_is_my_parent (node, p))
		    {
			mdb_node_update_effective_status (mdb_node_get_parent (node),
							  self, status);
			status = 0;
		    }

		    node = p;
		    p = mdb_node_get_prev (p);

		} while (mdb_node_get_next (p) == node);

		mdb_node_update_effective_status (p, self, status);
		node = NULL;
	    }

	    /* --- PART 3c --- */

	    /* We have now possibly finished processing a whole subtree, and
	       the (table, tableidx) tuple indicates which mapping table entry
	       we are currently working on (if any).  It might be that we have
	       just recursed up from a subtable.  If so, and we have not found
	       a proper node to work on yet, we try to search through the
	       mapping table entries for valid nodes or recurse into subtables
	       if they exist. */

	    while (node == NULL && table != NULL)
	    {
		if (tableidx < (1UL << mdb_table_get_radix (table)) - 1)
		{
		    /* Try next table entry.  Recurse into subtable(s) if
		       necessary. */

		    ASSERT (recurse_level > 0);
		    tableidx++;

		    if (! r_values[recurse_level - 1].mod &&
			! mdb_range_in_range (&range,
					      (mdb_table_get_prefix (table) &
					       ~mdb_table_get_mask (table)) +
					      mdb_table_get_addr (table, tableidx)))
		    {
			/* We have reached the end of a mapctrl range in a
			   table that should only be partially processed.
			   Force algorithm to recurse up to previous level. */

			tableidx = (1UL << mdb_table_get_radix (table)) - 1;
			continue;
		    }

		    while (mdb_table_get_table (table,
						mdb_table_get_addr (table, tableidx)))
		    {
			ASSERT (recurse_level < MAX_MDB_RECURSION);
			r_table[recurse_level] = table;
			r_values[recurse_level].idx = tableidx &
			    MDB_BITMASK (BITS_WORD - 1);
			r_values[recurse_level++].mod = do_modify;
			table = mdb_table_get_table (table,
						     mdb_table_get_addr (table, tableidx));
			tableidx = 0;
		    }
		    node = mdb_table_get_node_at (table,
						  mdb_table_get_addr (table, tableidx));
		}
		else
		{
		    /* Reached end of mapping table.  Recurse up to previous
		       mapping table.  Before continuing to parse parent
		       table, we first try to get the next mapping node out of
		       the current table structure.  If table has fallen empty
		       because of an unmap operation it would have been
		       deleted in step 3a. */

		    node = mdb_table_get_node (table);

		    ASSERT (recurse_level > 0);
		    recurse_level--;
		    table = r_table[recurse_level];
		    tableidx = r_values[recurse_level].idx;
		    do_modify = r_values[recurse_level].mod;
		}
	    }

	    /* --- PART 3d --- */

	    /* If we still have not found a valid mapping node it means that
	       we are finished parsing the whole subtree.  Exit the main
	       loop. */

	    if (node == NULL)
	    {
		ASSERT (recurse_level == 0);
		if (ctrl.unmap)
		    ASSERT (mdb_node_get_next (prev) == NULL);
		break;
	    }
	}

    } while (mdb_node_get_depth (node) > mdb_node_get_depth (startnode));

    if (ctrl.unmap)
    {
	/* Remove the mapping tree we have just parsed thorugh and propagate
	   the status bits upwards to the parent of the deleted mapping
	   tree. */

	node = prev;
	prev = mdb_node_get_prev (node);

	while (node != startnode)
	{
	    status_bits |= mdb_node_get_effective_status (node, self);
	    mdb_tree_delete_node (self, node);
	    node = prev;
	    prev = mdb_node_get_prev (node);
	}

	if (ctrl.mapctrl_self)
	{
	    status_bits |= mdb_node_get_effective_status (startnode, self);
	    mdb_node_update_effective_status (mdb_node_get_parent (startnode),
					      self, status_bits);
	    mdb_tree_delete_node (self, startnode);
	}
	else
	    mdb_node_update_effective_status (startnode, self, status_bits);
    }

    if (ctrl.reset_status)
    {
	/* We've reached the end of the subtree.  Reset and propagate
	   reference status bits upwards to the starting node. */

	word_t status = 0;
	node = prev;
	prev = mdb_node_get_prev (node);

	while (node != startnode)
	{
	    status |= mdb_node_reset_effective_status (node, self);
	    mdb_node_flush_cached_entry (node, self, range);
	    mdb_node_update_purged_status (node, self, status);

	    if (! mdb_node_is_my_parent (node, prev))
	    {
		mdb_node_update_effective_status (mdb_node_get_parent (node),
						  self, status);
		status = 0;
	    }

	    node = prev;
	    prev = mdb_node_get_prev (prev);
	}

	mdb_node_update_effective_status (node, self, status);

	if (ctrl.mapctrl_self)
	{
	    /* If we also reset the bits in the startnode, make sure that we
	       propagate the bits to the parent of the startnode. */

	    status |= mdb_node_reset_effective_status (startnode, self);
	    mdb_node_flush_cached_entry (startnode, self, range);
	    mdb_node_reset_purged_status (startnode, self);
	    mdb_node_update_effective_status (mdb_node_get_parent (startnode),
					      self, status);
	}
    }

    spinlock_unlock (&mdb_lock);
    return status_bits;
}


/**
 * Locate parent of current mapping node.
 * @return pointer to parent
 */
mdb_node_t * mdb_node_get_parent (mdb_node_t *self)
{
    mdb_node_t *p = self;

    do { p = mdb_node_get_prev (p); }
    while (mdb_node_get_depth (p) >= mdb_node_get_depth (self));

    return p;
}
