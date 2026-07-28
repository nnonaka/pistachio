/*********************************************************************
 *
 * Copyright (C) 2000-2006, 2010,  Karlsruhe University
 *
 * File path:     generic/mapping.cc
 * Description:   Generic mapping database implementation
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
 ********************************************************************/
#include <debug.h>
#include <mapping.h>
#include <linear_ptab.h>
#include <sync.h>

#include INC_API(tcb.h)
#include INC_ARCH(pgent.h)
#include INC_API(fpage.h)
#include INC_API(space.h)

spinlock_t mdb_lock;

word_t mdb_pgshifts[] = MDB_PGSHIFTS;


// The sigma0_mapnode is initialized to own the whole address space.
static mapnode_t __sigma0_mapnode;
mapnode_t * sigma0_mapnode;

static rootnode_t * mdb_create_roots (word_t size) NOINLINE;
static dualnode_t * mdb_create_dual (mapnode_t * map, rootnode_t * root) NOINLINE;


/* mdb_buflist_init now lives in mapping_alloc.c (C linkage). */
void mdb_buflist_init (void);

/**
 * Initialize mapping database structures
 */
void SECTION (".init") init_mdb (void)
{
    dualnode_t *dual;
    word_t i, j;

    mdb_buflist_init ();

    spinlock_lock (&mdb_lock);

    // Frame table for the complete address space.
    dual = mdb_create_dual (NULL, mdb_create_roots (MDB_PGSIZE_MAX));

    // Let sigma0 own the whole address space.
    sigma0_mapnode = &__sigma0_mapnode;
    mapnode_set_backlink_map (sigma0_mapnode, (mapnode_t *) NULL, (pgent_t *) NULL);
    mapnode_set_space (sigma0_mapnode, (space_t *) 0);
    mapnode_set_depth (sigma0_mapnode, 0);
    mapnode_set_next_dual (sigma0_mapnode, dual);

    /* Sanity checking of pgshift arrays.  This was pgent_t::size_max before the
       flip to C, and briefly became the x86-only X86_PGSIZE_MAX; MDB_PGSIZE_MAX
       is the architecture-neutral spelling, defined in mapping.h. */
    for (i = 0; i < MDB_PGSIZE_MAX; i++)
    {
	if (! is_page_size_valid (i))
	    continue;
	for (j = 0; j < MDB_PGSIZE_MAX; j++)
	    if (hw_pgshifts[i] == mdb_pgshifts[j])
		break;
	if (j == MDB_PGSIZE_MAX)
	    panic ("mdb_pgshifts[] is not a superset of valid hw_pgshifts[]");
    }

    spinlock_unlock (&mdb_lock);
}



/*
 * Helper functions
 */

INLINE word_t mdb_arraysize (word_t pgsize)
{
    return 1 << (mdb_pgshifts[pgsize+1] - mdb_pgshifts[pgsize]);
}

INLINE word_t mdb_get_index (word_t size, addr_t addr)
{
    return ((word_t) addr >> mdb_pgshifts[size]) & (mdb_arraysize(size) - 1);
}

INLINE rootnode_t * mdb_index_root (word_t size, rootnode_t * r, addr_t addr)
{
    return r + mdb_get_index (size, addr);
}

INLINE word_t mdb_pgsize (word_t hw_pgsz)
{
    word_t s = 0;
    while (mdb_pgshifts[s] < hw_pgshifts[hw_pgsz])
	s++;
    return s;
}

INLINE word_t hw_pgsize (word_t mdb_pgsz)
{
    word_t s = 0;
    while (hw_pgshifts[s] < mdb_pgshifts[mdb_pgsz])
	s++;
    return s;
}

