/*********************************************************************
 *                
 * Copyright (C) 2005-2007,  Karlsruhe University
 *                
 * File path:     generic/vrt.c
 * Description:   Generic Variable Radix Table for managing mappings
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
 * $Id: vrt.cc,v 1.14 2006/11/14 18:54:54 skoglund Exp $
 *                
 ********************************************************************/
#include <vrt.h>
#include <mdb.h>
#include <kdb/tracepoints.h>

#include INC_API (fpage.h)


DECLARE_TRACEPOINT (VRT_MAP);
DECLARE_TRACEPOINT (VRT_OVERMAP);
DECLARE_TRACEPOINT (VRT_MAPCTRL);

/* dispatch shorthands for the vrt_t ops table */
#define VRT_NAME(v)		((v)->ops->get_name (v))
#define VRT_RADIX(v, sz)	((v)->ops->get_radix ((v), (sz)))
#define VRT_NEXT_OBJSIZE(v, sz)	((v)->ops->get_next_objsize ((v), (sz)))
#define VRT_SIZE(v)		((v)->ops->get_vrt_size (v))
#define VRT_MAPDB(v)		((v)->ops->get_mapdb (v))


/**
 * Add buffer allocation sizes.
 */
MDB_INIT_FUNCTION (1, init_vrt_sizes)
{
    void mdb_add_size (word_t size);	/* generic/mapping_alloc.c */
    mdb_add_size (sizeof (vrt_table_t));
}


/**
 * Create and partially initialize a new table object.  Allocates both
 * the table control structure and the table entries themselves.
 * Was vrt_table_t::operator new.
 *
 * @param radix_log2	size of table
 *
 * @return new table object
 */
vrt_table_t * vrt_table_alloc (word_t radix_log2)
{
    vrt_table_t *t = (vrt_table_t *) mdb_alloc_buffer (sizeof (vrt_table_t));
    vrt_node_t *n;
    word_t k;

    t->radix = radix_log2 & MDB_BITMASK (6);
    t->entries = (word_t)
	mdb_alloc_buffer ((sizeof (vrt_node_t) + sizeof (mdb_node_t *)) *
			  (1UL << radix_log2));

    n = vrt_table_get_node (t, 0);
    for (k = 0; k < (1UL << radix_log2); k++, n++)
	vrt_node_clear (n);

    return t;
}


/**
 * Remove table.  The operation assumes that the table is empty.
 * Was vrt_table_t::operator delete.
 *
 * @param t		table to remove
 */
void vrt_table_free (vrt_table_t *t)
{
    if (t == NULL)
	return;

    mdb_free_buffer ((void *) (word_t) t->entries,
		     (sizeof (vrt_node_t) + sizeof (mdb_node_t *)) *
		     (1UL << vrt_table_get_radix (t)));
    mdb_free_buffer (t, sizeof (vrt_table_t));
}


/**
 * Lookup object in table.
 *
 * @param addr		address of mapping
 * @param value		returned object value
 * @param objsize	returned object size
 *
 * @return true if object found, false otherwise
 */
bool vrt_lookup (vrt_t *self, word_t addr, word_t *value, word_t *objsize)
{
    vrt_table_t *table = vrt_get_table (self);
    vrt_node_t *node;

    for (;;)
    {
	if (! vrt_table_match_prefix (table, addr))
	    return false;

	node = vrt_table_get_node (table, addr);

	if (! vrt_node_is_valid (node))
	    return false;

	if (! vrt_node_is_table (node))
	{
	    *objsize = vrt_table_get_objsize (table);
	    *value = vrt_node_get_object (node);
	    return true;
	}

	table = vrt_node_get_table (node);
    }

    /* NOTREACHED */
    return false;
}


/**
 * Perform a map operation within current table/space.
 *
 * @param f_fp		flexpage for current space
 * @param base		send base to use for mapping
 * @param t_space	destination table/space
 * @param t_fp		flexpage for destination space
 * @param grant		true if map operation is a grant
 */
