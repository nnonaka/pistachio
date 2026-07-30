/*********************************************************************
 *
 * Copyright (C) 2002-2010,  Karlsruhe University
 *
 * File path:     generic/linear_ptab_walker.cc
 * Description:   Linear page table manipulation
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
#ifndef __GENERIC__LINEAR_PTAB_WALKER_CC__
#define __GENERIC__LINEAR_PTAB_WALKER_CC__

#include INC_ARCH(pgent.h)
#include INC_API(fpage.h)
#include INC_API(tcb.h)
#include INC_API(space.h)

#include <kdb/tracepoints.h>
#include <linear_ptab.h>
#include <mdb.h>

#if !defined(CONFIG_NEW_MDB)
#include <mapping.h>
#else
#include <mdb_mem.h>
#endif


DECLARE_TRACEPOINT (FPAGE_MAP);
DECLARE_TRACEPOINT (FPAGE_OVERMAP);
DECLARE_TRACEPOINT (FPAGE_MAPCTRL);
#if !defined(CONFIG_NEW_MDB)
DECLARE_TRACEPOINT (MDB_MAP);
DECLARE_TRACEPOINT (MDB_UNMAP);
#endif

/*
 * arch/x86/pgent.h aliases mapnode_t to mdb_node_t for its own declarations and
 * undefines it again at the end, because generic/mapping.h defines a real
 * struct mapnode_t for the *old* mapping database and the two must not collide.
 * Files that define those pgent functions, or hold the type in locals, have to
 * re-establish the alias for themselves -- which is what the C++
 * linear_ptab_walker.cc did.  Notes §120.
 */
#if defined(CONFIG_NEW_MDB)
#define mapnode_t mdb_node_t
#endif

DECLARE_KMEM_GROUP (kmem_pgtab);

word_t hw_pgshifts[] = HW_PGSHIFTS;


/*
 * Helper functions.
 */

static inline word_t dbg_pgsize (word_t sz)
{
    return (sz >= GB (1) ? sz >> 30 : sz >= MB (1) ? sz >> 20 : sz >> 10);
}

static inline char dbg_szname (word_t sz)
{
    return (sz >= GB (1) ? 'G' : sz >= MB (1) ? 'M' : 'K');
}

/* C reimplementation of mdb_ctrl_t::string() (C++-only) for the mapctrl
   tracepoint below; the ctrl bitfields are C-visible. */
static inline char * dbg_ctrl_string (mdb_ctrl_t ctrl)
{
    static char s[7] = "~~~~~~";
    s[0] = ctrl.set_attribute  ? 'm' : '~';
    s[1] = ctrl.deliver_status ? 'd' : '~';
    s[2] = ctrl.reset_status   ? 'r' : '~';
    s[3] = ctrl.set_rights     ? 'p' : '~';
    s[4] = ctrl.unmap          ? 'u' : '~';
    s[5] = ctrl.mapctrl_self   ? 'c' : '~';
    return s;
}


/**
 * Map fpage to another address space.  If mapping is a grant
 * operation the mappings in the current address space are
 * subsequently removed.  The access bits in the send fpage indicate
 * which access rights the mappings in the destination address space
 * should have (access bits in the send fpage are first bitwise and'ed
 * with access bits of existing source mappings).
 *
 * @param snd_fp	fpage to map
 * @param base		send base
 * @param t_space	destination address space
 * @param rcv_fp	receive window in destination space
 * @param grant		is mapping a grant operation
 */
