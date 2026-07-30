/*********************************************************************
 *
 * Copyright (C) 2006-2007,  Karlsruhe University
 *
 * File path:     glue/v4-ia32/hvm/vtlb.cc
 * Description:   Full Virtualization Extensions - Generic VTLB
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
 * $Id$
 *
 ********************************************************************/

#include INC_API(tcb.h)
#include INC_GLUE(hvm.h)
#include INC_GLUE(hvm-vtlb.h)

#include <kmemory.h>
#include <linear_ptab.h>
#include <lib.h>		/* min() */

/*
 * Was the x86_hvm_vtlb_t methods.  pgent_t::pgsize_e is a word_t holding an
 * X86_PGSIZE_* value, as everywhere else since the ptab conversion, and the
 * pgent_t methods are the pgent_* / x86_pgent_* wrappers in arch/x86/pgent.h
 * and arch/x86/x32/ptab.h -- `pg->pgent.clear()' reached the raw x86_pgent_t
 * inside pgent_t and is x86_pgent_clear (&pg->pgent), distinct from the
 * space-aware pgent_clear.
 */

DECLARE_KMEM_GROUP (kmem_vtlb);

DECLARE_TRACEPOINT(X86_HVM_VTLB);
DECLARE_TRACEPOINT(X86_HVM_VTLB_MISS);
DECLARE_TRACEPOINT(X86_HVM_VTLB_FLUSH);

/* Were the private members set_gphys_entry / set_hphys_entry. */
static void x86_hvm_vtlb_set_gphys_entry (x86_hvm_vtlb_t *self, addr_t gvaddr, addr_t gpaddr,
					  word_t gvpgsz, word_t rwx, word_t attrib,
					  bool kernel, bool global, word_t access);
static void x86_hvm_vtlb_set_hphys_entry (x86_hvm_vtlb_t *self, addr_t gvaddr, addr_t hpaddr,
					  word_t hppgsz, word_t rwx, word_t attrib,
					  bool kernel, bool global);


bool x86_hvm_vtlb_alloc (x86_hvm_vtlb_t *self, space_t *space)
{
    word_t i;

    /* Allocate a single host pdir for paged mode */
    self->hpdir_paged = (pgent_t *) kmem_alloc(&kmem, kmem_vtlb, X86_PAGE_SIZE);;
    if (!self->hpdir_paged)
	return false;

    for (i = 0; i < 1024; i++)
	x86_pgent_clear (&self->hpdir_paged[i].pgent);


    /* Allocate a single host pdir for unpaged mode */
    self->hpdir_nonpaged = (pgent_t *) kmem_alloc(&kmem, kmem_vtlb, X86_PAGE_SIZE);;
    if (!self->hpdir_nonpaged)
	return false;

    for (i = 0; i < 1024; i++)
	x86_pgent_clear (&self->hpdir_nonpaged[i].pgent);

    self->hpdir = self->hpdir_nonpaged;

    self->space = space;
    self->flags.pe = self->flags.wp = false;


    return true;
}

void x86_hvm_vtlb_free (x86_hvm_vtlb_t *self)
{
    if (self->hpdir_paged)
	kmem_free(&kmem, kmem_vtlb, self->hpdir_paged, X86_PAGE_SIZE);

    if (self->hpdir_nonpaged)
	kmem_free(&kmem, kmem_vtlb, self->hpdir_nonpaged, X86_PAGE_SIZE);

    self->space = NULL;
}


