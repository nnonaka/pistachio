/*********************************************************************
 *                
 * Copyright (C) 2003-2004, 2007,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/x32/smallspaces.c
 * Description:   Handling of small address spaces
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
 * $Id: smallspaces.cc,v 1.8 2004/03/10 18:33:22 skoglund Exp $
 *                
 ********************************************************************/
#include <debug.h>
#include <linear_ptab.h>
#include <kdb/tracepoints.h>

#include INC_API(kernelinterface.h)
#include INC_API(tcb.h)
#include INC_API(smp.h)
#include INC_GLUE(space.h)


FEATURESTRING ("smallspaces");

DECLARE_TRACEPOINT (SMALLSPACE_CREATE);
DECLARE_TRACEPOINT (SMALLSPACE_ENLARGE);
DECLARE_TRACEPOINT (SMALLSPACE_SYNC);


/**
 * Array containing the owners of small space slots.
 */
x86_space_t * small_space_owner[SMALLSPACE_AREA_SIZE >> X86_X32_PDIR_BITS];


/**
 * Spinlock protecting access to the small space owners array.
 */
DEFINE_SPINLOCK (small_space_owner_lock);


/**
 * Linked list of spaces that are polluted with small space mappings.
 */
static x86_space_t * polluted_spaces = NULL;


/**
 * Spinlock protecting the polluted spaces list.
 */
DEFINE_SPINLOCK (polluted_spaces_lock);



/**
 * Set to non-nil if currently running in a small space.
 */
word_t __is_small UNIT ("cpulocal");


#if defined(CONFIG_X86_X32_SMALL_SPACES_GLOBAL)
/**
 * Modify global bits in the page tables of indicated space.
 *
 * @param space		space to modify global bits in
 * @param pg		pgent to start modification in
 * @param size		size of memory region to modify (in bytes)
 * @param onoff		whether to set (true) or clear (false) global bit
 */
static void modify_global_bits (space_t * space, pgent_t * pg,
				int size, bool onoff)
{
    word_t pgsize = X86_PGSIZE_MAX;

    while (size > 0)
    {
	if (pgent_is_valid (pg, space, pgsize))
	{
	    pgent_set_global (pg, space, pgsize, onoff);

	    if (pgent_is_subtree (pg, space, pgsize))
	    {
		pgent_t * pg2 = pgent_subtree (pg, space, pgsize--);
		for (int i = 1024; i > 0; i--)
		{
		    if (pgent_is_valid (pg2, space, pgsize))
			pgent_set_global (pg2, space, pgsize, onoff);
		    pg2 = pgent_next (pg2, space, pgsize, 1);
		}
		pgsize++;
	    }
	}

	size -= (int) page_size (pgsize);
	pg = pgent_next (pg, space, pgsize, 1);
    }
}
#endif /* CONFIG_X86_X32_SMALL_SPACES_GLOBAL */


/*
 * When global small spaces is enabled we must do a global TLB flush
 * to get rid of the small space TLB entries.
 */
#if defined(CONFIG_X86_X32_SMALL_SPACES_GLOBAL)
#define FLUSH_GLOBAL true
#else
#define FLUSH_GLOBAL false
#endif


#if defined(CONFIG_SMP)
static void do_xcpu_flush_tlb(cpu_mb_entry_t * entry)
{
    (void) entry;
    spin(60, get_current_cpu());
    x86_mmu_flush_tlb (FLUSH_GLOBAL);
}
#endif /* CONFIG_SMP */



/**
 * Turn address space into a small space.  If address space is already
 * small we first turn the space into a large space before turning it
 * into a small space again.
 *
 * @param id		small space id
 *
 * @return true if conversion succeeded, false otherwise
 */