static void remove_map_from_map (mapnode_t * pmap, mapnode_t * cmap)
{
    mapnode_t * nmap = mapnode_get_nextmap (cmap);

    if (mapnode_is_next_map (pmap))
	mapnode_set_next_map (pmap, nmap);
    else
    {
	dualnode_t * dual = mapnode_get_nextdual (pmap);
	if (nmap)
	    dual->map = nmap;
	else
	{
	    // No more mappings, remove dual node
	    mapnode_set_next_root (pmap, dual->root);
	    mdb_free_buffer ((addr_t) dual, sizeof (dualnode_t));
	}
    }

    mdb_free_buffer ((addr_t) cmap, sizeof (mapnode_t));

    if (nmap)
	mapnode_set_backlink_map (nmap, pmap, mapnode_get_pgent (nmap, cmap));
}

static void NOINLINE remove_map_from_root (rootnode_t * proot, mapnode_t * cmap)
{
    mapnode_t * nmap = mapnode_get_nextmap (cmap);

    if (rootnode_is_next_map (proot))
	rootnode_set_ptr_map (proot, nmap);
    else
    {
	dualnode_t * dual = rootnode_get_dual (proot);
	if (nmap)
	    dual->map = nmap;
	else
	{
	    // No more mappings, remove dual node
	    rootnode_set_ptr_root (proot, dual->root);
	    mdb_free_buffer ((addr_t) dual, sizeof (dualnode_t));
	}
    }

    mdb_free_buffer ((addr_t) cmap, sizeof (mapnode_t));

    if (nmap)
	mapnode_set_backlink_root (nmap, proot, mapnode_get_pgent (nmap, cmap));
}


/**
 * Inserts mapping into mapping database.  See generic/mapping.cc history for
 * the parameter documentation.  mdb_map/mdb_flush are file-static (the C
 * bridge below, mdb_map_c/mdb_flush_c, is the only external entry point).
 */
static mapnode_t * mdb_map (mapnode_t * f_map, pgent_t * f_pg,
			    word_t f_hwpgsize, addr_t f_addr,
			    pgent_t * t_pg, word_t t_hwpgsize,
			    space_t * t_space, bool grant)
{
    rootnode_t *root, *proot;
    mapnode_t *nmap;

    spinlock_lock (&mdb_lock);

    // Grant operations simply reuse the old mapping node
    if (grant)
    {
	if (mapnode_is_prev_root (f_map))
	    mapnode_set_backlink_root (f_map, mapnode_get_prevroot (f_map, f_pg), t_pg);
	else
	    mapnode_set_backlink_map (f_map, mapnode_get_prevmap (f_map, f_pg), t_pg);

	mapnode_set_space (f_map, t_space);
	spinlock_unlock (&mdb_lock);
	return f_map;
    }

    // Convert to mapping database pagesizes
    word_t f_pgsize = mdb_pgsize (f_hwpgsize);
    word_t t_pgsize = mdb_pgsize (t_hwpgsize);

    mapnode_t * newmap = (mapnode_t *) mdb_alloc_buffer (sizeof (mapnode_t));

    if (f_pgsize == t_pgsize)
    {
	// Hook new node directly below mapping node
	nmap = mapnode_get_nextmap (f_map);
	mapnode_set_backlink_map (newmap, f_map, t_pg);
	mapnode_set_space (newmap, t_space);
	mapnode_set_depth (newmap, mapnode_get_depth (f_map) + 1);
	mapnode_set_next_map (newmap, nmap);

	// Fixup prev->next pointer
	if (mapnode_is_next_root (f_map))
	    mapnode_set_next_dual (f_map, mdb_create_dual (newmap, mapnode_get_nextroot (f_map)));
	else if (mapnode_is_next_both (f_map))
	    mapnode_get_nextdual (f_map)->map = newmap;
	else
	    mapnode_set_next_map (f_map, newmap);

	// Fixup next->prev pointer
	if (nmap)
	    mapnode_set_backlink_map (nmap, newmap, mapnode_get_pgent (nmap, f_map));

	spinlock_unlock (&mdb_lock);
	return newmap;
    }

    root = NULL;

    while (f_pgsize > t_pgsize)
    {
	// Need to traverse into subtrees
	f_pgsize--;
	proot = root;

	if (proot)
	{
	    nmap = rootnode_get_map (proot);
	    root = rootnode_get_root (proot);
	}
	else
	{
	    // This is for 1st iteration only
	    nmap = mapnode_get_nextmap (f_map);
	    root = mapnode_get_nextroot (f_map);
	}

	if (! root)
	{
	    // New array needs to be created
	    root = mdb_create_roots (f_pgsize);

	    if (proot)
		// Insert below previous root node
		if (nmap)
		    rootnode_set_ptr_dual (proot, mdb_create_dual (nmap, root));
		else
		    rootnode_set_ptr_root (proot, root);
	    else
		// Insert below original mapping node
		if (nmap)
		    mapnode_set_next_dual (f_map, mdb_create_dual (nmap, root));
		else
		    mapnode_set_next_root (f_map, root);
	}

	// Traverse into subtree
	root = mdb_index_root (f_pgsize, root, f_addr);
    }

    // Insert mapping below root node
    nmap = rootnode_get_map (root);
    mapnode_set_backlink_root (newmap, root, t_pg);
    mapnode_set_space (newmap, t_space);
    mapnode_set_depth (newmap, mapnode_get_depth (f_map) + 1);
    mapnode_set_next_map (newmap, rootnode_get_map (root));

    // Fixup root->next pointer
    if (rootnode_is_next_root (root))
	rootnode_set_ptr_dual (root, mdb_create_dual (newmap, rootnode_get_root (root)));
    else if (rootnode_is_next_both (root))
	rootnode_get_dual (root)->map = newmap;
    else
	rootnode_set_ptr_map (root, newmap);

    // Fixup next->prev pointer
    if (nmap)
	mapnode_set_backlink_root (nmap, root, mapnode_get_pgent (nmap, root));

    spinlock_unlock (&mdb_lock);
    return newmap;
}