void x86_hvm_vtlb_flush_hpdir (x86_hvm_vtlb_t *self, pgent_t *pdir)
{
    space_t *space = self->space;
    word_t i;

    TRACEPOINT(X86_HVM_VTLB_FLUSH, "VTLB (%x:%x) flush %x", self->gpdir, self->hpdir, pdir);
    ASSERT(pdir);

    for (i = 0; i < 1024; i++)
    {
	bool pdir_global = false;

	if (pgent_is_valid (&pdir[i], space, X86_PGSIZE_4M) &&
	    pgent_is_subtree (&pdir[i], space, X86_PGSIZE_4M) &&
	    (pgent_reference_bits (&pdir[i], space, X86_PGSIZE_4M, 0) & 0x6))
	{
	    pgent_t *hptab = pgent_subtree (&pdir[i], space, X86_PGSIZE_4M);
	    bool ptab_global = false;
	    word_t j;

	    //If any pte is global, leave in place
	    for (j = 0; j < 1024; j++)
	    {
		if (pgent_is_valid (&hptab[j], space, X86_PGSIZE_4K) &&
		    pgent_is_global (&hptab[j], space, X86_PGSIZE_4K))
		    ptab_global = true;
		else
		    x86_pgent_clear (&hptab[j].pgent);
	    }
	    if (!ptab_global || !self->flags.pg)
		kmem_free(&kmem, kmem_vtlb, hptab, X86_PAGE_SIZE);
	    else
		pdir_global = true;
	}
	if (!pdir_global || !self->flags.pg)
	    x86_pgent_clear (&pdir[i].pgent);
    }

}


void x86_hvm_vtlb_flush_hpdir_addr (x86_hvm_vtlb_t *self, pgent_t *pdir, addr_t gvaddr)
{
    space_t *space = self->space;
    pgent_t *pg;

    TRACEPOINT(X86_HVM_VTLB_FLUSH, "VTLB (%x:%x) flush %x single %x", self->gpdir, self->hpdir, pdir, gvaddr);
    ASSERT(pdir);

    pg = pgent_next (pdir, space, X86_PGSIZE_4M, page_table_index (X86_PGSIZE_4M, gvaddr));
    /* We clear only single entries and never remove page tables. */

    if (EXPECT_TRUE (!x86_pgent_is_valid (&pg->pgent)))
	return;

    if (!pgent_is_subtree (pg, space, X86_PGSIZE_4M))
	x86_pgent_clear (&pg->pgent);
    else
    {
	pg = pgent_next (pgent_subtree (pg, space, X86_PGSIZE_4M), space,
			 X86_PGSIZE_4K, page_table_index (X86_PGSIZE_4K, (addr_t) gvaddr));

	x86_pgent_clear (&pg->pgent);
    }

}



bool x86_hvm_vtlb_lookup_gphys_addr (x86_hvm_vtlb_t *self, addr_t gvaddr, addr_t *gpaddr)
{
    space_t *space = self->space;
    word_t gvpgsz;
    pgent_t *gvpgent;
    pgent_t gvpgent_phys;

    if (!self->flags.pe)
    {
	*gpaddr = gvaddr;
	return true;
    }

    /* Read guest page table. */
    gvpgsz = X86_PGSIZE_MAX;
    gvpgent = pgent_next (self->gpdir, space, X86_PGSIZE_4M,
			  page_table_index (X86_PGSIZE_4M, (addr_t) gvaddr));

    for (;;)
    {
	ASSERT(space);

	if (! space_readmem (space, (addr_t) gvpgent, (word_t *) &gvpgent_phys))
	    return false;

	gvpgent = &gvpgent_phys;

	if (pgent_is_valid (gvpgent, space, gvpgsz))
	{
	    if (pgent_is_subtree (gvpgent, space, gvpgsz))
	    {
		// Recurse into subtree
		if (gvpgsz == 0)
		    return false;

		gvpgent = pgent_next (virt_to_phys (pgent_subtree (gvpgent, space, gvpgsz)),
				      space, gvpgsz-1, page_table_index (gvpgsz-1, gvaddr));
		gvpgsz--;
	    }
	    else
	    {
		// Return address
		*gpaddr = addr_offset (pgent_address (gvpgent, space, gvpgsz),
				       addr_mask(gvaddr, page_mask(gvpgsz)));

		return true;
	    }
	}
	else
	    // No valid mapping or subtree
	    return false;
    }

    /* NOTREACHED */
    return false;


}