bool x86_space_make_small (x86_space_t * self, smallspace_id_t id)
{
    const word_t max_idx = SMALLSPACE_AREA_SIZE >> X86_X32_PDIR_BITS;

    word_t size = smallspace_id_size (&id) >> 22;
    word_t offset = smallspace_id_offset (&id) >> 22;
    word_t i;

    TRACEPOINT (SMALLSPACE_CREATE,
		"make_small: space=%p  size=%dMB  offset=%dMB\n", self, size*4, offset*4);

    if (offset + size > max_idx)
	return false;

    // Grab lock and make sure that we are not already holding a small
    // space area.
    for (;;)
    {
	spinlock_lock (&small_space_owner_lock);

	// Verify that space is not already small.  If so, we enlarge
	// space and try again.
	for (i = 0; i < max_idx; i++)
	    if (small_space_owner[i] == self)
	    {
		spinlock_unlock (&small_space_owner_lock);
		x86_space_make_large (self);
		continue;
	    }

	break;
    }

    // Try allocation small space slots.
    for (i = 0; i < size; i++)
    {
	if (small_space_owner[offset + i] == NULL)
	    small_space_owner[offset + i] = self;
	else
	    break;
    }

    // If allocation failed, free up the allocated slots and return
    // error.
    if (i < size)
    {
	while (i > 0)
	    small_space_owner[offset + --i] = NULL;

	spinlock_unlock (&small_space_owner_lock);
	return false;
    }

    // Small space area has now been allocated.
    *x86_space_smallid (self) = id;

    x86_segdesc_set_seg (x86_space_segdesc (self),
			 x86_space_smallspace_offset (self),
			 x86_space_smallspace_size (self) - 1,
			 3, X86_SEGDESC_DATA);

    spinlock_unlock (&small_space_owner_lock);

#if defined(CONFIG_X86_X32_SMALL_SPACES_GLOBAL)
    // Set global bits for all pages in small space area.
    modify_global_bits ((space_t *) self, space_pgent ((space_t *) self, 0),
			(int) x86_space_smallspace_size (self), true);
#endif

    if ((space_t *) self == get_current_space_c () || get_current_tcb () == get_idle_tcb_c ())
    {
	// Reset GDT entries to have proper limits.
	extern x86_segdesc_t gdt[];

	x86_segdesc_set_seg (&gdt[X86_UCS >> 3], x86_space_smallspace_offset (self),
			     x86_space_smallspace_size (self)-1, 3, X86_SEGDESC_CODE);
	x86_segdesc_set_seg (&gdt[X86_UDS >> 3], x86_space_smallspace_offset (self),
			     x86_space_smallspace_size (self)-1, 3, X86_SEGDESC_DATA);

	reload_user_segregs_c ();

	// Inform thread switch code that we run in a small space.
	__is_small = 1;
    }

    return true;
}


/**
 * Turn address space into a large space.  This is an expensive
 * operation since all stale pagedir entries in the small space area
 * of other page directories must be purged.
 */
void x86_space_make_large (x86_space_t * self)
{
    smallspace_id_t id = *x86_space_smallid (self);
    word_t size, offset, i;

    // Ignore if already running in a small space.
    if (! smallspace_id_is_small (&id))
	return;

    size = smallspace_id_size (&id) >> 22;
    offset = smallspace_id_offset (&id) >> 22;

    //ENABLE_TRACEPOINT(SMALLSPACE_ENLARGE,~0,~0);

    TRACEPOINT (SMALLSPACE_ENLARGE, "make_large: space=%p (current size=%dMB  offset=%dMB)\n",
		self, size*4, offset*4);

    spinlock_lock (&small_space_owner_lock);

    // Release allocated slots
    for (i = 0; i < size; i++)
    {
	ASSERT (small_space_owner[offset + i] == self);
	small_space_owner[offset + i] = NULL;
    }

    smallspace_id_set_large (x86_space_smallid (self));

#if defined(CONFIG_X86_X32_SMALL_SPACES_GLOBAL)
    // Clear global bits for all pages in small space area.
    modify_global_bits ((space_t *) self, space_pgent ((space_t *) self, 0),
			(int) smallspace_id_size (&id), false);
#endif

    // Remove any stale pdir entries in other page tables
    spinlock_lock (&polluted_spaces_lock);

    if (polluted_spaces)
    {
	x86_space_t * s = polluted_spaces;
	x86_space_t * b = s;
	do {
	    for (word_t cpu = 0; cpu < CONFIG_SMP_MAX_CPUS; cpu++)
	    {
		if (self->data.cpu_ptab[cpu].top_pdir)
		    for (i = 0; i < size; i++)
			x86_pgent_clear (&s->data.cpu_ptab[cpu].top_pdir->small[offset + i]);
	    }
	    s = x86_space_get_next (s);
	} while (s != b);
    }

    spinlock_unlock (&polluted_spaces_lock);

    spinlock_unlock (&small_space_owner_lock);

    if (get_current_space_c () == (space_t *) self)
    {
	// Reset GDT entries to 3GB limit.
	extern x86_segdesc_t gdt[];

	x86_segdesc_set_seg (&gdt[X86_UCS >> 3], 0, USER_AREA_END-1, 3, X86_SEGDESC_CODE);
	x86_segdesc_set_seg (&gdt[X86_UDS >> 3], 0, USER_AREA_END-1, 3, X86_SEGDESC_DATA);

	reload_user_segregs_c ();

	// Make sure that we run on our own page table.
	x86_mmu_set_active_pagetable
	    ((u32_t) space_get_top_pdir_phys (get_current_space_c (), tcb_get_cpu (get_current_tcb ())));

	// Make sure that there are no stale TLB entries.
	x86_mmu_flush_tlb (FLUSH_GLOBAL);

	// Inform thread switch code that we run in a large space.
	__is_small = 0;
    }

#if defined(CONFIG_SMP)
    // Perform TLB shootdown on remote CPUs.
    for (word_t cpu = 0;
	 cpu < processor_info_get_num_processors (&get_kip ()->processor_info);
	 cpu++)
    {
	if (cpu == get_current_cpu ())
	    continue;
	xcpu_request (cpu, do_xcpu_flush_tlb);
    }
#endif
}


