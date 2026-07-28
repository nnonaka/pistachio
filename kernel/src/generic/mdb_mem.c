/*********************************************************************
 *                
 * Copyright (C) 2005-2007, 2010,  Karlsruhe University
 *                
 * File path:     generic/mdb_mem.c
 * Description:   Memory specific generic mapping database functions
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
 * $Id: mdb_mem.cc,v 1.9 2007/01/08 14:11:23 skoglund Exp $
 *                
 ********************************************************************/
#include <mdb_mem.h>
#include <linear_ptab.h>
#include INC_ARCH(pgent.h)
#include INC_API(tcb.h)
#include INC_API(space.h)
#include INC_GLUE(space.h)


/*
 * Helper functions.  These were file-scope INLINEs named pgsize/space, which
 * would now collide with the C accessors, so they carry an mm_ prefix.
 */

INLINE word_t mm_pgsize (mdb_node_t *node)
{
    mdb_mem_misc_t misc; misc.raw = mdb_node_get_misc (node);
    return (word_t) misc.pgsize;
}

INLINE word_t mm_purged_status (mdb_node_t *node)
{
    mdb_mem_misc_t misc; misc.raw = mdb_node_get_misc (node);
    return misc.purged_status;
}

INLINE space_t * mm_space (mdb_node_t *node)
{
    mdb_mem_misc_t misc; misc.raw = mdb_node_get_misc (node);
    return (space_t *) (word_t) (misc.space << 8);
}


/*
 * MDB specific functions.  These were the mdb_mem_t overrides of the mdb_t
 * virtuals; they are now the entries of mdb_mem_ops, each taking the database
 * as its first argument.
 */


/**
 * Get radix to use for mapping table.
 * @param objsize	size of objects in table
 * @return radix to use for mapping table
 */
static word_t mm_get_radix (mdb_t *self, word_t objsize)
{
    word_t k;

    (void) self;
    ASSERT (objsize < mdb_mem_sizes[mdb_mem_num_sizes]);

    for (k = 0; objsize >= mdb_mem_sizes[k]; k++) {}
    return mdb_mem_sizes[k] - objsize;
}

/**
 * Get the next smaller valid object size.
 * @param objsize	size of object
 * @return size of next smaller object size
 */
static word_t mm_get_next_objsize (mdb_t *self, word_t objsize)
{
    word_t k;

    (void) self;
    ASSERT (objsize > mdb_mem_sizes[0]);

    for (k = mdb_mem_num_sizes - 1; objsize <= mdb_mem_sizes[k]; k--) {}
    return mdb_mem_sizes[k];
}

/**
 * Get name of mapping database.
 * @return mapping database name
 */
static const char * SECTION(SEC_KDEBUG) mm_get_name (mdb_t *self)
{
    (void) self;
    return "mem";
}


/*
 * MDB specific mapping node functions.
 */


/**
 * Clear the mapping and flush any cached entries.
 * @param node		mapping node
 */
static void mm_clear (mdb_t *self, mdb_node_t *node)
{
    pgent_t *pg = (pgent_t *) mdb_node_get_object (node);
    addr_t vaddr;

    (void) self;
    vaddr = pgent_vaddr (pg, mm_space (node), mm_pgsize (node), node);
    pgent_clear (pg, mm_space (node), mm_pgsize (node), 0, vaddr);
    pgent_flush (pg, mm_space (node), mm_pgsize (node), 0, vaddr);
    space_flush_tlbent (mm_space (node), get_current_space_c (), vaddr,
			mdb_node_get_objsize (node));
}

/**
 * Get effective access rights of mapping.
 * @param node		mapping node
 * @return effective access rights.
 */
static word_t mm_get_rights (mdb_t *self, mdb_node_t *node)
{
    pgent_t *pg = (pgent_t *) mdb_node_get_object (node);
    (void) self;
    return pgent_rights (pg, mm_space (node), mm_pgsize (node));
}

/**
 * Set effective access rights for mapping.  Note that the TLB entry
 * for the mapping is not flushed.  This must be done explecitly
 * afterwards.
 * @param node		mapping node
 * @param r		new access rights
 */
static void mm_set_rights (mdb_t *self, mdb_node_t *node, word_t r)
{
    pgent_t *pg = (pgent_t *) mdb_node_get_object (node);
    (void) self;
    pgent_set_rights (pg, mm_space (node), mm_pgsize (node), r);
}

/**
 * Flush TLB entry for mapping.
 * @param node		mapping node
 */