/**
 * Flush mapping recursively from mapping database.  Recursively revokes access
 * attributes of the indicated mapping; if all rights are revoked the mapping is
 * removed.  Returns the logical OR of all involved mappings' access attributes.
 */
static word_t mdb_flush (mapnode_t * f_map, pgent_t * f_pg,
			 word_t f_hwpgsize, addr_t f_addr,
			 word_t t_hwpgsize, fpage_t fp, bool unmap_self)
{
    dualnode_t *dual;
    mapnode_t *pmap, *nmap;
    rootnode_t *root, *proot, *nroot;
    word_t rcnt, startdepth, rwx;
    addr_t vaddr;
    space_t *space, *parent_space;
    pgent_t *parent_pg;
    word_t parent_pgsize;

    mapnode_t * r_nmap[MDB_PGSIZE_MAX];
    rootnode_t * r_root[MDB_PGSIZE_MAX];
    word_t  r_rcnt[MDB_PGSIZE_MAX];
    word_t  r_prev[MDB_PGSIZE_MAX];	/* Bit 0 set indicates mapping.
					   Bit 0 cleared indicates root. */
    rcnt = 0;
    startdepth = mapnode_get_depth (f_map);
    root = NULL;

    if (unmap_self)
    {
	// Read and reset the reference bits stored in the mapping node.
	space = mapnode_get_space (f_map);
	vaddr = pgent_vaddr (f_pg, space, f_hwpgsize, f_map);

	rwx = mapnode_get_rwx (f_map) |
	    pgent_reference_bits (f_pg, space, f_hwpgsize, vaddr);
	mapnode_set_rwx (f_map, 0);
	pgent_reset_reference_bits (f_pg, space, f_hwpgsize);

	parent_pg = NULL;
    }
    else
    {
	// Record the reference bits for the whole subtree in the pgent.
	rwx = 0;
	parent_pg = f_pg;
	parent_space = mapnode_get_space (f_map);
	parent_pgsize = f_hwpgsize;
    }

    // Convert to mapping database pagesizes
    word_t f_pgsize = mdb_pgsize (f_hwpgsize);
    word_t t_pgsize = mdb_pgsize (t_hwpgsize);

    spinlock_lock (&mdb_lock);

    do {
	// Variables `f_map' and `f_pg' are valid at this point

	dual  = mapnode_get_nextdual (f_map);
	nroot = mapnode_get_nextroot (f_map);
	nmap  = mapnode_get_nextmap (f_map);
	pmap  = mapnode_get_prevmap (f_map, f_pg);
	proot = mapnode_get_prevroot (f_map, f_pg);
	space = mapnode_get_space (f_map);

	f_hwpgsize = hw_pgsize (f_pgsize);

	vaddr = pgent_vaddr (f_pg, space, f_hwpgsize, f_map);

	if (unmap_self)
	{
	    // Update reference bits
	    mapnode_update_rwx (f_map,
		pgent_reference_bits (f_pg, space, f_hwpgsize, vaddr));
	    rwx |= pgent_reference_bits (f_pg, space, f_hwpgsize, vaddr);

	    ASSERT (f_pgsize <= t_pgsize);

	    if (fpage_is_rwx (&fp))
	    {
		// Revoke all access rights (i.e., remove node)
		if (pmap)
		    remove_map_from_map (pmap, f_map);
		else
		    remove_map_from_root (proot, f_map);
		pgent_clear (f_pg, space, f_hwpgsize, false, vaddr);
	    }
	    else
	    {
		// Revoke access rights
		pgent_revoke_rights (f_pg, space, f_hwpgsize, fpage_get_rwx (&fp));
		pgent_reset_reference_bits (f_pg, space, f_hwpgsize);
		pgent_flush (f_pg, space, f_hwpgsize, false, vaddr);
		pmap = f_map;
		proot = NULL;
	    }

	    // We might have to flush some TLB entries
	    if (! space_does_tlbflush_pay (fpage_get_size_log2 (&fp)))
		space_flush_tlbent (space, get_current_space_c (), vaddr,
				    page_shift (f_hwpgsize));
	}
	else
	{
	    pgent_reset_reference_bits (f_pg, space, f_hwpgsize);
	    pmap = f_map;
	    proot = NULL;
	}
	f_map = NULL;

	// Variables `f_map' and `f_pg' are no longer valid here

	if (nroot)
	{
	    f_pgsize--;

	    if (f_pgsize < t_pgsize)
	    {
		// Recurse into subarray before checking mappings
		ASSERT (f_pgsize < MDB_PGSIZE_MAX);
		r_prev[f_pgsize] = pmap ? (word_t) pmap | 1 : (word_t) proot;
		r_nmap[f_pgsize] = nmap;
		r_root[f_pgsize] = root;
		r_rcnt[f_pgsize] = rcnt;

		root = nroot - 1;
		rcnt = mdb_arraysize (f_pgsize);

		if (dual && fpage_is_rwx (&fp) && unmap_self)
		    mdb_free_buffer ((addr_t) dual, sizeof (dualnode_t));
	    }
	    else
	    {
		// We may use the virtual address f_addr for indexing
		// here since the alignment within the page will be
		// the same as with the physical address.
		root = mdb_index_root (f_pgsize, nroot, f_addr) - 1;
		rcnt = 1;
	    }
	}
	else
	{
	    if (nmap)
	    {
		if (pmap)
		    f_pg = mapnode_get_pgent (nmap, pmap);
		else
		    f_pg = mapnode_get_pgent (nmap, proot);
		f_map = nmap;
	    }
	    else if ((f_pgsize < t_pgsize)  && root)
	    {
		// Recurse up from subarray
		if (fpage_is_rwx (&fp))
		{
		    // Revoke all access rights (i.e., remove subtree)
		    root -= mdb_arraysize (f_pgsize) - 1;
		    mdb_free_buffer ((addr_t) root,
				     mdb_arraysize (f_pgsize) *
				     sizeof (rootnode_t));
		}

		ASSERT (f_pgsize < MDB_PGSIZE_MAX);
		f_map = r_nmap[f_pgsize];
		root  = r_root[f_pgsize];
		rcnt  = r_rcnt[f_pgsize];
		if (r_prev[f_pgsize] & 1)
		{
		    proot = NULL;
		    pmap = (mapnode_t *) (r_prev[f_pgsize] & ~1UL);
		    if (f_map)
			f_pg = mapnode_get_pgent (f_map, pmap);
		}
		else
		{
		    proot = (rootnode_t *) r_prev[f_pgsize];
		    pmap = NULL;
		    if (f_map)
			f_pg = mapnode_get_pgent (f_map, proot);
		}

		f_pgsize++;
	    }
	}

	// If f_map now is non-nil, the variables f_map, f_pg, pmap
	// and proot will all be valid.  Otherwise, root and rcnt will
	// be valid.

	while ((! f_map) && (rcnt > 0))
	{
	    rcnt--;
	    root++;

	    dual  = rootnode_get_dual (root);
	    nroot = rootnode_get_root (root);
	    f_map = rootnode_get_map (root);

	    if (nroot)
	    {
		// Recurse into subarray before checking mappings
		f_pgsize--;

		if (fpage_is_rwx (&fp))
		    rootnode_set_ptr_map (root, f_map); // Remove subarray

		ASSERT (f_pgsize < MDB_PGSIZE_MAX);
		r_prev[f_pgsize] = (word_t) root;
		r_nmap[f_pgsize] = f_map;
		r_root[f_pgsize] = root;
		r_rcnt[f_pgsize] = rcnt;

		f_map = NULL;
		root = nroot - 1;
		rcnt = mdb_arraysize (f_pgsize);

		if (dual && fpage_is_rwx (&fp))
		    mdb_free_buffer ((addr_t) dual, sizeof (dualnode_t));
	    }
	    else
	    {
		if (f_map)
		{
		    f_pg = mapnode_get_pgent (f_map, root);
		    pmap = NULL;
		    proot = root;
		}
	    }
	}

	if (f_pgsize <= t_pgsize)
	    // From now on we will unmap all nodes
	    unmap_self = true;

    } while (f_map && mapnode_get_depth (f_map) > startdepth);

    // Update the reference bits in the page table entry of the parent mapping.
    // XXX: Handle the case where one does flush instead of unmap.
    if (parent_pg)
	pgent_update_reference_bits (parent_pg, parent_space, parent_pgsize, rwx);

    spinlock_unlock (&mdb_lock);

    return rwx;
}