bool x86_hvm_vtlb_dump_ptab_entry (x86_hvm_vtlb_t *self, addr_t gvaddr)
{
    space_t *space = self->space;
    word_t gvpgsz;
    pgent_t *gvpgent;
    pgent_t gvpgent_phys;

    if (!self->flags.pe)
    {
	printf("[gvirt] %p -> [gphys] %p (no paging)\n", gvaddr, gvaddr);
	return true;
    }

    /* Read guest page table. */
    gvpgsz = X86_PGSIZE_MAX;
    gvpgent = pgent_next (self->gpdir, space, X86_PGSIZE_4M,
			  page_table_index (X86_PGSIZE_4M, (addr_t) gvaddr));

    for (;;)
    {
	ASSERT(space);

	if (! space_readmem (space, (addr_t) gvpgent, (word_t *) &gvpgent_phys))
	    return false;

	gvpgent = &gvpgent_phys;

	if (pgent_is_valid (gvpgent, space, gvpgsz))
	{
	    if (pgent_is_subtree (gvpgent, space, gvpgsz))
	    {
		// Recurse into subtree
		if (gvpgsz == 0)
		    return false;

		gvpgent = pgent_next (virt_to_phys (pgent_subtree (gvpgent, space, gvpgsz)),
				      space, gvpgsz-1, page_table_index (gvpgsz-1, gvaddr));
		gvpgsz--;
	    }
	    else
	    {
		addr_t gpaddr = addr_offset (pgent_address (gvpgent, space, gvpgsz),
					     addr_mask(gvaddr, page_mask(gvpgsz)));
		word_t pgsz;
		word_t rwx;

		printf("[gvirt] %p -> [gphys] %p ", gvaddr, gpaddr );
		pgsz = page_size (gvpgsz);
		rwx = pgent_reference_bits (gvpgent, space, gvpgsz, gvaddr);
		printf("%3d%cB %c%c%c (%c%c%c) %s ",
		       (pgsz >= GB (1) ? pgsz >> 30 :
			pgsz >= MB (1) ? pgsz >> 20 : pgsz >> 10),
		       pgsz >= GB (1) ? 'G' : pgsz >= MB (1) ? 'M' : 'K',
		       pgent_is_readable (gvpgent, space, gvpgsz)   ? 'r' : '~',
		       pgent_is_writable (gvpgent, space, gvpgsz)   ? 'w' : '~',
		       pgent_is_executable (gvpgent, space, gvpgsz) ? 'x' : '~',
		       rwx & 4 ? 'R' : '~',
		       rwx & 2 ? 'W' : '~',
		       rwx & 1 ? 'X' : '~',
		       pgent_is_kernel (gvpgent, space, gvpgsz) ? "kernel" : "user");
		pgent_dump_misc (gvpgent, space, gvpgsz);
		printf("\n");
		return true;
	    }
	}
	else
	    // No valid mapping or subtree
	    return false;
    }

    /* NOTREACHED */
    return false;


}


/* Insert a GV->GP mapping into the VTLB.
   Note: This function is generic, not IA-32 specific. */