void vrt_map_fpage (vrt_t *self, fpage_t f_fp, word_t base, vrt_t *t_space,
		    fpage_t t_fp, bool grant)
{
    word_t f_addr, t_addr, f_num, t_num, mapsize, offset;
    mdb_node_t *map;
    mdb_node_t *f_map;
    vrt_table_t *f_table, *t_table;
    vrt_node_t *f_node, *t_node;
    word_t f_off = 0;

    /* Arrays to use for recursion.  We don't want to do full function
       recursion because of stack space requirements. */

    vrt_table_t * r_ftable[MAX_VRT_DEPTH];
    vrt_node_t *  r_fnode[MAX_VRT_DEPTH];
    word_t	  r_fnum[MAX_VRT_DEPTH];
    vrt_table_t * r_ttable[MAX_VRT_DEPTH];
    vrt_node_t *  r_tnode[MAX_VRT_DEPTH];
    word_t	  r_tnum[MAX_VRT_DEPTH];
    word_t f_depth = 0, t_depth = 0;

    TRACEPOINT (VRT_MAP, "%s::%s (fp=%p [%p,%d] base=%p) to %s (fp=%p [%p,%d])\n",
			VRT_NAME (self), grant ? "grant" : "map",
			f_fp.raw, fpage_get_base (&f_fp), fpage_get_size_log2 (&f_fp),
			base, VRT_NAME (t_space), t_fp.raw,
			fpage_get_base (&t_fp), fpage_get_size_log2 (&t_fp));

    /* Determine the exact location for where to map from and where to
       map to. */

    if (fpage_get_size_log2 (&f_fp) <= fpage_get_size_log2 (&t_fp))
    {
	f_num = fpage_get_size_log2 (&f_fp);
	base &= fpage_base_mask (t_fp, f_num);
	f_addr = (word_t) fpage_address (f_fp, f_num);
	t_addr = (word_t) addr_offset
	    (addr_mask (fpage_address (t_fp, f_num),
			~fpage_base_mask (t_fp, f_num)), base);
    }
    else
    {
	f_num = fpage_get_size_log2 (&t_fp);
	base &= fpage_base_mask (f_fp, f_num);
	f_addr = (word_t) addr_offset
	    (addr_mask (fpage_address (f_fp, f_num),
			~fpage_base_mask (f_fp, f_num)), base);
	t_addr = (word_t) fpage_address (t_fp, f_num);
    }

    /* Determine the size and number of objects to map. */

    if (f_num > VRT_SIZE (self))
	f_num = VRT_SIZE (self);
    mapsize = VRT_NEXT_OBJSIZE (self, f_num + 1);
    f_num = t_num = 1UL << (f_num - mapsize);

    f_table = vrt_get_table (self);
    t_table = vrt_get_table (t_space);
    f_node = vrt_table_get_node (f_table, f_addr);
    t_node = vrt_table_get_node (t_table, t_addr);

    while (f_num > 0 || t_num > 0)
    {
	if (! vrt_node_is_valid (f_node) ||
	    (vrt_node_is_table (f_node) &&
	     ! vrt_table_match_prefix (vrt_node_get_table (f_node), f_addr)))
	{
	    /* There exist no valid object in source space.  Skip the whole
	       mapping (possibly recursing up from destination space first). */

	    while (vrt_table_get_objsize (t_table) < vrt_table_get_objsize (f_table))
	    {
		t_depth--;
		t_table = r_ttable[t_depth];
		t_node = r_tnode[t_depth];
		t_num = r_tnum[t_depth];
	    }

	    if (vrt_table_get_objsize (t_table) == vrt_table_get_objsize (f_table))
		goto Next_receiver_entry;

	    /* Destination obj size > source obj size. */
	    goto Next_sender_entry;
	}


	if (vrt_table_get_objsize (f_table) > mapsize && vrt_node_is_table (f_node))
	{
	    /* We are working on too large source nodes.  Skip down into
	       subtable. */

	    f_table = vrt_node_get_table (f_node);
	    f_node = vrt_table_get_node (f_table, f_addr);
	    continue;
	}
	else if (vrt_node_is_table (f_node))
	{
	    /* Mappings in sender space are small.  Need to map every single
	       object within subtable(s). */

	    r_ftable[f_depth] = f_table;
	    r_fnode[f_depth] = f_node + 1;
	    r_fnum[f_depth] = f_num - 1;
	    f_depth++;

	    f_table = vrt_node_get_table (f_node);
	    f_node = vrt_table_get_node (f_table, 0);
	    f_num = 1UL << vrt_table_get_radix (f_table);
	    continue;
	}
	else if (vrt_table_get_objsize (f_table) > mapsize)
	{
	    /* We are mapping smaller sub-objects out of a larger object. */

	    f_num = 1;
	}

	/* We have now finished looking up the mapping in the source space.
	   Current status is:

	     f_node - object to map from
	     f_table - table where object resides
	     f_num - number of f_node objects (of this size) to map

	   Next we have to find the location in the destination space where to
	   insert the new object(s). */

	if ((vrt_table_get_objsize (t_table) > vrt_table_get_objsize (f_table)) ||
	    (vrt_table_get_objsize (t_table) > mapsize))
	{
	    /* We are currently working on too large receive sizes.  We need
	       to either recurse into a subtable, or if no such table exist,
	       create a subtable. */

	    r_ttable[t_depth] = t_table;
	    r_tnode[t_depth] = t_node + 1;
	    r_tnum[t_depth] = t_num - 1;
	    t_depth++;

	    if (vrt_node_is_valid (t_node) && ! vrt_node_is_table (t_node))
	    {
		/* We are overmapping a large object with a smaller one.  We
		   need to unmap the larger object. */

		TRACEPOINT (VRT_OVERMAP, "%s overmap: faddr=%p fsz=%d "
			    "taddr=%p tsz=%d (single entry)\n",
			    VRT_NAME (t_space),
			    f_addr, vrt_table_get_objsize (f_table),
			    t_addr, vrt_table_get_objsize (t_table));

		mdb_tree_flush (VRT_MAPDB (t_space),
				vrt_table_get_mapnode (t_table, t_addr));
		ASSERT (! vrt_node_is_valid (t_node));

		/* We might have unmapped the source mapping during the flush
		   operation. */

		if (! vrt_node_is_valid (f_node))
		    goto Next_receiver_entry;
	    }

	    if ((! vrt_node_is_valid (t_node)) ||
		(vrt_node_is_table (t_node) &&
		 ! vrt_table_match_prefix (vrt_node_get_table (t_node), t_addr)))
	    {
		/* Appropriate subtable does not exist.  Create a table
		   according to the wanted object size.  We perform path
		   compression to avoid unnecessary lookup and memory
		   overhead. */

		word_t newsize = vrt_table_get_objsize (f_table) < mapsize ?
		    vrt_table_get_objsize (f_table) : mapsize;
		vrt_table_t *newtable = vrt_table_alloc (VRT_RADIX (self, newsize));

		vrt_table_set_prefix (newtable, t_addr);
		vrt_table_set_objsize (newtable, newsize);

		if (vrt_node_is_valid (t_node))
		{
		    ASSERT (vrt_node_is_table (t_node));

		    if (vrt_table_match_prefix
			(newtable, vrt_table_get_prefix (vrt_node_get_table (t_node))))
		    {
			/* There exists a sub-table that is completely
			   contained within the new table.  Remove it. */

			word_t num = 1;
			word_t addr = t_addr;
			word_t start_depth = t_depth;

			TRACEPOINT (VRT_OVERMAP,"%s overmap: faddr=%p fsz=%d "
				    "taddr=%p (subtable <%p,%p>)\n",
				    VRT_NAME (t_space),
				    f_addr, vrt_table_get_objsize (f_table),
				    t_addr,
				    vrt_table_get_start_addr (vrt_node_get_table (t_node)),
				    vrt_table_get_end_addr (vrt_node_get_table (t_node)));

			while (num > 0)
			{
			    if (! vrt_node_is_valid (t_node))
			    {
				/* Skip invalid entries. */
			    }
			    else if (vrt_node_is_table (t_node))
			    {
				/* We must unmap each single object in
				   subtable. */

				r_ttable[t_depth] = t_table;
				r_tnode[t_depth] = t_node;
				r_tnum[t_depth] = num - 1;
				t_depth++;

				t_table = vrt_node_get_table (t_node);
				t_node = vrt_table_get_node (t_table, 0);
				num = 1UL << vrt_table_get_radix (t_table);
				continue;
			    }
			    else
			    {
				/* Unmap object from destination space. */

				mdb_tree_flush (VRT_MAPDB (t_space),
						vrt_table_get_mapnode (t_table, addr));
			    }

			    /* Skip to next table entry. */

			    addr += 1UL << vrt_table_get_objsize (t_table);
			    if (t_depth > start_depth)
				t_node++;
			    num--;

			    while (num == 0 && t_depth > start_depth)
			    {
				/* Recurse up an remove subtable. */

				t_depth--;
				t_table = r_ttable[t_depth];
				t_node = r_tnode[t_depth];
				num = r_tnum[t_depth];

				vrt_table_free (vrt_node_get_table (t_node));
				vrt_node_clear (t_node);
				if (t_depth > start_depth)
				    t_node++;
			    }

			    /* Don't delete the source node */

			    if (t_depth == start_depth)
				break;
			}

			/* We might have unmapped the source mapping during
			   the flush operation. */

			if (! vrt_node_is_valid (f_node))
			{
			    vrt_table_free (newtable);
			    goto Next_receiver_entry;
			}
		    }
		    else
		    {
			/* There is a conflict between the newly created table
			   and a previously existing table (i.e., both tables
			   fall under the same parent table entry).  We need to
			   create an intermediate table that can resolve the
			   conflict.

			   The intermediate table will hold objects of sizes
			   which is the maximum valid object size that does not
			   generate a mapping table where the two subtables map
			   to the same table entry. */

			word_t imask;
			word_t isize = vrt_table_get_objsize (t_table);
			vrt_table_t *itable;

			do {
			    isize = VRT_NEXT_OBJSIZE (self, isize);
			    imask = VRT_RADIX (self, isize) + isize;
			    imask = (imask == sizeof (word_t) * 8 ?
				     ~0UL : ((1UL << imask) - 1))
				& ~((1UL << isize) - 1);
			} while ((t_addr & imask) ==
				 (vrt_table_get_prefix (vrt_node_get_table (t_node)) &
				  imask));

			ASSERT (isize > newsize);
			ASSERT (isize > vrt_table_get_objsize (vrt_node_get_table (t_node)));

			itable = vrt_table_alloc (VRT_RADIX (self, isize));

			vrt_table_set_prefix (itable, t_addr);
			vrt_table_set_objsize (itable, isize);
			vrt_table_set_table (itable, t_addr, newtable);
			vrt_table_set_table
			    (itable, vrt_table_get_prefix (vrt_node_get_table (t_node)),
			     vrt_node_get_table (t_node));
			newtable = itable;
		    }
		}

		vrt_table_set_table (t_table, t_addr, newtable);
	    }

	    ASSERT (vrt_node_is_valid (t_node) && vrt_node_is_table (t_node) &&
		    vrt_table_match_prefix (vrt_node_get_table (t_node), t_addr));

	    if (vrt_table_get_objsize (vrt_node_get_table (t_node)) >= mapsize)
	    {
		/* We still haven't recursed down to the object size we are
		   supposed to operate on. */

		t_table = vrt_node_get_table (t_node);
		t_node = vrt_table_get_node (t_table, t_addr);
		continue;
	    }

	    t_table = vrt_node_get_table (t_node);
	    t_node = vrt_table_get_node (t_table, t_addr);
	    t_num = 1UL << vrt_table_get_radix (t_table);
	    continue;
	}

	else if (vrt_node_is_valid (t_node) &&
		 vrt_table_get_objsize (t_table) <= vrt_table_get_objsize (f_table))
	{
	    /* We are ovemapping a node or a whole subtree.  Unmap each single
	       entry in the subtree. */

	    word_t num = 1;
	    word_t addr = t_addr;
	    word_t start_depth = t_depth;

	    TRACEPOINT (VRT_OVERMAP, "%s overmap: faddr=%p fsz=%d "
			"taddr=%p tsz=%d (%s)\n",
			VRT_NAME (self),
			f_addr, vrt_table_get_objsize (f_table),
			t_addr, vrt_table_get_objsize (t_table),
			vrt_node_is_table (t_node) ? "subtable" : "single entry");

	    while (num > 0)
	    {
		if (! vrt_node_is_valid (t_node))
		{
		    /* Skip invalid entries. */
		}
		else if (vrt_node_is_table (t_node))
		{
		    /* We must unmap each single object in subtable. */

		    r_ttable[t_depth] = t_table;
		    r_tnode[t_depth] = t_node;
		    r_tnum[t_depth] = num - 1;
		    t_depth++;

		    t_table = vrt_node_get_table (t_node);
		    t_node = vrt_table_get_node (t_table, 0);
		    num = 1UL << vrt_table_get_radix (t_table);
		    continue;
		}
		else
		{
		    /* Unmap object from destination space. */

		    mdb_tree_flush (VRT_MAPDB (t_space),
				    vrt_table_get_mapnode (t_table, addr));
		}

		/* Skip to next table entry. */

		addr += 1UL << vrt_table_get_objsize (t_table);
		if (t_depth > start_depth)
		    t_node++;
		num--;

		while (num == 0 && t_depth > start_depth)
		{
		    /* Recurse up an remove subtable. */

		    t_depth--;
		    t_table = r_ttable[t_depth];
		    t_node = r_tnode[t_depth];
		    num = r_tnum[t_depth];

		    vrt_table_free (vrt_node_get_table (t_node));
		    vrt_node_clear (t_node);
		    if (t_depth > start_depth)
			t_node++;
		}
	    }

	    /* We might have unmapped the source mapping during the flush
	       operation. */

	    if (! vrt_node_is_valid (f_node))
		goto Next_receiver_entry;
	}

	/* We have now finished looking up the mapping in both the source and
	   the destination space

	     f_node  - object to map from
	     f_table - table where source object resides
	     f_num   - number of f_node objects (of this size) to map
	     t_node  - object to map to
	     t_table - table where destination object resides
	     t_num   - number of t_node objects (of this size) to map

	   Further, the t_node is guaranteed to be invalid at this point. */

	ASSERT (! vrt_node_is_valid (t_node));

	offset = f_addr
	    & ((1UL << vrt_table_get_objsize (f_table)) - 1)
	    & ~((1UL << vrt_table_get_objsize (t_table)) - 1);

	f_map = vrt_table_get_mapnode (f_table, f_addr);
	if (grant)
	    f_map = mdb_node_get_parent (f_map);

	vrt_table_set_object (t_table, t_space, t_addr,
			      vrt_node_get_address (f_node, self) + offset + f_off,
			      f_node, vrt_table_get_objsize (f_table),
			      fpage_get_rwx (&f_fp));
	map = mdb_tree_map (VRT_MAPDB (self), f_map, t_node,
			    vrt_table_get_objsize (t_table),
			    vrt_node_get_address (f_node, self) + offset + f_off,
			    fpage_get_rwx (&f_fp), ~0UL);
	mdb_node_set_misc (map, t_space->ops->make_misc (t_space, t_node, map));
	vrt_table_set_mapnode (t_table, t_addr, map);

    Next_receiver_entry:

	t_addr += 1UL << vrt_table_get_objsize (t_table);
	t_num--;

	if (t_num > 0)
	{
	    t_node++;
	    if (vrt_table_get_objsize (t_table) < vrt_table_get_objsize (f_table))
	    {
		/* We are mapping smaller objects out of a large object.
		   Update the object offset for the mapping in the desination
		   space and do not skip to the next source object yet. */

		f_off += 1UL << vrt_table_get_objsize (t_table);
		continue;
	    }
	}
	else if (vrt_table_get_objsize (t_table) < vrt_table_get_objsize (f_table) &&
		 vrt_table_get_objsize (f_table) <= mapsize)
	{
	    /* We have finished mapping a subtable.  Recurse up to parent
	       table. */

	    do {
		f_off += 1UL << vrt_table_get_objsize (t_table);
		t_depth--;
		t_table = r_ttable[t_depth];
		t_node = r_tnode[t_depth];
		t_num = r_tnum[t_depth];
	    } while (t_num == 0 &&
		     vrt_table_get_objsize (t_table) < vrt_table_get_objsize (f_table));

	    /* If mapping large object into smaller objects, don't skip to the
	       next source object yet. */

	    if (t_num > 0 &&
		vrt_table_get_objsize (t_table) < vrt_table_get_objsize (f_table))
		continue;
	}

    Next_sender_entry:

	if (grant)
	    mdb_tree_flush (VRT_MAPDB (self), vrt_table_get_mapnode (f_table, f_addr));

	f_addr += 1UL << vrt_table_get_objsize (f_table);
	f_num--;
	f_off = 0;

	if (f_num > 0)
	{
	    f_node++;
	    continue;
	}
	else if (f_depth > 0)
	{
	    /* We have finished mapping a subtable.  Recurse up to the parent
	       table. */

	    do {
		f_depth--;
		f_table = r_ftable[f_depth];
		f_node = r_fnode[f_depth];
		f_num = r_fnum[f_depth];
	    } while (f_num == 0 && f_depth > 0);

	    /* We may also need to recurse up in the receiver space now. */

	    while (t_num == 0 && t_depth > 0)
	    {
		t_depth--;
		f_off += 1UL << vrt_table_get_objsize (t_table);
		t_table = r_ttable[t_depth];
		t_node = r_tnode[t_depth];
		t_num = r_tnum[t_depth];
	    }
	}
	else
	{
	    /* Finished. */
	    t_num = 0;
	}
    }
}


