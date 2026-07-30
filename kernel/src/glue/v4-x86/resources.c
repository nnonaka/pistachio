/*********************************************************************
 *
 * Copyright (C) 2007-2008, 2010,  Karlsruhe University
 *
 * File path:     glue/v4-x86/resources.cc
 * Description:
 *
 * @LICENSE@
 *
 * $Id:$
 *
 ********************************************************************/
#include INC_API(tcb.h)
#include INC_ARCH(fpu.h)
#include INC_ARCH(mmu.h)
#include INC_GLUE(space.h)
#include INC_GLUE(resource_functions.h)
#include <kdb/tracepoints.h>

//#define FPU_REENABLE
static tcb_t * fpu_owner UNIT("cpulocal");


DECLARE_KMEM_GROUP (kmem_resources);

#ifdef FPU_REENABLE
DECLARE_TRACEPOINT(X86_FPU_REENABLE);
#endif

/* Defined below; called by the save/purge/free paths (and, via the __asm__
   label tcb_resources_release_copy_area, from C++ in x64/tcb.h). */
void tcb_resources_release_copy_area(thread_resources_t * self, tcb_t * tcb,
				     bool disable_copyarea);


void tcb_resources_save(thread_resources_t * self, tcb_t * tcb)
{
    if (resource_bits_have_resource (&tcb->resource_bits, FPU))
    {
	x86_fpu_disable();
#ifndef FPU_REENABLE
	resource_bits_remove (&tcb->resource_bits, FPU);
#endif
    }

    if (resource_bits_have_resource (&tcb->resource_bits, COPY_AREA))
	tcb_resources_release_copy_area (self, tcb, false);
}


void tcb_resources_load(thread_resources_t * self, tcb_t * tcb)
{
    (void) self;

    if (resource_bits_have_resource (&tcb->resource_bits, COPY_AREA))
    {
	// If we had a nested pagefault, the saved partner will be our
	// real communication partner.  For other types of IPC copy
	// interruptions, the saved_partner will be nil.
	threadid_t saved = tcb->misc.saved_state[0].partner;	/* get_saved_partner() */
	threadid_t ptid = threadid_is_nilthread (&saved) ? tcb->partner : saved;
	tcb_t *partner = tcb_get_tcb (ptid);

	for (word_t i = 0; i < COPY_AREA_COUNT; i++)
	    space_populate_copy_area (tcb->space, i, tcb, partner->space, tcb->cpu);
    }

#if defined(CONFIG_SMP)
    if (resource_bits_have_resource (&tcb->resource_bits, SMP_PAGE_TABLE))
    {
	/* Thread migrated to a processor where no first level ptab is
	 * allocated yet.  Using a resource flag ensures that the
	 * memory comes from the processor local pool (NUMA).  We
	 * already run in the thread context and therefore need a
	 * valid ptab.  We use the kernel space when migrating
	 * (space_t::move_tcb) and fix it up later. */
	if (!space_has_cpu_top_pdir (tcb->space, get_current_cpu()))
	    space_alloc_cpu_top_pdir (tcb->space, get_current_cpu());

	ASSERT(space_get_top_pdir_phys (tcb->space, get_current_cpu()));
	tcb->pdir_cache = space_get_top_pdir_phys (tcb->space, get_current_cpu());
	x86_mmu_set_active_pagetable (tcb->pdir_cache);
	resource_bits_remove (&tcb->resource_bits, SMP_PAGE_TABLE);
    }
#endif

    /* CONFIG_X86_SMALL_SPACES (IPC_PAGE_TABLE) and FPU_REENABLE are off in this
       configuration, so their branches are omitted here. */
}