static void x86_hvm_vtlb_set_gphys_entry (x86_hvm_vtlb_t *self, addr_t gvaddr, addr_t gpaddr,
					  word_t gvpgsz, word_t rwx, word_t attrib,
					  bool kernel, bool global, word_t access)
{
    space_t *space = self->space;
    pgent_t *gppgent = NULL;
    word_t gppgsz;
    tcb_t *current = get_current_tcb();
    addr_t hpaddr;
    word_t hppgsz;

    /* Lookup/request a mapping from monitor. */
    while (!(space_lookup_mapping_c (space, gpaddr, &gppgent, &gppgsz)) ||
	   (!(space_is_user_area (gpaddr))) ||
	   ((access & X86_PAGE_WRITABLE) && !pgent_is_writable (gppgent, space, gppgsz)))

    {
	addr_t gpaddr_base;
	fpage_t kip_area, utcb_area;

        TRACEPOINT(X86_HVM_VTLB_MISS, "VTLB (%x:%x) gp %08x miss", self->gpdir, self->hpdir, gpaddr);

        gpaddr_base = addr_align(gpaddr, page_size(gvpgsz));
	kip_area  = space_get_kip_page_area (space);
	utcb_area = space_get_utcb_page_area (space);

	// Check if access to KIP/UTCB page area
	if ((access & X86_PAGE_WRITABLE) &&
	    (fpage_is_range_overlapping (&kip_area, gpaddr_base,
					 addr_offset (gpaddr_base, page_size (gvpgsz)-1)) ||
             fpage_is_range_overlapping (&utcb_area, gpaddr_base,
					 addr_offset (gpaddr_base, page_size (gvpgsz)-1))))

	{
	    // If unpaged access, try  a smaller size
	    if (gvpgsz == X86_PGSIZE_MAX)
	    {
		gvpgsz--;
		continue;
	    }
	    else
	    {
		printf("VTLB (%x:%x) gp %08x base %08x vpgsz %d access %x KIP area %x %x UTCB area %x %x\n",
		       self->gpdir, self->hpdir, gpaddr, gpaddr_base, page_size(gvpgsz), access,
		       fpage_get_address (&utcb_area), fpage_get_size (&utcb_area),
		       fpage_get_address (&kip_area), fpage_get_size (&kip_area));
		enter_kdebug("HVM thread writes KIP/UTCB area");
	    }
	}

        /*
	 * The space does not have a mapping.
	 * Send page fault message to monitor.
	 */
	tcb_send_pagefault_ipc (current, (addr_t) gpaddr, tcb_get_user_ip (current),
				(int) (access & X86_PAGEFAULT_BITS));

	memory_barrier();
#warning Nilpage handling needs proper implementation
	/* Still no mapping after pagefault, assume someone send a Nilpage */
	if (!(space_lookup_mapping_c (space, gpaddr, &gppgent, &gppgsz)) ||
	    (!(space_is_user_area (gpaddr))) ||
	    ((access & X86_PAGE_WRITABLE) && !pgent_is_writable (gppgent, space, gppgsz)))
	    return;
    }

    hpaddr = addr_offset (pgent_address (gppgent, space, gppgsz),
			  addr_mask(gpaddr, page_mask(gppgsz)));


    /* Use the smaller of guest virtual and guest physical page size. */
    hppgsz = (word_t) min ((int) gvpgsz, (int) gppgsz);

    /* Merge bits */
    rwx &= pgent_rights (gppgent, space, gppgsz);
    attrib |= pgent_attributes (gppgent, space, gppgsz);

    /* Insert the VTLB entry. */
    x86_hvm_vtlb_set_hphys_entry (self, gvaddr, hpaddr, hppgsz, rwx, attrib, kernel, global);


}