/**
 * Dequeue space from the list of polluted spaces.
 */
void x86_space_dequeue_polluted (x86_space_t * self)
{
    spinlock_lock (&polluted_spaces_lock);

    if (x86_space_get_next (self) == NULL)
    {
	// Space is not in list.
    }
    else if (x86_space_get_next (self) == self)
    {
	// Space is only member of list.
	polluted_spaces = NULL;
    }
    else
    {
	// Fixup pointers of neighbor spaces.
	x86_space_t * n = x86_space_get_next (self);
	x86_space_t * p = x86_space_get_prev (self);
	x86_space_set_prev (n, p);
	x86_space_set_next (p, n);
	if (polluted_spaces == self)
	    polluted_spaces = n;
    }

    x86_space_set_prev (self, NULL);
    x86_space_set_next (self, NULL);

    spinlock_unlock (&polluted_spaces_lock);
}


/**
 * Enqueue into list of polluted spaces.
 */
void x86_space_enqueue_polluted (x86_space_t * self)
{
    spinlock_lock (&polluted_spaces_lock);

    if (x86_space_get_next (self) != NULL)
    {
	// Space already in list.
    }
    else if (polluted_spaces != NULL)
    {
	// Insert into list.
	x86_space_t * p = x86_space_get_prev (polluted_spaces);
	x86_space_t * n = polluted_spaces;

	x86_space_set_prev (n, self);
	x86_space_set_next (p, self);
	x86_space_set_prev (self, p);
	x86_space_set_next (self, n);
    }
    else
    {
	// Space is first one in list.
	x86_space_set_prev (self, self);
	x86_space_set_next (self, self);
	polluted_spaces = self;
    }

    spinlock_unlock (&polluted_spaces_lock);
}


/**
 * Synchronize page table of small space with page table of current
 * space.  That is, copy page directory entry from original page table
 * into small space area of current page table.
 *
 * @param faddr		fault address (in small space area)
 *
 * @return true if a valid page directory entry was copied
 */
bool x86_space_sync_smallspace (x86_space_t * self, addr_t faddr)
{
    word_t size = X86_PGSIZE_MAX;

    // Get space which fault occured in.
    space_t * sspace = (space_t *) self;
    space_t * fspace = space_top_pdir_to_space (x86_mmu_get_active_pagetable());

    // Calculate real fault address.
    addr_t addr = addr_offset (faddr, 0 - x86_space_smallspace_offset (self));

    pgent_t * pgent_s = space_pgent (sspace, page_table_index (size, addr));
    pgent_t * pgent_f = space_pgent (fspace, page_table_index (size, faddr));

    // Copy page directory entry if it is valid.
    if (pgent_is_valid (pgent_s, sspace, size) && ! pgent_is_valid (pgent_f, fspace, size))
    {
	TRACEPOINT(SMALLSPACE_SYNC,
		   "smallspace_sync (%t, cr3 %p): s=%p:v=%p (p=%p) => s=%p:v=%p (p=%p)\n",
		   get_current_tcb(), x86_mmu_get_active_pagetable(),
		   self, addr, pgent_s, fspace, faddr, pgent_f);

	*pgent_f = *pgent_s;
	pgent_sync (pgent_f, fspace, size);

	// Mark fault space as polluted with small space entries.
	space_enqueue_polluted (fspace);
	return true;
    }

    return false;
}