/**
 * Perform a map control operation within current table/space.
 *
 * @param fp		flexpage to perform mapctrl on
 * @param ctrl		type of operation
 * @param rights	new access rights
 * @param attrib	new attributes
 *
 * @return status bits for flexpage
 */
word_t vrt_mapctrl (vrt_t *self, fpage_t fp, mdb_ctrl_t ctrl,
		    word_t rights, word_t attrib)
{
    word_t mapsize, num, vaddr, status = 0;
    vrt_table_t *table;
    vrt_node_t *node;

    /* Arrays to use for recursion.  We don't want to do full function
       recursion because of stack space requirements. */

    vrt_table_t * r_table[MAX_VRT_DEPTH];
    vrt_node_t *  r_node[MAX_VRT_DEPTH];
    word_t	  r_num[MAX_VRT_DEPTH];
    word_t depth = 0;

    TRACEPOINT (VRT_MAPCTRL,
		"%s::mapctrl (%p [%p,%d], %x [%s] %x, %p)\n",
		VRT_NAME (self),
		fp.raw, fpage_get_base (&fp), fpage_get_size_log2 (&fp),
		ctrl.raw, mdb_ctrl_string (&ctrl), rights & 0x7, attrib);

    /* Determine size and number of objects to mapctrl.  Make sure that we
       stay within the bounds of the space even if user specified an fpage
       larger than the space. */

    mapsize = VRT_NEXT_OBJSIZE (self, fpage_get_size_log2 (&fp) + 1);
    num = VRT_SIZE (self);
    if (num > fpage_get_size_log2 (&fp))
	num = fpage_get_size_log2 (&fp);
    num = 1UL << (num - mapsize);

    vaddr = (word_t) fpage_address (fp, mapsize);
    table = vrt_get_table (self);
    node = vrt_table_get_node (table, vaddr);

    while (num)
    {
	if (! vrt_node_is_valid (node) ||
	    (vrt_node_is_table (node) &&
	     ! fpage_is_range_overlapping
	     (&fp,
	      (addr_t) vrt_table_get_start_addr (vrt_node_get_table (node)),
	      (addr_t) vrt_table_get_end_addr (vrt_node_get_table (node)))))
	{
	    /* There exist no valid object in source space.  Skip the whole
	       mapping. */

	    goto Next_entry;
	}

	if (vrt_table_get_objsize (table) > mapsize && vrt_node_is_table (node))
	{
	    /* We are working on too large source nodes.  Skip down into
	       subtable. */

	    table = vrt_node_get_table (node);
	    node = vrt_table_get_node (table, vaddr);
	    continue;
	}
	else if (vrt_node_is_table (node))
	{
	    /* Mappings in space are small.  Need to mapctrl every single
	       object within subtable(s). */

	    r_table[depth] = table;
	    r_node[depth] = node;
	    r_num[depth] = num - 1;
	    depth++;

	    table = vrt_node_get_table (node);
	    node = vrt_table_get_node (table, 0);
	    num = 1UL << vrt_table_get_radix (table);
	    continue;
	}
	else if (vrt_table_get_objsize (table) > mapsize)
	{
	    /* We are performing mapctrl on too small a mapping.  Increase the
	       scope of the mapctrl operation. */

	    num = 1;
	    mapsize = vrt_table_get_objsize (table);
	}

	mdb_tree_mapctrl (VRT_MAPDB (self), vrt_table_get_mapnode (table, vaddr),
			  mdb_range_make (fpage_get_base (&fp),
					  fpage_get_size_log2 (&fp)),
			  ctrl, rights, attrib);

    Next_entry:

	vaddr += 1UL << vrt_table_get_objsize (table);
	num--;

	if (num > 0)
	    node++;
	else if (depth > 0)
	{
	    /* We have finished parsing a subtable.  Recurse up to the parent
	       table.  Delete table if we are doing unmap in the current
	       space. */

	    do {
		depth--;
		table = r_table[depth];
		node = r_node[depth];
		num = r_num[depth];

		if (ctrl.unmap && ctrl.mapctrl_self)
		{
		    vrt_table_free (vrt_node_get_table (node));
		    vrt_node_clear (node);
		}
		node++;
	    } while (num == 0 && depth > 0);
	}
    }

    return status;
}