static void x86_hvm_vtlb_set_hphys_entry (x86_hvm_vtlb_t *self, addr_t gvaddr, addr_t hpaddr,
					  word_t hppgsz, word_t rwx, word_t attrib,
					  bool kernel, bool global)
{
    space_t *space = self->space;
    pgent_t *hppgent = pgent_next (self->hpdir, space, X86_PGSIZE_4M,
				   page_table_index (X86_PGSIZE_4M, gvaddr));


    TRACEPOINT(X86_HVM_VTLB, "VTLB (%x:%x) gv %08x -> hp %08x sz %d rwx %x attr %x %c %c",
		self->gpdir, self->hpdir, gvaddr, hpaddr, page_size(hppgsz), rwx, attrib,
		(kernel ? 'k' : 'u'), (global ? 'g' : ' '));

    /* Check whether we are mapping a superpage. */
    if (hppgsz == X86_PGSIZE_4M)
    {
	/* If there is a page table in place, remove it. */
	if (pgent_is_valid (hppgent, space, hppgsz) && pgent_is_subtree (hppgent, space, hppgsz))
	{
	    pgent_t *pt = pgent_subtree (hppgent, space, X86_PGSIZE_4M);
	    //printf( "VTLB (%x:%x) gv %08x -> hp %08x sz %d flush subtree %x",
	    //       gpdir, hpdir, gvaddr, hpaddr, page_size(hppgsz), pt);
	    kmem_free(&kmem, kmem_vtlb, pt, X86_PAGE_SIZE);
	}
    }
    else
    {
	/* If there is no page table, allocate a new one. */
	if (!pgent_is_valid (hppgent, space, hppgsz+1) || !pgent_is_subtree (hppgent, space, hppgsz+1))
	{
	    /* Allocate memory. */
	    pgent_t *pt = (pgent_t *) kmem_alloc(&kmem, kmem_vtlb, X86_PAGE_SIZE);
	    word_t i;

	    if (!pt)
	    {
		printf("VTLB (%x:%x) not enough memory, flush", self->gpdir, self->hpdir);
		enter_kdebug("UNTESTED");
		/* Try to free up some space. */
		x86_hvm_vtlb_flush_gphys (self);
		/* This will cause a retry. */
		return;
	    }

	    //printf( "VTLB (%x:%x) gv %08x -> hp %08x sz %d alloc subtree %x",
	    //       gpdir, hpdir, gvaddr, hpaddr, page_size(hppgsz), pt);

	    /* Clear new page table. */
	    for (i = 0; i < 1024; i++)
		x86_pgent_clear (&pt[i].pgent);

	    /* Set page table. */
	    x86_pgent_set_ptab_entry (&hppgent->pgent, virt_to_phys (pt),
				      X86_PAGE_USER | X86_PAGE_WRITABLE);

	}
	//printf( "VTLB (%x:%x) hppgent %08x", gpdir, hpdir, hppgent->raw);

	hppgent = pgent_next (pgent_subtree (hppgent, space, X86_PGSIZE_4M), space,
			      X86_PGSIZE_4K, page_table_index (X86_PGSIZE_4K, gvaddr));
    }

    /* Set mapping. Don't use set_entry(), since it syncs entries */
    x86_pgent_set_entry (&hppgent->pgent, hpaddr, hppgsz,
			 (kernel ? X86_PAGE_KERNEL : X86_PAGE_USER) |
			  (attrib & 1 ? X86_PAGE_WRITE_THROUGH : 0) |
			  (attrib & 2 ? X86_PAGE_CACHE_DISABLE : 0) |
			  (attrib & 4 ? (1UL << (hppgsz == X86_PGSIZE_4K ? 7 : 12)) : 0) |
			  (rwx & 2 ? X86_PAGE_WRITABLE : 0) |
			  (global ? X86_PAGE_GLOBAL : 0) |
			  X86_PAGE_VALID);

    //printf( "VTLB (%x:%x) hppgent %08x\n", gpdir, hpdir, hppgent->raw);


}