void tcb_resources_purge(thread_resources_t * self, tcb_t * tcb)
{
    if (fpu_owner == tcb)
    {
	x86_fpu_enable();
	x86_fpu_save_state(self->fpu_state);
	fpu_owner = NULL;
	x86_fpu_disable();
#ifdef FPU_REENABLE
	resource_bits_remove (&tcb->resource_bits, FPU);
#endif
    }

    if (resource_bits_have_resource (&tcb->resource_bits, COPY_AREA))
	tcb_resources_release_copy_area (self, tcb, false);

#if defined(CONFIG_X_X86_HVM)
    if (resource_bits_have_resource (&tcb->resource_bits, HVM))
	arch_hvm_ktcb_disable_hvm (&tcb->arch.hvm);
#endif
}


void tcb_resources_init(thread_resources_t * self, tcb_t * tcb)
{
    resource_bits_init (&tcb->resource_bits);
    self->fpu_state = NULL;

    self->last_copy_area = 0;
    /* pdir_idx is [COPY_AREA_COUNT][COPY_AREA_PDIRS] -- the loop bounds used
       to be the other way round, which on x64 (1x2) wrote one row past the
       array and left pdir_idx[0][1] uninitialised. */
    for (word_t i = 0; i < COPY_AREA_COUNT; i++)
	for (word_t j = 0;  j < COPY_AREA_PDIRS; j++)
	    self->pdir_idx[i][j] = ~0UL;
}


void tcb_resources_free(thread_resources_t * self, tcb_t * tcb)
{
    ASSERT(tcb);
    if (self->fpu_state)
    {
	kmem_free(&kmem, kmem_resources, self->fpu_state, x86_fpu_get_state_size());
	self->fpu_state = NULL;

	if (fpu_owner == tcb)
	{
	    fpu_owner = NULL;
	    x86_fpu_disable();
	}
    }

    if (resource_bits_have_resource (&tcb->resource_bits, COPY_AREA))
	tcb_resources_release_copy_area (self, tcb, false);
}


void tcb_resources_x86_no_math_exception(thread_resources_t * self, tcb_t * tcb)
{
    ASSERT(&tcb->resources == self);
    x86_fpu_enable();

    // if the current thread owns the fpu already do a quick exit
    if (fpu_owner != tcb)
    {
	if (fpu_owner != NULL)
	{
	    x86_fpu_save_state(fpu_owner->resources.fpu_state);
#ifdef  FPU_REENABLE
	    resource_bits_remove (&fpu_owner->resource_bits, FPU);
#endif
	}
	fpu_owner = tcb;

	if (self->fpu_state == NULL)
	{
	    self->fpu_state = kmem_alloc(&kmem, kmem_resources, x86_fpu_get_state_size());
	    x86_fpu_init();
	}
	else
	    x86_fpu_load_state(self->fpu_state);
    }

    resource_bits_add (&tcb->resource_bits, FPU);
}


/**
 * Release all copy areas.
 *
 * @param tcb			TCB of current thread
 * @param disable_copyarea	should copy area resource be disabled or not
 */
void tcb_resources_release_copy_area(thread_resources_t * self, tcb_t * tcb,
				     bool disable_copyarea)
{
    if (resource_bits_have_resource (&tcb->resource_bits, COPY_AREA))
    {
	for (word_t i = 0; i < COPY_AREA_COUNT; i++)
	    space_delete_copy_area (tcb->space, i, tcb->cpu);

	// Flush TLB to get rid of copy area TLB entries.  The C++ original
	// passed IS_SPACE_GLOBAL(partner->get_space()); that macro expands to
	// `false` when small spaces are disabled (this config), dropping the
	// lookup, and the macro itself is C++-only -- so pass false directly.
	x86_mmu_flush_tlb (false);

	if (disable_copyarea)
	{
	    resource_bits_remove (&tcb->resource_bits, COPY_AREA);
	    for (word_t i = 0; i < COPY_AREA_COUNT; i++)
		for (word_t j = 0; j < COPY_AREA_PDIRS; j++)
		    self->pdir_idx[i][j] = ~0UL;
	    self->last_copy_area = 0;
	}
    }
}