void space_map_fpage (space_t * self, fpage_t snd_fp, word_t base,
		      space_t * t_space, fpage_t rcv_fp,
		      bool grant)
{
    word_t offset, f_num, t_num, f_off;
    pgent_t *fpg, *tpg;
    word_t f_size, t_size, pgsize;
    struct mapnode_t *newmap, *map = NULL;
    addr_t f_addr, t_addr;

    pgent_t * r_fpg[PGENT_SIZE_MAX];
    pgent_t * r_tpg[PGENT_SIZE_MAX];
    word_t r_fnum[PGENT_SIZE_MAX];
    word_t r_tnum[PGENT_SIZE_MAX];

    /* See the original linear_ptab_walker.cc for the full description of this
       recursion-without-function-calls mapping algorithm. */

    TRACEPOINT (FPAGE_MAP,"%s_fpage (f_spc=%p  f_fp=%p  t_spc=%p  t_fp=%p)\n",
		grant ? "grant" : "map", self, snd_fp.raw, t_space, rcv_fp.raw);

    /* Since get_size returns 1 for nil fpages, and get_size_log2 cannot
       return a useful value anyway, check for nil fpages at first. */
    if (fpage_is_nil_fpage (&snd_fp) || fpage_is_nil_fpage (&rcv_fp))
	return;

    /*
     * Calculate the actual send and receive address to use.
     */

    if (fpage_get_size_log2 (&snd_fp) <= fpage_get_size_log2 (&rcv_fp))
    {
	f_num = fpage_get_size_log2 (&snd_fp);
	base &= fpage_base_mask (rcv_fp, f_num);
	f_addr = fpage_address (snd_fp, f_num);
	t_addr = addr_offset (addr_mask (fpage_address (rcv_fp, f_num),
					 ~fpage_base_mask (rcv_fp, f_num)), base);
    }
    else
    {
	f_num = t_num = fpage_get_size_log2 (&rcv_fp);
	base &= fpage_base_mask (snd_fp, t_num);
	f_addr = addr_offset (addr_mask (fpage_address (snd_fp, t_num),
					 ~fpage_base_mask (snd_fp, t_num)), base);
	t_addr = fpage_address (rcv_fp, t_num);
    }

    if (f_num < hw_pgshifts[0])
    {
	if (f_num != 0)
	    enter_kdebug ("map_fpage(): invalid fpage size");
	return;
    }


    /*
     * Find pagesize to use, and number of pages to map.
     */

    for (pgsize = PGENT_SIZE_MAX; hw_pgshifts[pgsize] > f_num; pgsize--) {}

    f_num = t_num = 1UL << (f_num - hw_pgshifts[pgsize]);
    f_size = t_size = PGENT_SIZE_MAX;
    f_off = 0;

    fpg = space_pgent (self, page_table_index (f_size, f_addr));
    tpg = space_pgent (t_space, page_table_index (t_size, t_addr));

    /* KIP/UTCB areas are loop-invariant (t_space is fixed) pure getters. */
    fpage_t t_kip  = space_get_kip_page_area (t_space);
    fpage_t t_utcb = space_get_utcb_page_area (t_space);

    space_begin_update ();

    while (f_num > 0 || t_num > 0)
    {
	if ((! space_is_user_area_addr (f_addr) && ! space_is_sigma0 (self)) ||
	    (! space_is_user_area_addr (t_addr)))
	    /* Do not mess with kernel area. */
	    break;

	if (space_is_sigma0 (self))
	{
	    /*
	     * When mapping from sigma0 we bypass the page table lookup.
	     */
	    f_size = pgsize;
	}
	else
	{
	    if (! pgent_is_valid (fpg, self, f_size))
	    {
		while (t_size < f_size)
		{
		    /* Recurse up. */
		    f_off += page_size (t_size);
		    tpg = r_tpg[t_size];
		    t_num = r_tnum[t_size];
		    t_size++;
		}

		if (t_size == f_size)
		    goto Next_receiver_entry;

		/* t_size > f_size */
		goto Next_sender_entry;
	    }

	    if ((f_size > pgsize) && pgent_is_subtree (fpg, self, f_size))
	    {
		/*
		 * We are currently working on too large page sizes.
		 */
		f_size--;
		fpg = pgent_next (pgent_subtree (fpg, self, f_size+1),
				  self, f_size, page_table_index (f_size, f_addr));
		continue;
	    }
	    else if (pgent_is_subtree (fpg, self, f_size))
	    {
		/*
		 * The mappings in the senders address space are too
		 * small.  We have to map each single entry in the
		 * subtree.
		 */
		f_size--;
		r_fpg[f_size] = pgent_next (fpg, self, f_size+1, 1);
		r_fnum[f_size] = f_num - 1;

		fpg = pgent_subtree (fpg, self, f_size+1);
		f_num = page_table_size (f_size);
		continue;
	    }
	    else if (f_size > pgsize)
	    {
		/*
		 * We'll be mapping smaller page sizes out of a single
		 * larger page size.
		 */
		f_num = 1;
	    }
	}

	/*
	 * If we get here `fpg' is a valid mapping in the senders
	 * address space.
	 */

	if ((t_size > f_size) || (t_size > pgsize))
	{
	    /*
	     * We are currently working on too large receive pages.
	     */
	    t_size--;
	    r_tpg[t_size] = pgent_next (tpg, t_space, t_size+1, 1);
	    r_tnum[t_size] = t_num - 1;

	    if (! pgent_is_valid (tpg, t_space, t_size+1))
	    {
		/*
		 * Subtree does not exist.  Create one.
		 */
		pgent_make_subtree (tpg, t_space, t_size+1, false);
	    }
	    else if (! pgent_is_subtree (tpg, t_space, t_size+1))
	    {
		/*
		 * There alredy exists a larger mapping.  Just
		 * continue.  BEWARE: This may cause extension of
		 * access rights to be refused even though they are
		 * perfectly legal.  I.e. if all the mappings in the
		 * subtree of the sender's address space are valid.
		 *
		 * NOTE: There have been discussions about changing
		 * the specifications so that the larger mapping is
		 * removed.
		 */
		printf ("map_fpage(): Larger mapping already exists.\n");
		enter_kdebug ("warning: larger mapping exists");
		t_size++;
		goto Next_receiver_entry;
	    }

	    if (t_size >= pgsize)
	    {
		tpg = pgent_next (pgent_subtree (tpg, t_space, t_size+1),
				  t_space, t_size, page_table_index (t_size, t_addr));
		continue;
	    }
	    else
	    {
		tpg = pgent_subtree (tpg, t_space, t_size+1);
		t_num = page_table_size (t_size);
	    }

	    /* Adjust destination according to where source is located. */
	    tpg = pgent_next (tpg, t_space, t_size,
			      page_table_index (t_size, f_addr));
	    t_num -= page_table_index (t_size, f_addr);
	    t_addr = addr_offset (t_addr, page_table_index (t_size, f_addr) <<
				  hw_pgshifts[t_size]);
	    continue;
	}
	else if (pgent_is_valid (tpg, t_space, t_size) &&
		 t_size == f_size && f_size <= pgsize &&

		 /* Make sure that we do not overmap KIP and UTCB area */
		 (! fpage_is_range_overlapping
		  (&t_kip, t_addr, addr_offset (t_addr, page_size (t_size)))) &&
		 (! fpage_is_range_overlapping
		  (&t_utcb, t_addr, addr_offset (t_addr, page_size (t_size)))) &&

		 /* Check if we're simply extending access rights */
		 (pgent_is_subtree (tpg, t_space, t_size) ||
		  (space_is_sigma0 (self) ?
		   (pgent_address (tpg, t_space, t_size) != (paddr_t) f_addr) :
		   (pgent_address (tpg, t_space, t_size) != pgent_address (fpg, self, f_size)))
#if defined(CONFIG_NEW_MDB)
		  ||
		  (mdb_node_get_parent (pgent_mapnode (tpg, t_space, t_size,
						       addr_mask (t_addr, ~page_mask (t_size))))
		   !=
		   pgent_mapnode (fpg, self, f_size,
				  addr_mask (f_addr, ~page_mask (f_size))))
#endif
		     ))
	{
	    /*
	     * We are doing overmapping.  Need to remove existing
	     * mapping or subtree from destination space.
	     */

	    TRACEPOINT (FPAGE_OVERMAP,"overmapping: faddr=%p fsz=%d%cB  taddr=%p tsz=%d%cB (%s)\n",
			f_addr, dbg_pgsize (page_size (f_size)), dbg_szname (page_size (f_size)),
			t_addr, dbg_pgsize (page_size (t_size)), dbg_szname (page_size (t_size)),
			pgent_is_subtree (tpg, t_space, t_size) ? "subtree" : "single map");

	    word_t num = 1;
	    addr_t vaddr = t_addr;
	    fpage_set (&rcv_fp, (word_t) vaddr, page_shift (t_size), true, true, true);

	    while (num > 0)
	    {
		if (! pgent_is_valid (tpg, t_space, t_size))
		{
		    /* Skip invalid entries. */
		}
		else if (pgent_is_subtree (tpg, t_space, t_size))
		{
		    /* We have to flush each single page in the subtree. */
		    t_size--;
		    r_tpg[t_size] = tpg;
		    r_tnum[t_size] = num - 1;

		    tpg = pgent_subtree (tpg, t_space, t_size+1);
		    num = page_table_size (t_size);
		    continue;
		}
		else if (space_is_mappable_addr (t_space, vaddr))
		{
		    struct mapnode_t * omap = pgent_mapnode
			(tpg, t_space, t_size,
			 addr_mask (vaddr, ~page_mask (t_size)));

		    TRACEPOINT
			(MDB_UNMAP, "mdb_flush (spc=%p pg=%p map=%p vaddr=%p rwx tsz=%d%cB)  paddr=%p\n",
			 t_space, tpg, omap, vaddr, dbg_pgsize (page_size (t_size)),
			 dbg_szname (page_size (t_size)), pgent_address (tpg, t_space, t_size));

#if defined(CONFIG_NEW_MDB)
		    mdb_tree_flush (&mdb_mem, omap);
#else
		    mdb_flush_c (omap, tpg, t_size, vaddr, pgsize, &rcv_fp, true);
#endif
		}

		if (t_size < f_size)
		{
		    /* Skip to next entry. */
		    vaddr = addr_offset (vaddr, page_size (t_size));
		    tpg = pgent_next (tpg, t_space, t_size, 1);
		}

		num--;
		if (num == 0 && t_size < f_size)
		{
		    do {
			/* Recurse up and remove subtree. */
			tpg = r_tpg[t_size];
			num = r_tnum[t_size];
			t_size++;
			pgent_remove_subtree (tpg, t_space, t_size, false);
			if (t_size < f_size)
			    tpg = pgent_next (tpg, t_space, t_size, 1);
		    } while (num == 0 && t_size < f_size);
		}
	    }

	    /* We might have to flush the TLB after removing mappings. */
	    if (space_does_tlbflush_pay (fpage_get_size_log2 (&rcv_fp)))
		space_flush_tlb (self, get_current_space_c ());

	    /*
	     * We might have invalidated the source mapping during the unmap
	     * operation above. If so, we have to skip it (unless whe deal with
	     * sigma0 mappings, where source mappings are invalid by default).
	     *
	     */
	    if (!pgent_is_valid (fpg, self, f_size) && !space_is_sigma0 (self))
		goto Next_receiver_entry;
	}
	else if (pgent_is_valid (tpg, t_space, t_size) &&
		 pgent_is_subtree (tpg, t_space, t_size))
	{
	    /*
	     * Target mappings are of smaller page size.  We have to
	     * map each single entry in the subtree.
	     */
	    t_size--;
	    r_tpg[t_size] = pgent_next (tpg, t_space, t_size+1, 1);
	    r_tnum[t_size] = t_num - 1;

	    tpg = pgent_subtree (tpg, t_space, t_size+1);
	    t_num = page_table_size (t_size);
	}
	else if (! is_page_size_valid (t_size))
	{
	    /*
	     * Pagesize is ok but is not a valid hardware pagesize.
	     * Need to create mappings of smaller size.
	     */
	    t_size--;
	    r_tpg[t_size] = pgent_next (tpg, t_space, t_size+1, 1);
	    r_tnum[t_size] = t_num - 1;
	    pgent_make_subtree (tpg, t_space, t_size+1, false);

	    tpg = pgent_subtree (tpg, t_space, t_size+1);
	    t_num = page_table_size (t_size);
	    continue;
	}

	if ((! space_is_mappable_addr (self, f_addr) && ! space_is_sigma0 (self)) ||
	    (! space_is_mappable_addr (t_space, t_addr)))
	    goto Next_receiver_entry;

	/*
	 * If we get here `tpg' will be the page table entry that we
	 * are going to change.
	 */

	offset = (word_t) f_addr & page_mask (f_size) & ~page_mask (t_size);

	if (pgent_is_valid (tpg, t_space, t_size))
	{
	    /*
	     * If a mapping already exists, it might be that we are
	     * just extending the current access rights.
	     */
	    if (space_is_sigma0 (self) ?
		(pgent_address (tpg, t_space, t_size) != (paddr_t) f_addr) :
		(pgent_address (tpg, t_space, t_size) !=
		 paddr_offset (pgent_address (fpg, self, f_size), offset)))
	    {
		paddr_t a UNUSED = space_is_sigma0 (self) ? (paddr_t) f_addr :
		    paddr_offset (pgent_address (fpg, self, f_size), offset);
		printf ("map_fpage(from=%p  to=%p  base=%p  "
			"sndfp=%p  rcvfp=%p)  paddr %p != %p\n",
			self, t_space, base, snd_fp.raw, rcv_fp.raw,
			pgent_address (tpg, t_space, t_size), a);
		enter_kdebug ("map_fpage(): Mapping already exists.");
		goto Next_receiver_entry;
	    }

	    /* Extend access rights. */
	    word_t rights = 0;

	    if (fpage_is_execute (&snd_fp) && pgent_is_executable (fpg, self, f_size))
		rights += 1;
	    if (fpage_is_write (&snd_fp) && pgent_is_writable (fpg, self, f_size))
		rights += 2;
	    if (fpage_is_read (&snd_fp) && pgent_is_readable (fpg, self, f_size))
		rights += 4;

	    pgent_update_rights (tpg, t_space, t_size, rights);
	}
	else
	{
	    /*
	     * This is where the real work is done.
	     */

	    if  (space_is_sigma0 (self))
	    {
		/*
		 * If mapping from sigma0, fpg will not be a valid
		 * page table entry.
		 */

		TRACEPOINT (MDB_MAP, "mdb_map (from {sigma0 pg=%p addr=%p %d%cB} to {"
				    "spc=%p pg=%p addr=%p %d%cB})  paddr=%p\n",
			fpg, addr_offset (f_addr, offset),
			dbg_pgsize (page_size (f_size)), dbg_szname (page_size (f_size)),
			t_space, tpg, t_addr,
			dbg_pgsize (page_size(t_size)), dbg_szname (page_size(t_size)),
			addr_offset (f_addr, offset + f_off));

#if defined(CONFIG_NEW_MDB)
		newmap = mdb_tree_map (&mdb_mem, sigma0_memnode, tpg,
				       page_shift (t_size),
				       (word_t) f_addr + offset + f_off,
				       fpage_get_rwx (&snd_fp), ~0UL);
		mdb_node_set_misc (newmap, mdb_mem_misc (t_space, t_size));
#else
		newmap = mdb_map_c (sigma0_mapnode, fpg, PGENT_SIZE_MAX+1,
				    addr_offset (f_addr, offset + f_off),
				    tpg, t_size, t_space, grant);
#endif

		{
		    paddr_t paddr = space_sigma0_translate (addr_offset(f_addr, offset + f_off), f_size);
		    pgent_set_entry (tpg, t_space, t_size,
				 paddr,
				 fpage_get_rwx (&snd_fp),
				 space_sigma0_attributes (fpg, paddr, f_size),
				 false);
		}
		pgent_set_linknode (tpg, t_space, t_size, newmap, t_addr);
	    }
	    else
	    {
		map = pgent_mapnode (fpg, self, f_size,
				     addr_mask (f_addr, ~page_mask (f_size)));

		TRACEPOINT (MDB_MAP, "mdb_map (node=%p from {pg=%p addr=%p %d%cB} to {"
			    "pg=%p addr=%p %d%cB}) paddr=%p\n", map,
			    fpg, f_addr, dbg_pgsize (page_size(f_size)), dbg_szname (page_size(f_size)),
			    tpg, t_addr, dbg_pgsize (page_size(t_size)), dbg_szname (page_size(t_size)),
			    paddr_offset (pgent_address (fpg, self, f_size),  offset + f_off));

#if defined(CONFIG_NEW_MDB)
		{
		    mdb_node_t *smap = grant ? mdb_node_get_parent (map) : map;
		    newmap = mdb_tree_map (&mdb_mem, smap, tpg, page_shift (t_size),
					   (word_t) pgent_address (fpg, self, f_size) +
					   offset + f_off,
					   fpage_get_rwx (&snd_fp), ~0UL);
		    mdb_node_set_misc (newmap, mdb_mem_misc (t_space, t_size));
		}
#else
		newmap = mdb_map_c (map, fpg, f_size,
				    addr_offset (f_addr, offset + f_off),
				    tpg, t_size, t_space, grant);
#endif

		pgent_set_entry
		    (tpg, t_space, t_size,
		     paddr_offset (pgent_address (fpg, self, f_size), offset+f_off),
		     fpage_get_rwx (&snd_fp), pgent_attributes (fpg, self, f_size),
		     false);
		pgent_set_linknode (tpg, t_space, t_size, newmap, t_addr);

		if (grant)
		{
		    /* Grant operation.  Remove mapping from current space. */
		    pgent_clear (fpg, self, f_size, false, f_addr);
		    space_flush_tlbent (self, get_current_space_c (), f_addr,
					page_shift (f_size));
		}
	    }
	}

    Next_receiver_entry:

	t_addr = addr_offset (t_addr, page_size (t_size));
	t_num--;

	if (t_num > 0)
	{
	    /* Go to next entry */
	    tpg = pgent_next (tpg, t_space, t_size, 1);
	    if (t_size < f_size)
	    {
		f_off += page_size (t_size);
		continue;
	    }
	}
	else if (t_size < f_size && f_size <= pgsize)
	{
	    do {
		/* Recurse up */
		f_off += page_size (t_size);
		tpg = r_tpg[t_size];
		t_num = r_tnum[t_size];
		t_size++;
	    } while (t_num == 0 && t_size < pgsize);
	    if (t_size < f_size)
		continue;
	}
	else if (t_size > f_size)
	{
	    /* Skip to next fpg entry.  Happens if tpg is already mapped. */
	    f_addr = addr_offset (f_addr,
				  page_size (t_size) - page_size (f_size));
	    f_num = 1;
	}

    Next_sender_entry:

#if defined(CONFIG_NEW_MDB)
	if (grant && map != NULL)
	{
	    mdb_tree_flush (&mdb_mem, map);
	    map = NULL;
	}
#endif

	f_addr = addr_offset (f_addr, page_size (f_size));
	f_off = 0;
	f_num--;

	if (f_num > 0)
	{
	    /* Go to next entry */
	    if (! space_is_sigma0 (self))
		fpg = pgent_next (fpg, self, f_size, 1);
	    continue;
	}
	else if (f_size < pgsize)
	{
	    do {
		/* Recurse up */
		fpg = r_fpg[f_size];
		f_num = r_fnum[f_size];
		f_size++;
	    } while (f_size < pgsize && f_num == 0);

	    /* We may now also need to recurse up in receiver space. */
	    if (t_size < f_size && t_num == 0)
	    {
		do {
		    /* Recurse up */
		    tpg = r_tpg[t_size];
		    t_num = r_tnum[t_size];
		    t_size++;
		} while (t_num == 0 && t_size < pgsize);
	    }
	}
	else
	{
	    /* Finished */
	    t_num = 0;
	}
    }

    space_end_update ();
}