/* Handle a VTLB miss. */
bool x86_hvm_vtlb_handle_vtlb_miss (x86_hvm_vtlb_t *self, addr_t gvaddr, word_t access)
{
    space_t *space = self->space;

    ASSERT(get_current_space_c () == space);

    if (!self->flags.pe)
    {
	x86_hvm_vtlb_set_gphys_entry (self, gvaddr, gvaddr, X86_PGSIZE_MAX, 0xf, 0, false, true, access);
	return true;
    }
    else
    {
	/* Parse the page table hierarchy. */
	word_t gvpgsz = X86_PGSIZE_MAX;
	pgent_t *gvpgent = pgent_next (self->gpdir, space, gvpgsz,
				       page_table_index (gvpgsz, gvaddr));
	word_t rwx = 0, attrib = 0;
	bool kernel = false, global = false;
	addr_t gpaddr = 0;

	if (!pgent_is_valid (gvpgent, space, gvpgsz))
	    goto access_fault;

	rwx = pgent_rights (gvpgent, space, gvpgsz);
	attrib = pgent_attributes (gvpgent, space, gvpgsz);
	kernel = pgent_is_kernel (gvpgent, space, gvpgsz);
	global = pgent_is_global (gvpgent, space, gvpgsz);

	while (pgent_is_subtree (gvpgent, space, gvpgsz))
	{
	    //printf( "VTLB (%x:%x) gvpgent %08x %d",
	    //       gpdir, hpdir, gvpgent->raw, page_table_index(gvpgsz, gvaddr));
	    gvpgent = virt_to_phys (pgent_subtree (gvpgent, space, gvpgsz--));
	    gvpgent = pgent_next (gvpgent, space, gvpgsz, page_table_index (gvpgsz, gvaddr));

	    if (!pgent_is_valid (gvpgent, space, gvpgsz))
		goto access_fault;

	    rwx &= pgent_rights (gvpgent, space, gvpgsz);
 	    attrib = pgent_attributes (gvpgent, space, gvpgsz);
	    kernel = (kernel || pgent_is_kernel (gvpgent, space, gvpgsz));
	    global = (global || pgent_is_global (gvpgent, space, gvpgsz));

	}

	//printf( "VTLB (%x:%x) gvpgent %08x %d",
	//   gpdir, hpdir, gvpgent->raw, page_table_index(gvpgsz, gvaddr));

	if (kernel && (access & X86_PAGE_USER))
	    goto access_fault;

	if (!(rwx & 2) && (access & X86_PAGE_WRITABLE) &&
	    ((access & X86_PAGE_USER) || self->flags.wp))
	    goto access_fault;


	gpaddr = addr_offset (pgent_address (gvpgent, space, gvpgsz),
			      addr_mask(gvaddr, page_mask(gvpgsz)));


	/* Notify guest about page access. */
	pgent_update_reference_bits (gvpgent, space, gvpgsz, (access & X86_PAGE_WRITABLE) ? 6 : 5);


	//printf( "VTLB (%x:%x) gv %08x -> gp %08x sz %d rwx %x attr %x %c %c access %x",
	//   gpdir, hpdir, gvaddr, gpaddr, page_size(gvpgsz), rwx, attrib,
	//   (kernel ? 'k' : 'u'), (global ? 'g' : ' '), access);

	/*
	 * The page must be marked as dirty before it can be written to.
	 * Otherwise the dirty bit would stay unset when the guest writes to it later.
	 * We never set the dirty bit on page directories, so don't check for it here.
	 *
	 * SR: What are the hardware semantics if a page is marked dirty but not
	 *     writable (i.e., the guest removed write privileges)?
	 *     We don't remove it from the VTLB since we don't notice the privilege
	 *     removal, but if we get a VTLB miss, we will map it read-only.
	 *     Every sane guest OS will cause a TLB flush, of course.
	 */
	if (!(pgent_reference_bits (gvpgent, space, gvpgsz, gvaddr) & 0x2))
	    rwx &= ~2UL;

	x86_hvm_vtlb_set_gphys_entry (self, gvaddr, gpaddr, gvpgsz, rwx, attrib, kernel, global, access);
	return true;

    access_fault:
	TRACEPOINT(X86_HVM_VTLB_MISS, "VTLB (%x:%x) gv %08x -> gp %08x sz %d rwx %x %c %c access fault %x",
		   self->gpdir, self->hpdir, gvaddr, gpaddr,
		   page_size(gvpgsz), pgent_rights (gvpgent, space, gvpgsz),
		   (kernel ? 'k' : 'u'), (global ? 'g' : ' '), access);

	return false;
    }

    /* NOTREACHED: the C++ body had an unreachable TRACEPOINT and return here,
       after an if/else in which both arms return.  Dropped rather than kept as
       dead code the compiler would warn about. */
}