static void mm_flush_cached_entry (mdb_t *self, mdb_node_t *node, mdb_range_t range)
{
    pgent_t *pg;
    addr_t vaddr;

    (void) self;
    if (space_does_tlbflush_pay (mdb_range_get_size (&range)))
	return;

    pg = (pgent_t *) mdb_node_get_object (node);
    vaddr = pgent_vaddr (pg, mm_space (node), mm_pgsize (node), node);
    pgent_flush (pg, mm_space (node), mm_pgsize (node), 0, vaddr);
    space_flush_tlbent (mm_space (node), get_current_space_c (), vaddr,
			mdb_node_get_objsize (node));
}

/**
 * Check whether attribute updates are allowed for mapping.
 * @param node		mapping node
 * @return true if attribute updates are allowed
 */
static bool mm_allow_attribute_update (mdb_t *self, mdb_node_t *node)
{
    /* XXX: Temporary solution for V4 */
    (void) self; (void) node;
    return is_privileged_space_c (get_current_space_c ());
}

/**
 * Set attribute for mapping.
 * @param node		mapping node
 * @param attrib	new attribute
 */
static void mm_set_attribute (mdb_t *self, mdb_node_t *node, word_t attrib)
{
    pgent_t *pg = (pgent_t *) mdb_node_get_object (node);
    (void) self;
    pgent_set_attributes (pg, mm_space (node), mm_pgsize (node), attrib);
}

/**
 * Get physical address of mapping.
 * @param node		mapping node
 * @return physical address of page frame
 */
static word_t mm_get_phys_address (mdb_t *self, mdb_node_t *node)
{
    pgent_t *pg = (pgent_t *) mdb_node_get_object (node);
    (void) self;
    return (word_t) pgent_address (pg, mm_space (node), mm_pgsize (node));
}

/**
 * Get the purged status bits for mapping.
 * @param node		mapping node
 * @return purged status bits
 */
static word_t mm_get_purged_status (mdb_t *self, mdb_node_t *node)
{
    (void) self;
    return mm_purged_status (node);
}

/*
 * Reset the purged status bits for mapping.
 * @param node		mapping node
 */
static void mm_reset_purged_status (mdb_t *self, mdb_node_t *node)
{
    (void) self;
    mdb_node_set_misc (node, mdb_mem_misc (mm_space (node), mm_pgsize (node)));
}

/**
 * Update the purged status bits for mapping.
 * @param node		mapping node
 * @param status	new status bits
 */
static void mm_update_purged_status (mdb_t *self, mdb_node_t *node, word_t status)
{
    (void) self;
    mdb_node_set_misc (node, mdb_mem_misc_stat (mm_space (node),
						mm_pgsize (node),
						mm_purged_status (node) | status));
}

/**
 * Get effective status bits for mapping.
 * @param node		mapping node
 * @return effective status bits
 */
static word_t mm_get_effective_status (mdb_t *self, mdb_node_t *node)
{
    pgent_t *pg = (pgent_t *) mdb_node_get_object (node);
    addr_t vaddr;

    (void) self;
    vaddr = pgent_vaddr (pg, mm_space (node), mm_pgsize (node), node);
    return pgent_reference_bits (pg, mm_space (node), mm_pgsize (node), vaddr);
}

/**
 * Reset effective status bits for mapping.
 * @param node		mapping node
 * @return effective status bits
 */
static word_t mm_reset_effective_status (mdb_t *self, mdb_node_t *node)
{
    pgent_t *pg = (pgent_t *) mdb_node_get_object (node);
    addr_t vaddr;
    word_t rwx;

    (void) self;
    vaddr = pgent_vaddr (pg, mm_space (node), mm_pgsize (node), node);
    rwx = pgent_reference_bits (pg, mm_space (node), mm_pgsize (node), vaddr);
    pgent_reset_reference_bits (pg, mm_space (node), mm_pgsize (node));
    return rwx;
}

/**
 * Update effective status bits for mapping.
 * @param node		mapping node
 * @param status	new status bits
 */
static void mm_update_effective_status (mdb_t *self, mdb_node_t *node, word_t status)
{
    pgent_t *pg = (pgent_t *) mdb_node_get_object (node);
    (void) self;
    pgent_update_reference_bits (pg, mm_space (node), mm_pgsize (node), status);
}

/**
 * Dump contents of mapping node.
 * @param node		mapping node
 */
