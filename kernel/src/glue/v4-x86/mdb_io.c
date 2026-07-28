/*********************************************************************
 *                
 * Copyright (C) 2005-2008,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/mdb_io.cc
 * Description:   IO port specific generic mappings database functions
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
 * $Id: mdb_io.cc,v 1.5 2007/01/08 14:08:11 skoglund Exp $
 *                
 ********************************************************************/
#include <vrt.h>
#include INC_GLUE(mdb_io.h)
#include INC_GLUE(io_space.h)


/* forward decls for the ops table */
static word_t	    mio_get_radix (mdb_t *self, word_t objsize);
static word_t	    mio_get_next_objsize (mdb_t *self, word_t objsize);
static const char * mio_get_name (mdb_t *self);
static void	    mio_clear (mdb_t *self, mdb_node_t *node);
static word_t	    mio_get_rights (mdb_t *self, mdb_node_t *node);
static void	    mio_set_rights (mdb_t *self, mdb_node_t *node, word_t r);
static void	    mio_flush_cached_entry (mdb_t *self, mdb_node_t *node, mdb_range_t range);
static bool	    mio_allow_attribute_update (mdb_t *self, mdb_node_t *node);
static void	    mio_set_attribute (mdb_t *self, mdb_node_t *node, word_t attrib);
static word_t	    mio_get_phys_address (mdb_t *self, mdb_node_t *node);
static word_t	    mio_get_purged_status (mdb_t *self, mdb_node_t *node);
static void	    mio_reset_purged_status (mdb_t *self, mdb_node_t *node);
static void	    mio_update_purged_status (mdb_t *self, mdb_node_t *node, word_t status);
static word_t	    mio_get_effective_status (mdb_t *self, mdb_node_t *node);
static word_t	    mio_reset_effective_status (mdb_t *self, mdb_node_t *node);
static void	    mio_update_effective_status (mdb_t *self, mdb_node_t *node, word_t status);
static void	    mio_dump (mdb_t *self, mdb_node_t *node);

const mdb_ops_t mdb_io_ops = {
    .get_radix			= mio_get_radix,
    .get_next_objsize		= mio_get_next_objsize,
    .get_name			= mio_get_name,
    .clear			= mio_clear,
    .get_rights			= mio_get_rights,
    .set_rights			= mio_set_rights,
    .flush_cached_entry		= mio_flush_cached_entry,
    .allow_attribute_update	= mio_allow_attribute_update,
    .set_attribute		= mio_set_attribute,
    .get_phys_address		= mio_get_phys_address,
    .get_purged_status		= mio_get_purged_status,
    .reset_purged_status	= mio_reset_purged_status,
    .update_purged_status	= mio_update_purged_status,
    .get_effective_status	= mio_get_effective_status,
    .reset_effective_status	= mio_reset_effective_status,
    .update_effective_status	= mio_update_effective_status,
    .dump			= mio_dump,
};

/**
 * Mapping database for all IO ports in the system.
 */
mdb_t mdb_io = { &mdb_io_ops };

word_t mdb_io_sizes[] = MDB_IO_SIZES;
word_t mdb_io_num_sizes = MDB_IO_NUMSIZES;


/**
 * Sigma0 IO port mapping node.
 */
mdb_node_t * sigma0_ionode;


/**
 * Dummy VRT entry to associate with the sigma0 mapping node.  Needed
 * in order to retrieve access rights when mapping from sigma0.
*/
static vrt_node_t sigma0_ioent;


/**
 * Add buffer allocation sizes.
 */
MDB_INIT_FUNCTION (1, init_mdb_io_sizes)
{
    void mdb_add_size (word_t size);	/* generic/mapping_alloc.c */
    word_t i;

    for (i = 0; i < mdb_io_num_sizes; i++)
	mdb_add_size ((1UL << (mdb_io_sizes[i+1] - mdb_io_sizes[i])) *
		      sizeof (mdb_tableent_t));
}


/**
 * Initialize the mapping database for IO ports.  A single mapping node
 * that covers the complete address space is created and owned by
 * sigma0.  All other mappings are derived from this node.
 */
