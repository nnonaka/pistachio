/*********************************************************************
 *                
 * Copyright (C) 2005-2008, 2010,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/vrt_io.cc
 * Description:   VRT for thread objects
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
 * $Id: vrt_io.cc,v 1.4 2006/06/12 17:10:26 skoglund Exp $
 *                
 ********************************************************************/
#include INC_API(tcb.h)
#include INC_GLUE(vrt_io.h)
#include INC_GLUE(io_space.h)

#include <kdb/tracepoints.h>
#include <debug.h>

word_t vrt_io_sizes[] = VRT_IO_SIZES;
word_t vrt_io_num_sizes = VRT_IO_NUMSIZES;

/* forward decls for the ops table */
static word_t	    vio_get_radix (vrt_t *self, word_t objsize);
static word_t	    vio_get_next_objsize (vrt_t *self, word_t objsize);
static word_t	    vio_get_vrt_size (vrt_t *self);
static mdb_t *	    vio_get_mapdb (vrt_t *self);
static const char * vio_get_name (vrt_t *self);
static void	    vio_set_object (vrt_t *self, vrt_node_t *n, word_t n_sz,
				    word_t paddr, vrt_node_t *o, word_t o_sz,
				    word_t access);
static word_t	    vio_get_address (vrt_t *self, vrt_node_t *n);
static word_t	    vio_make_misc (vrt_t *self, vrt_node_t *obj, mdb_node_t *map);
static void	    vio_dump (vrt_t *self, vrt_node_t *n);

const vrt_ops_t vrt_io_ops = {
    .get_radix		= vio_get_radix,
    .get_next_objsize	= vio_get_next_objsize,
    .get_vrt_size	= vio_get_vrt_size,
    .get_mapdb		= vio_get_mapdb,
    .get_name		= vio_get_name,
    .set_object		= vio_set_object,
    .get_address	= vio_get_address,
    .make_misc		= vio_make_misc,
    .dump		= vio_dump,
};


/**
 * Add buffer allocation sizes.
 */
MDB_INIT_FUNCTION (1, init_vrt_io_sizes)
{
    void mdb_add_size (word_t size);	/* generic/mapping_alloc.c */
    word_t i;

    for (i = 0; i < vrt_io_num_sizes; i++)
	mdb_add_size ((1UL << (vrt_io_sizes[i+1] - vrt_io_sizes[i])) *
		      (sizeof (vrt_node_t) + sizeof (mdb_node_t *)));
    mdb_add_size (sizeof (vrt_io_t));
}


/**
 * Allocate new VRT structure for implementing IO space.
 * Was vrt_io_t::operator new.
 *
 * @return new VRT for thread space
 */
vrt_io_t * vrt_io_alloc (void)
{
    vrt_io_t *vrt = (vrt_io_t *) mdb_alloc_buffer (sizeof (vrt_io_t));
    vrt_table_t *table;

    vrt->base.ops = &vrt_io_ops;

    /* Allocate and initialize root table.  The C++ built a throwaway
       vrt_io_t just to reach get_radix(); with an ops table the function is
       reachable directly. */

    table = vrt_table_alloc (vio_get_radix (&vrt->base,
					    vrt_io_sizes[vrt_io_num_sizes - 1]));
    vrt_table_set_prefix (table, 0);
    vrt_table_set_objsize (table, vrt_io_sizes[vrt_io_num_sizes - 1]);
    vrt_set_table (&vrt->base, table);

    vrt_io_init (vrt);
    return vrt;
}

/**
 * Delete the VRT structure.  Was vrt_io_t::operator delete.
 *
 * NB: the original freed sizeof (mdb_node_t), not sizeof (vrt_io_t).
 * Preserved -- see notes §114.
 */
void vrt_io_free (vrt_io_t *v)
{
    mdb_free_buffer (v, sizeof (mdb_node_t));
}

/**
 * Initialize the VRT for the IO space.
 */
void vrt_io_init (vrt_io_t *self)
{
#if defined(CONFIG_DEBUG)
    /* Initialize name to "io<address>". */

    word_t idx = 4 + sizeof (word_t) * 2;
    word_t num = (word_t) self;

    self->name[0] = 'i'; self->name[1] = 'o'; self->name[2] = '<';
    self->name[idx--] = 0; self->name[idx--] = '>';
    while (idx > 2)
    {
	self->name[idx--] = (num & 0xf) > 9 ?
	    (char) ((num & 0xf) - 10 + 'a') : (char) ((num & 0xf) + '0');
	num >>= 4;
    }
#endif

    self->count = 0;
}