/**
 * Unmaps, flushes, or otherwise operate on indicated fpage.  The CTRL
 * parameter specifies the exact operation to perform; unmapping,
 * reading access bits, setting page attributes, etc.  The UNMAP_ALL
 * parameter if true indicates that kernel mappings should also be
 * removed from the address space.
 *
 * @param fpage		fpage to operate on
 * @param ctrl		specifies operation to perform
 * @param attribute	new page attribute
 * @param unmap_all	also unmap kernel mappings (i.e., UTCB and KIP)
 *
 * @returns the original fpage (with access bits set if specified)
 */
fpage_t space_mapctrl (space_t * self, fpage_t fpage, mdb_ctrl_t ctrl,
		       word_t attribute, bool unmap_all)
{
    word_t size, pgsize;
    pgent_t * pg;
    struct mapnode_t * map;
    addr_t vaddr;
    word_t num, rwx = 0;

    pgent_t *r_pg[PGENT_SIZE_MAX];
    word_t r_num[PGENT_SIZE_MAX];

    TRACEPOINT (FPAGE_MAPCTRL,
		"<spc=%p>::mapctrl ([%d, %x %x] [%x], %s, %x %c)\n",
		self, fpage_get_address (&fpage),
		fpage_get_size_log2 (&fpage), fpage_get_rwx (&fpage),
		ctrl.raw, dbg_ctrl_string (ctrl), attribute,
		unmap_all ? 't' : 'f');

    // Fpage access bits semantics are reversed for old MDB
    fpage_set_rwx (&fpage, ~fpage_get_rwx (&fpage));

    num = fpage_get_size_log2 (&fpage);
    vaddr = fpage_address (fpage, num);

    if (num < hw_pgshifts[0])
    {
	WARNING ("mapctrl_fpage(): too small fpage size (%d) - fixed\n", num);
	num = hw_pgshifts[0];
    }

    /*
     * Some architectures may not support a complete virtual address
     * space.  Enforce unmaps to only cover the supported space.
     */

    if (num > hw_pgshifts[PGENT_SIZE_MAX+1])
	num = hw_pgshifts[PGENT_SIZE_MAX+1];

    /*
     * Find pagesize to use, and number of pages to map.
     */

    for (pgsize = PGENT_SIZE_MAX; hw_pgshifts[pgsize] > num; pgsize--) {}

    num = 1UL << (num - hw_pgshifts[pgsize]);
    size = PGENT_SIZE_MAX;
    pg = space_pgent (self, page_table_index (size, vaddr));

    space_begin_update ();

    while (num)
    {
	if (! space_is_user_area_addr (vaddr))
	    /* Do not mess with kernel area. */
	    break;

	if (size > pgsize)
	{
	    /* We are operating on too large page sizes. */
	    if (! pgent_is_valid (pg, self, size))
		break;
	    else if (pgent_is_subtree (pg, self, size))
	    {
		size--;
		pg = pgent_next (pgent_subtree (pg, self, size+1),
				 self, size, page_table_index (size, vaddr));
		continue;
	    }
	    else if (ctrl.mapctrl_self)
	    {
		/* Fpage too small -- expand to current size */
		num = 1;
		pgsize = size;
	    }
	}

	if (! pgent_is_valid (pg, self, size))
	    goto Next_entry;

	if (pgent_is_subtree (pg, self, size))
	{
	    /* We have to flush each single page in the subtree. */
	    size--;
	    r_pg[size] = pg;
	    r_num[size] = num - 1;

	    pg = pgent_subtree (pg, self, size+1);
	    num = page_table_size (size);
	    continue;
	}

	/* Only unmap from mapping database if user-mapping. */
	if (space_is_mappable_addr (self, vaddr))
	{
	    map = pgent_mapnode (pg, self, size,
				 addr_mask (vaddr, ~page_mask (size)));

	    TRACEPOINT (MDB_UNMAP, "mdb_%s (spc=%p pg=%p map=%p vaddr=%p %c%c%c "
			"fsz=%d%cB tsz=%d%cB)  paddr=%p\n",
			ctrl.mapctrl_self ? "flush" : "unmap",
			self, pg, map, vaddr,
			fpage_is_read (&fpage) ? 'r' : '~',
			fpage_is_write (&fpage) ? 'w' : '~',
			fpage_is_execute (&fpage) ? 'x' : '~',
			dbg_pgsize (page_size(size)), dbg_szname (page_size(size)),
			dbg_pgsize (page_size(pgsize)), dbg_szname (page_size(pgsize)),
			pgent_address (pg, self, size));

#if defined(CONFIG_NEW_MDB)
	    rwx |= mdb_tree_mapctrl (&mdb_mem, map,
				     mdb_range_make (fpage_get_base (&fpage),
						     fpage_get_size_log2 (&fpage)),
				     ctrl, fpage_get_rwx (&fpage), attribute);
#else
	    rwx |= mdb_flush_c (map, pg, size, vaddr, pgsize,
				&fpage, ctrl.mapctrl_self);
#endif
	}
	else if (unmap_all)
	{
	    space_release_kernel_mapping (self, vaddr, pgent_address (pg, self, size),
					  page_shift (size));

	    pgent_clear (pg, self, size, true, vaddr);
	    if (! space_does_tlbflush_pay (fpage_get_size_log2 (&fpage)))
		space_flush_tlbent (self, get_current_space_c (), vaddr, page_shift (size));
	}

    Next_entry:

	pg = pgent_next (pg, self, size, 1);
	vaddr = addr_offset (vaddr, page_size (size));
	num--;

	while (num == 0 && size < pgsize)
	{
	    /* Recurse up */
	    pg = r_pg[size];
	    num = r_num[size];
	    size++;

	    fpage_t fp;
	    fp.raw = 0;
	    fpage_set (&fp, (word_t) vaddr, page_size (size), false, false, false);

	    if (ctrl.mapctrl_self && ctrl.unmap &&
		(unmap_all || space_is_mappable_fpage (self, fp)))
		pgent_remove_subtree (pg, self, size, false);

	    pg = pgent_next (pg, self, size, 1);
	}
    }

    if (space_does_tlbflush_pay (fpage_get_size_log2 (&fpage)))
	space_flush_tlb (self, get_current_space_c ());

    space_end_update ();

    fpage_set_rwx (&fpage, rwx);
    return fpage;
}