MDB_INIT_FUNCTION (3, init_mdb_io)
{
    word_t i, j;

    sigma0_ionode = mdb_node_alloc ();
    mdb_node_init (sigma0_ionode);

    mdb_node_set_prev (sigma0_ionode, sigma0_ionode);
    mdb_node_set_next (sigma0_ionode, NULL);
    mdb_node_set_depth (sigma0_ionode, 0);
    mdb_node_set_inrights (sigma0_ionode, ~0UL);
    mdb_node_set_outrights (sigma0_ionode, ~0UL);
    mdb_node_set_object (sigma0_ionode, &sigma0_ioent);
    mdb_node_set_objsize (sigma0_ionode, mdb_io_sizes[mdb_io_num_sizes]);

    /* Set the access rights for the dummy sigma0 pgent to full permissions. */

    vrt_io_set_rights (&sigma0_ioent, VRT_IO_FULLRIGHTS);

    /* Sanity checking of object sizes. */

    for (i = 0; i < vrt_io_num_sizes; i++)
    {
	for (j = 0; j < mdb_io_num_sizes; j++)
	    if (vrt_io_sizes[i] == mdb_io_sizes[j])
		break;
	if (j == mdb_io_num_sizes)
	    panic ("mdb_io_sizes[] is not a superset of vrt_io_sizes[]");
    }
}


/*
 * MDB specific functions.
 */

static word_t mio_get_radix (mdb_t *self, word_t objsize)
{
    word_t k;

    (void) self;
    ASSERT (objsize < mdb_io_sizes[mdb_io_num_sizes]);

    for (k = 0; objsize >= mdb_io_sizes[k]; k++) {}
    return mdb_io_sizes[k] - objsize;
}

static word_t mio_get_next_objsize (mdb_t *self, word_t objsize)
{
    word_t k;

    (void) self;
    ASSERT (objsize > mdb_io_sizes[0]);

    for (k = mdb_io_num_sizes - 1; objsize <= mdb_io_sizes[k]; k--) {}
    return mdb_io_sizes[k];
}

static const char * SECTION(SEC_KDEBUG) mio_get_name (mdb_t *self)
{
    (void) self;
    return "io";
}


/*
 * MDB specific mapping node functions.
 */

static void mio_clear (mdb_t *self, mdb_node_t *node)
{
    vrt_node_t *n = (vrt_node_t *) mdb_node_get_object (node);

    (void) self;
    set_io_bitmap ((space_t *) mdb_node_get_misc (node),
		   vrt_io_get_port (vrt_node_get_object (n)),
		   mdb_node_get_objsize (node));
    vrt_node_clear (n);
}

static word_t mio_get_rights (mdb_t *self, mdb_node_t *node)
{
    vrt_node_t *n = (vrt_node_t *) mdb_node_get_object (node);
    (void) self;
    return vrt_io_get_rights (vrt_node_get_object (n));
}

static void mio_set_rights (mdb_t *self, mdb_node_t *node, word_t r)
{
    vrt_node_t *n = (vrt_node_t *) mdb_node_get_object (node);
    (void) self;
    vrt_io_set_rights (n, r);
}

static void mio_flush_cached_entry (mdb_t *self, mdb_node_t *node, mdb_range_t range)
{
    (void) self; (void) node; (void) range;
}

static bool mio_allow_attribute_update (mdb_t *self, mdb_node_t *node)
{
    (void) self; (void) node;
    return false;
}

static void mio_set_attribute (mdb_t *self, mdb_node_t *node, word_t attrib)
{
    (void) self; (void) node; (void) attrib;
}

/**
 * Get physical address of mapping.  The physical address is the
 * global IO port number.
 */
static word_t mio_get_phys_address (mdb_t *self, mdb_node_t *node)
{
    vrt_node_t *n = (vrt_node_t *) mdb_node_get_object (node);
    (void) self;
    return vrt_io_get_port (vrt_node_get_object (n));
}

static word_t mio_get_purged_status (mdb_t *self, mdb_node_t *node)
{
    (void) self; (void) node;
    return 0;
}

static void mio_reset_purged_status (mdb_t *self, mdb_node_t *node)
{
    (void) self; (void) node;
}

static void mio_update_purged_status (mdb_t *self, mdb_node_t *node, word_t status)
{
    (void) self; (void) node; (void) status;
}

static word_t mio_get_effective_status (mdb_t *self, mdb_node_t *node)
{
    (void) self; (void) node;
    return 0;
}

static word_t mio_reset_effective_status (mdb_t *self, mdb_node_t *node)
{
    (void) self; (void) node;
    return 0;
}

static void mio_update_effective_status (mdb_t *self, mdb_node_t *node, word_t status)
{
    (void) self; (void) node; (void) status;
}

static void mio_dump (mdb_t *self, mdb_node_t *node)
{
    vrt_node_t *n = (vrt_node_t *) mdb_node_get_object (node);
    word_t value = vrt_node_get_object (n);

    (void) self;
    printf ("[%d] ", mdb_node_get_depth (node));
    printf ("port: %04x-%04x, space: %p\n",
	    vrt_io_get_port (value),
	    vrt_io_get_port (value) + (1UL << mdb_node_get_objsize (node)) - 1,
	    mdb_node_get_misc (node));
}