/**
 * Create root array.  Allocates and initializes a new root array of the given
 * page size, returning a pointer to it.
 */
static NOINLINE rootnode_t * mdb_create_roots (word_t size)
{
    rootnode_t *newnodes, *n;

    word_t num = mdb_arraysize (size);
    newnodes = (rootnode_t *) mdb_alloc_buffer (sizeof (rootnode_t) * num);

    for (n = newnodes; num--; n++)
	rootnode_set_ptr_map (n, (mapnode_t *) NULL);

    return newnodes;
}


/**
 * Create a dual node holding the given map and root pointers.
 */
static NOINLINE dualnode_t * mdb_create_dual (mapnode_t * map, rootnode_t * root)
{
    dualnode_t * dual = (dualnode_t *) mdb_alloc_buffer (sizeof (dualnode_t));
    dual->map = map;
    dual->root = root;
    return dual;
}


/* C-linkage entry points for the mapping-database, so linear_ptab_walker.c can
   call them (declared in mapping.h).  fpage_t passes by pointer. */
BEGIN_DECLS
mapnode_t * mdb_map_c (mapnode_t * f_map, pgent_t * f_pg, word_t f_hwpgsize,
		       addr_t f_addr, pgent_t * t_pg, word_t t_hwpgsize,
		       space_t * t_space, bool grant)
{
    return mdb_map (f_map, f_pg, f_hwpgsize, f_addr,
		    t_pg, t_hwpgsize, t_space, grant);
}

word_t mdb_flush_c (mapnode_t * f_map, pgent_t * f_pg, word_t f_hwpgsize,
		    addr_t f_addr, word_t t_hwpgsize, fpage_t * fp, bool unmap_self)
{
    return mdb_flush (f_map, f_pg, f_hwpgsize, f_addr,
		      t_hwpgsize, *fp, unmap_self);
}
END_DECLS