/**
 * Read word from address space.  Parses page tables to find physical
 * address of mapping and reads the indicated word directly from
 * kernel-mapped physical memory.
 *
 * @param vaddr		virtual address to read
 * @param contents	returned contents in given virtual location
 *
 * @return true if mapping existed, false otherwise
 */
bool space_readmem (space_t * self, addr_t vaddr, word_t * contents)
{
    pgent_t * pg;
    word_t pgsize;

    if (! space_lookup_mapping_c (self, vaddr, &pg, &pgsize))
	return false;

    paddr_t paddr = pgent_address (pg, self, pgsize);
    paddr = paddr_offset (paddr, (word_t) vaddr & page_mask (pgsize));
    paddr_t paddr1 = paddr_mask (paddr, ~(sizeof (word_t) - 1));

    if (paddr1 == paddr)
    {
	// Word access is properly aligned.
	*contents = space_readmem_phys (paddr);
    }
    else
    {
	// Word access not properly aligned.  Need to perform two
	// separate accesses.
	paddr_t paddr2 = paddr_offset (paddr1, sizeof (word_t));
	word_t mask = ~page_mask (pgsize);

	if (paddr_mask (paddr1, mask) != paddr_mask (paddr2, mask))
	{
	    // Word access crosses page boundary.
	    vaddr = addr_offset (vaddr, sizeof (word_t));
	    if (! space_lookup_mapping_c (self, vaddr, &pg, &pgsize))
		return false;
	    paddr2 = pgent_address (pg, self, pgsize);
	    paddr2 = paddr_offset (paddr2, (word_t) vaddr & page_mask (pgsize));
	    paddr2 = paddr_mask (paddr2, ~(sizeof (word_t) - 1));
	}

	word_t idx = ((word_t) vaddr) & (sizeof (word_t) - 1);

#if defined(CONFIG_BIGENDIAN)
	*contents =
	    (space_readmem_phys (paddr1) << (idx * 8)) |
	    (space_readmem_phys (paddr2) >> ((sizeof (word_t) - idx) * 8));
#else
	*contents =
	    (space_readmem_phys (paddr1) >> (idx * 8)) |
	    (space_readmem_phys (paddr2) << ((sizeof (word_t) - idx) * 8));
#endif
    }

    return true;
}

#endif /* !__GENERIC__LINEAR_PTAB_WALKER_CC__ */