/**
 * Populate complete space with idempotent mappings.  We don't care
 * about allocating mapping nodes for all the entries.  We just use
 * the root sigma0 mapping node for all entries.  The mapping database
 * will allocate entries on demand.
 */
void SECTION(".init") vrt_io_populate_sigma0 (vrt_io_t *self)
{
    vrt_table_t *t = vrt_get_table (&self->base);
    vrt_node_t *n = vrt_table_get_node (t, 0);
    word_t addr = 0;
    word_t k;

    for (k = 0; k < (1UL << vrt_table_get_radix (t)); k++, n++)
    {
	vrt_node_set_object_raw (n, (addr & 0xffff) | (1UL << 16));
	vrt_table_set_mapnode (t, addr, sigma0_ionode);
	addr += (1UL << vrt_table_get_objsize (t));
    }
}

/**
 * Get radix to use for table.
 * @param objsize	size of objects in table
 * @return radix to use for table
 */
static word_t vio_get_radix (vrt_t *self, word_t objsize)
{
    word_t k;

    (void) self;
    ASSERT (objsize < vrt_io_sizes[vrt_io_num_sizes]);

    for (k = 0; objsize >= vrt_io_sizes[k]; k++) {}
    return vrt_io_sizes[k] - objsize;
}

/**
 * Get the next smaller valid object size.
 * @param objsize	size of object
 * @return size of next smaller object size
 */
static word_t vio_get_next_objsize (vrt_t *self, word_t objsize)
{
    word_t k;

    (void) self;
    ASSERT (objsize > vrt_io_sizes[0]);

    for (k = vrt_io_num_sizes - 1; objsize <= vrt_io_sizes[k]; k--) {}
    return vrt_io_sizes[k];
}

/**
 * Get size of VRT space.
 * @return log2 size of VRT space.
 */
static word_t vio_get_vrt_size (vrt_t *self)
{
    (void) self;
    return vrt_io_sizes[vrt_io_num_sizes];
}

/**
 * Get mapping database associated with VRT.
 * @return mappings database associated with VRT
 */
static mdb_t * vio_get_mapdb (vrt_t *self)
{
    (void) self;
    return &mdb_io;
}

/**
 * Get name of table structure.
 * @reurn name of table structure
 */
static const char * vio_get_name (vrt_t *self)
{
#if defined(CONFIG_DEBUG)
    return ((vrt_io_t *) self)->name;
#else
    (void) self;
    return "";
#endif
}

/**
 * Copy IO port object and set valid bit.
 * @param n		destination object
 * @param paddr		physical address
 * @param o		source object
 * @param access	access rights
 */
static void vio_set_object (vrt_t *self, vrt_node_t *n, word_t n_sz, word_t paddr,
			    vrt_node_t *o, word_t o_sz, word_t access)
{
    (void) o; (void) o_sz; (void) access;
    vrt_node_set_object_raw (n, (paddr & 0xffff) | (1UL << 16));
    zero_io_bitmap (vrt_io_get_space ((vrt_io_t *) self), (paddr & 0xffff), n_sz);
}

/**
 * Get address (IO port) of object.
 * @param n		destination object
 * @return global port number of thread object
 */
static word_t vio_get_address (vrt_t *self, vrt_node_t *n)
{
    (void) self;
    return vrt_io_get_port (vrt_node_get_object (n));
}

/**
 * Dump VRT node information
 * @param n		destination object
 */
static void vio_dump (vrt_t *self, vrt_node_t *n)
{
    word_t value = vrt_node_get_object (n);
    (void) self;
    printf ("port: %04x\n", vrt_io_get_port (value));
}

/**
 * Create mapping node misc contents.
 * @param obj		vrt object
 * @param map		mapping node
 * @return pointer to current space
 */
static word_t vio_make_misc (vrt_t *self, vrt_node_t *obj, mdb_node_t *map)
{
    (void) obj; (void) map;
    return (word_t) vrt_io_get_space ((vrt_io_t *) self);
}