static void SECTION(SEC_KDEBUG) mm_dump (mdb_t *self, mdb_node_t *node)
{
    pgent_t *pg;
    space_t *spc;
    word_t psz;
    addr_t vaddr;

    printf ("[%d] ", mdb_node_get_depth (node));

    pg = (pgent_t *) mdb_node_get_object (node);
    spc = mm_space (node);
    psz = mm_pgsize (node);
    vaddr = pgent_vaddr (pg, spc, psz, node);

    printf ("vaddr: %p  spc: %p  ", vaddr, spc);
    printf ("rights: [o=%c%c%c i=%c%c%c e=%c%c%c] (%p)\n",
	    mdb_node_get_outrights (node) & 0x4 ? 'r' : '~',
	    mdb_node_get_outrights (node) & 0x2 ? 'w' : '~',
	    mdb_node_get_outrights (node) & 0x1 ? 'x' : '~',
	    mdb_node_get_inrights (node) & 0x4 ? 'r' : '~',
	    mdb_node_get_inrights (node) & 0x2 ? 'w' : '~',
	    mdb_node_get_inrights (node) & 0x1 ? 'x' : '~',
	    mm_get_rights (self, node) & 0x4 ? 'r' : '~',
	    mm_get_rights (self, node) & 0x2 ? 'w' : '~',
	    mm_get_rights (self, node) & 0x1 ? 'x' : '~', node);
}


/*
 * The ops table -- what the vtable of class mdb_mem_t used to be.
 */
const mdb_ops_t mdb_mem_ops = {
    .get_radix			= mm_get_radix,
    .get_next_objsize		= mm_get_next_objsize,
    .get_name			= mm_get_name,
    .clear			= mm_clear,
    .get_rights			= mm_get_rights,
    .set_rights			= mm_set_rights,
    .flush_cached_entry		= mm_flush_cached_entry,
    .allow_attribute_update	= mm_allow_attribute_update,
    .set_attribute		= mm_set_attribute,
    .get_phys_address		= mm_get_phys_address,
    .get_purged_status		= mm_get_purged_status,
    .reset_purged_status	= mm_reset_purged_status,
    .update_purged_status	= mm_update_purged_status,
    .get_effective_status	= mm_get_effective_status,
    .reset_effective_status	= mm_reset_effective_status,
    .update_effective_status	= mm_update_effective_status,
    .dump			= mm_dump,
};

/**
 * Mapping database for all physical page frames in the system.
 */
mdb_t mdb_mem = { &mdb_mem_ops };

word_t mdb_mem_sizes[] = MDB_MEM_SIZES;
word_t mdb_mem_num_sizes = MDB_MEM_NUMSIZES;


/**
 * Sigma0 memory mapping node.
 */
mdb_node_t * sigma0_memnode;


/**
 * Dummy page rable entry to associate with the sigma0 mapping node.
 * Needed in order to retrieve access rights when mapping from sigma0.
 */
static pgent_t sigma0_pgent;


/**
 * Add buffer allocation sizes.
 */
MDB_INIT_FUNCTION (1, init_mdb_mem_sizes)
{
    void mdb_add_size (word_t size);	/* generic/mapping_alloc.c */
    word_t i;

    for (i = 0; i < mdb_mem_num_sizes; i++)
	mdb_add_size ((1UL << (mdb_mem_sizes[i+1] - mdb_mem_sizes[i])) *
		      sizeof (mdb_tableent_t));
}


/**
 * Initialize the mapping database for memory.  A single mapping node
 * that covers the complete address space is created and owned by
 * sigma0.  All other mappings are derived from this node.
 */
MDB_INIT_FUNCTION (3, init_mdb_mem)
{
    extern space_t * sigma0_space;
    word_t i, j;

    sigma0_memnode = mdb_node_alloc ();
    ASSERT (sigma0_memnode);
    mdb_node_init (sigma0_memnode);

    mdb_node_set_prev (sigma0_memnode, sigma0_memnode);
    mdb_node_set_next (sigma0_memnode, NULL);
    mdb_node_set_depth (sigma0_memnode, 0);
    mdb_node_set_inrights (sigma0_memnode, ~0UL);
    mdb_node_set_outrights (sigma0_memnode, ~0UL);
    mdb_node_set_object (sigma0_memnode, &sigma0_pgent);
    mdb_node_set_objsize (sigma0_memnode, mdb_mem_sizes[mdb_mem_num_sizes]);
    mdb_node_reset_purged_status (sigma0_memnode, &mdb_mem);
    mdb_node_reset_effective_status (sigma0_memnode, &mdb_mem);

    /* Set the access rights for the dummy sigma0 pgent to full
       permissions. */

    pgent_update_rights (&sigma0_pgent, sigma0_space, PGENT_SIZE_MAX + 1, ~0UL);

    /* Sanity checking of page size arrays. */

    for (i = (word_t) 0; i < PGENT_SIZE_MAX; i++)
    {
	if (! is_page_size_valid (i))
	    continue;
	for (j = 0; j < mdb_mem_num_sizes; j++)
	    if (hw_pgshifts[i] == mdb_mem_sizes[j])
		break;
	if (j == mdb_mem_num_sizes)
	    panic ("mdb_mem_sizes[] is not a superset of valid hw_pgshifts[]");
    }
}
