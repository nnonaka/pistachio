/*********************************************************************
 *
 * Copyright (C) 2007-2010,  Karlsruhe University
 *
 * File path:     glue/v4-x86/space.c
 * Description:
 *
 * @LICENSE@
 *
 * $Id:$
 *
 ********************************************************************/

#include <debug.h>
#include <kmemory.h>
#include <generic/lib.h>
#include <linear_ptab.h>
#include <kdb/tracepoints.h>

#include INC_API(tcb.h)
#include INC_API(smp.h)
#include INC_API(kernelinterface.h)
#include INC_API(cpu.h)

#include INC_ARCH(mmu.h)
#include INC_ARCH(trapgate.h)
#include INC_ARCH(pgent.h)

#include INC_GLUE(memory.h)
#include INC_GLUE(space.h)

/* for the tcb_t/time_t bridge wrappers relocated from thread.cc: sched_state
   set_timeout (was schedule.h -> the policy's schedule_functions.h, now in the
   policy's schedule.c) and acceptor_t. */
#include INC_API(schedule.h)
#include INC_API(generic-archmap.h)

#if defined(CONFIG_X86_COMPATIBILITY_MODE)
#include INC_GLUE_SA(x32comp/kernelinterface.h)
#endif

EXTERN_KMEM_GROUP  (kmem_space);
EXTERN_KMEM_GROUP (kmem_tcb);
DECLARE_KMEM_GROUP (kmem_iofp);
DECLARE_KMEM_GROUP (kmem_utcb);

space_t * kernel_space = NULL;
addr_t utcb_page = NULL;

/* Forward declarations for the (file-internal) space_t methods without a public
   space_* wrapper name, plus C forms used before their definition below. */
static void space_init_kernel_mappings (space_t *self);
void space_free_cpu_top_pdir (space_t *self, cpuid_t cpu);
fpage_t space_mapctrl (space_t *self, fpage_t fpage, mdb_ctrl_t ctrl, word_t attribute, bool unmap_all);
void space_handle_pagefault (space_t *self, addr_t addr, addr_t ip, word_t access, bool kernel);
space_t * space_top_pdir_to_space (word_t ptab);

space_t * active_cpu_space_get (cpuid_t cpu);

/* sign-extend an address to canonical form (x86_space_t::sign_extend). */
static inline word_t sign_ext (addr_t addr) { return (word_t) addr | X86_X64_SIGN_EXTENSION; }

/* C form of active_cpu_space_t (a C++ class in space.h; used only here). */
typedef struct { struct { space_t *space; char __pad[CACHE_LINE_SIZE - sizeof (space_t *)]; } active_space[CONFIG_SMP_MAX_CPUS]; } active_cpu_space_t;

/* IS_SPACE_SMALL/GLOBAL are C++-only macros in space.h (SMALL_SPACES off). */
#if !defined(IS_SPACE_SMALL)
#define IS_SPACE_SMALL(s)	false
#endif
#if !defined(IS_SPACE_GLOBAL)
#define IS_SPACE_GLOBAL(s)	false
#endif

/* asm-named space_t methods defined in glue/v4-x86/x64/space.c (§62). */
extern paddr_t space_t_sigma0_translate (addr_t addr, word_t size);
extern word_t  space_t_readmem_phys (addr_t paddr);
extern word_t  space_t_space_control (space_t *self, word_t ctrl, fpage_t kip_area, fpage_t utcb_area, threadid_t redir);

/**********************************************************************
 *
 *                    space_t implementation
 *
 **********************************************************************/

/**
 * initialize a space
 */
void space_init (space_t *self, fpage_t utcb_area, fpage_t kip_area)
{
    for (word_t addr = KERNEL_AREA_END; addr >= KERNEL_AREA_START; addr -= X86_TOP_PDIR_SIZE)
	space_sync_kernel_space (self, (addr_t) addr);

    self->base.data.kip_area = kip_area;
    self->base.data.utcb_area = utcb_area;

    /* map kip read-only to user (COMPATIBILITY_MODE / IO_FLEXPAGES off) */
    space_add_mapping (self, fpage_get_base (&kip_area), virt_to_phys ((addr_t) get_kip ()), X86_PGSIZE_4K, false, false, false, true);
}


/**
 * Map memory usable for TCB
 */
void space_allocate_tcb (space_t *self, addr_t addr)
{
#if !defined(CONFIG_STATIC_TCBS)
    addr_t page = kmem_alloc (&kmem, kmem_tcb, X86_PAGE_SIZE);
    ASSERT (page);

    /* map tcb kernel-writable, global */
    space_add_mapping (get_kernel_space_c (), addr, virt_to_phys (page), PGSIZE_KTCB,
		       true, true, true, true);

    space_flush_tlbent (self, self, addr, page_shift (PGSIZE_KTCB));
    space_sync_kernel_space (self, addr);
#endif
}

/**
 * Allocate a new UTCB
 */
utcb_t * space_allocate_utcb (space_t *self, tcb_t *tcb)
{
    ASSERT (tcb);

    addr_t utcb = (addr_t) tcb_get_utcb_location (tcb);

    /* walk ptab, to see if a page is already mapped */
    word_t size = X86_PGSIZE_MAX;
    pgent_t *pgent = space_pgent (self, page_table_index (size, utcb));

    while (size > X86_PGSIZE_4K && pgent_is_valid (pgent, self, size))
    {
	ASSERT (pgent_is_subtree (pgent, self, size));

	pgent = pgent_subtree (pgent, self, size);
	size--;
	pgent = pgent_next (pgent, self, size, page_table_index (size, utcb));
    }

    utcb_t *result;

    /* if pgent is valid a page is mapped, otherwise allocate a new one */
    if (pgent_is_valid (pgent, self, size))
	result = (utcb_t *) phys_to_virt (addr_offset (pgent_address (pgent, self, size),
						       (word_t) utcb & (~X86_PAGE_MASK)));
    else
    {
	/* allocate new UTCB page */
	addr_t page = kmem_alloc (&kmem, kmem_utcb, X86_PAGE_SIZE);
	ASSERT (page);
	space_add_mapping (self, (addr_t) utcb, virt_to_phys (page), PGSIZE_UTCB, true, false, false, true);
	result = (utcb_t *) addr_offset (page, (word_t) utcb & (~X86_PAGE_MASK));
    }

    return result;
}

space_t * space_allocate_space (void)
{
    space_t *space = (space_t *) kmem_alloc (&kmem, kmem_space, sizeof (space_t) + sizeof (x86_top_pdir_t));
    ASSERT (space);
    space->base.data.cpu_ptab[get_current_cpu ()].top_pdir = (x86_top_pdir_t *) addr_offset ((addr_t) space, sizeof (space_t));
    /* create backlink to space */
    space->base.data.cpu_ptab[get_current_cpu ()].top_pdir->space = space;
    space->base.data.reference_ptab = get_current_cpu ();
    return space;
}


void space_free_space (space_t *space)
{
#if defined(CONFIG_SMP)
    for (cpuid_t cpuid = 0; cpuid < CONFIG_SMP_MAX_CPUS; cpuid++)
	if (space->base.data.cpu_ptab[cpuid].top_pdir)
	    space_free_cpu_top_pdir (space, cpuid);
    kmem_free (&kmem, kmem_space, (addr_t) space, sizeof (space_t));
#else
    kmem_free (&kmem, kmem_space, (addr_t) space, sizeof (space_t) + sizeof (x86_top_pdir_t));
#endif
}


void space_remap_area (space_t *self, addr_t vaddr, addr_t paddr, word_t pgsize,
		       word_t len, bool writable, bool kernel, bool global)
{
    word_t psize = (pgsize == X86_PGSIZE_4K) ? X86_PAGE_SIZE : X86_SUPERPAGE_SIZE;

    /* length must be page-size aligned */
    ASSERT ((len & (psize - 1)) == 0);

    for (word_t offset = 0; offset < len; offset += psize)
	space_add_mapping (self, addr_offset (vaddr, offset), addr_offset (paddr, offset),
			   pgsize, writable, kernel, global, true);
}


/* JS: TODO nx bit */
void space_add_mapping (space_t *self, addr_t vaddr, addr_t paddr, word_t size,
			bool writable, bool kernel, bool global, bool cacheable)
{
    word_t curr_size = X86_PGSIZE_MAX;
    pgent_t *pgent = space_pgent (self, page_table_index (curr_size, vaddr));

    /* Sanity checking on page size (must be 4k or 2m) */
    if (!is_page_size_valid (size))
    {
	printf ("Mapping invalid pagesize (%dKB)\n", page_size (size) >> 10);
	enter_kdebug ("invalid page size");
	return;
    }

    /* Walk down hierarchy */
    while (size < curr_size)
    {
	/* Check if already mapped as larger mapping */
	if (pgent_is_valid (pgent, self, curr_size))
	{
	    if (!pgent_is_subtree (pgent, self, curr_size))
	    {
		/* check that alignement of virtual and physical page fits */
		ASSERT (addr_mask (vaddr, ~X86_SUPERPAGE_MASK) ==
			addr_mask (paddr, ~X86_SUPERPAGE_MASK));

		if (((addr_t) pgent_address (pgent, self, curr_size)) ==
		    addr_mask (paddr, X86_SUPERPAGE_MASK))
		{
		    return;
		}
		TRACEF ("already exsisting but inconsistend mapping %p space %p .\n",
			vaddr, self);
		ASSERT (false);
	    }
	}
	else
	{
	    pgent_make_subtree (pgent, self, curr_size, kernel);
	}
	curr_size--;
	pgent = pgent_next (pgent_subtree (pgent, self, curr_size + 1), self, curr_size, page_table_index (curr_size, vaddr));
    }

    pgent_set_entry (pgent, self, size, paddr, writable ? 7 : 5, 0, kernel);

    /* default is cacheable */
    if (!cacheable) pgent_set_cacheability (pgent, self, curr_size, false);

    /* default: kernel->global, user->non-global */
    if (kernel != global) pgent_set_global (pgent, self, curr_size, global);
}


/**
 * Release mappings that belong to the kernel (UTCB, KIP)
 */
void space_release_kernel_mapping (space_t *self, addr_t vaddr, addr_t paddr, word_t log2size)
{
    fpage_t utcb_area = self->base.data.utcb_area;
    /* Free up memory used for UTCBs */
    if (fpage_is_addr_in_fpage (&utcb_area, vaddr))
	kmem_free (&kmem, kmem_utcb, phys_to_virt (paddr), 1UL << log2size);
}


/**
 * Install a dummy TCB (read-only, fails all validity tests)
 */
void space_map_dummy_tcb (space_t *self, addr_t addr)
{
#if !defined(CONFIG_STATIC_TCBS)
    space_add_mapping (get_kernel_space_c (), addr, (addr_t) virt_to_phys (get_dummy_tcb_c ()), PGSIZE_KTCB,
		       false, true, false, true);

    space_flush_tlbent (self, self, addr, page_shift (PGSIZE_KTCB));
    space_sync_kernel_space (self, addr);
#endif
}


void space_switch_to_kernel_space (cpuid_t cpu)
{
    x86_mmu_set_active_pagetable ((word_t) space_get_top_pdir_phys (get_kernel_space_c (), cpu));
}

/**
 * Try to copy a mapping from kernel space into the current space
 */
bool space_sync_kernel_space (space_t *self, addr_t addr)
{
    if (self == get_kernel_space_c ()) return false;

    word_t size = X86_PGSIZE_MAX;
    cpuid_t cpu = get_current_cpu ();
    pgent_t *dst_pgent = space_pgent_cpu (self, page_table_index (size, addr), cpu);
    pgent_t *src_pgent = space_pgent_cpu (get_kernel_space_c (), page_table_index (size, addr), cpu);

    /* (already valid) || (kernel space invalid) */
    if (pgent_is_valid (dst_pgent, self, size) ||
	(!pgent_is_valid (src_pgent, get_kernel_space_c (), size)))
    {
	return false;
    }

#if !defined(CONFIG_SMP)
    *dst_pgent = *src_pgent;
#else
    for (unsigned c = 0; c < CONFIG_SMP_MAX_CPUS; c++)
	if (self->base.data.cpu_ptab[c].top_pdir) {
	    *space_pgent_cpu (self, page_table_index (size, addr), c) =
		*space_pgent_cpu (get_kernel_space_c (), page_table_index (size, addr), c);
	}
#endif
    return true;
}


EXTERN_TRACEPOINT (IPC_STRING_COPY);

void space_populate_copy_area (space_t *self, word_t n, tcb_t *tcb, space_t *partner, cpuid_t cpu)
{
    ASSERT (tcb);
    ASSERT (partner);
    ASSERT (cpu < CONFIG_SMP_MAX_CPUS && self->base.data.cpu_ptab[cpu].top_pdir);

    if (tcb->resources.pdir_idx[n][0] != ~0UL)
    {
	word_t pgsize = X86_PGSIZE_MAX;

	pgent_t *src_pgent = space_pgent (partner, tcb->resources.pdir_idx[n][0]);
	pgent_t *dst_pgent = space_pgent (self, page_table_index (pgsize, (addr_t) (COPY_AREA_START + n * COPY_AREA_SIZE)));

	for (word_t i = 1; i < COPY_AREA_PDIRS; i++)
	{
	    pgsize--;
	    src_pgent = pgent_next (pgent_subtree (src_pgent, partner, pgsize), partner, pgsize,
				    tcb->resources.pdir_idx[n][i]);

	    dst_pgent = pgent_next (pgent_subtree (dst_pgent, partner, pgsize), self, pgsize,
				    page_table_index (pgsize, (addr_t) (COPY_AREA_START + n * COPY_AREA_SIZE)));
	}

	for (word_t i = 0; i < (COPY_AREA_SIZE >> page_shift (pgsize)); i++)
	{
	    /* pgent_t assignment (set_entry(pgent) overload) + sync */
	    *dst_pgent = *src_pgent;
	    pgent_sync (dst_pgent, self, pgsize);
	    dst_pgent++;
	    src_pgent++;
	}
    }
}

void space_delete_copy_area (space_t *self, word_t n, cpuid_t cpu)
{
    ASSERT (cpu < CONFIG_SMP_MAX_CPUS && self->base.data.cpu_ptab[cpu].top_pdir);
    word_t pgsize = X86_PGSIZE_MAX;

    pgent_t *copy_pgent =
	space_pgent_cpu (self, page_table_index (pgsize, (addr_t) (COPY_AREA_START + n * COPY_AREA_SIZE)),
			 self->base.data.reference_ptab);

    for (word_t i = 1; i < COPY_AREA_PDIRS; i++)
	copy_pgent = pgent_next (pgent_subtree (copy_pgent, self, pgsize), self, pgsize,
				 page_table_index (pgsize, (addr_t) (COPY_AREA_START + n * COPY_AREA_SIZE)));

    for (word_t i = 0; i < (COPY_AREA_SIZE >> page_shift (pgsize)); i++)
	x86_pgent_clear (&copy_pgent->pgent);
}


/* The IO bitmap methods are at the end of this file (notes §116). */

void space_arch_free (space_t *self)
{
#if defined(CONFIG_X86_IO_FLEXPAGES)
    if (space_get_io_space (self))
    {
	/* Unmap IO-space */
	mdb_ctrl_t ctrl;
	ctrl.raw = 0;
	ctrl.unmap = ctrl.mapctrl_self = true;
	vrt_mapctrl (&space_get_io_space (self)->base, fpage_complete_arch (),
		     ctrl, 0, 0);
	space_free_io_bitmap (self);
    }
#else
    (void) self;
#endif
    /* CONFIG_X86_SMALL_SPACES off: nothing further to do. */
}

/**********************************************************************
 *
 *                         System initialization
 *
 **********************************************************************/

static void SECTION(".init.memory") space_init_kernel_mappings (space_t *self)
{
    /* we map both reserved areas into the kernel area */
    mem_region_t reg = get_kip ()->reserved_mem0;
    align_memregion (&reg, KERNEL_PAGE_SIZE);

    space_remap_area (self, phys_to_virt (reg.low), reg.low, PGSIZE_KERNEL, mem_region_get_size (&reg),
		      true, true, true);

    if (!mem_region_is_empty (&get_kip ()->reserved_mem1))
    {
	reg = get_kip ()->reserved_mem1;
	align_memregion (&reg, KERNEL_PAGE_SIZE);
	space_remap_area (self, phys_to_virt (reg.low), reg.low, PGSIZE_KERNEL,
			  mem_region_get_size (&reg), true, true, true);
    }

    /* map init memory */
    mem_region_set (&reg, start_init, end_init);
    align_memregion (&reg, KERNEL_PAGE_SIZE);
    space_remap_area (self, reg.low, reg.low, PGSIZE_KERNEL, mem_region_get_size (&reg),
		      true, true, true);

    /* map low 4MB pages for initialization */
    mem_region_set (&reg, (addr_t) 0, (addr_t) 0x00400000);
    align_memregion (&reg, X86_SUPERPAGE_SIZE);
    space_remap_area (self, reg.low, reg.low, PGSIZE_KERNEL, mem_region_get_size (&reg),
		      true, true, false);

    /* map video mem to kernel */
    space_add_mapping (self, phys_to_virt ((addr_t) VIDEO_MAPPING), (addr_t) VIDEO_MAPPING,
		       X86_PGSIZE_4K, true, true, true, true);

    /* MYUTCB mapping: a full page for all myutcb pointers, accessed via gs:0.
       user-writable and global. */
    EXTERN_KMEM_GROUP (kmem_misc);
    utcb_page = kmem_alloc (&kmem, kmem_misc, X86_PAGE_SIZE);
    ASSERT (utcb_page);
    space_add_mapping (self, (addr_t) UTCB_MAPPING, virt_to_phys (utcb_page),
		       X86_PGSIZE_4K, true, false, true, true);

#if defined(CONFIG_SUBARCH_X64)
    /* map syscalls read-only/executable to user */
    ASSERT (((word_t) end_syscalls - (word_t) start_syscalls) <= KERNEL_PAGE_SIZE);
    space_remap_area (self, start_syscalls, virt_to_phys (start_syscalls),
		      PGSIZE_KERNEL, KERNEL_PAGE_SIZE, false, false, true);
    /* Remap 4GB physical memory (e.g. for readmem) */
    space_remap_area (self, (addr_t) REMAP_32BIT_START, 0, X86_PGSIZE_2M, REMAP_32BIT_SIZE, true, true, true);
#endif

#if defined(CONFIG_SMP) || defined(CONFIG_X86_IO_FLEXPAGES)
    for (addr_t addr = start_cpu_local; addr < end_cpu_local;
	 addr = addr_offset (addr, KERNEL_PAGE_SIZE))
    {
	word_t size = X86_PGSIZE_MAX;
	pgent_t *pgent = space_pgent_cpu (self, page_table_index (size, start_cpu_local), 0);

	while (size != PGSIZE_KERNEL)
	{
	    pgent_set_cpulocal (pgent, self, size, true);
	    pgent = pgent_subtree (pgent, self, size--);
	    ASSERT (pgent);
	    pgent = pgent_next (pgent, self, size, page_table_index (size, addr));
	}

#if defined(CONFIG_X86_PGE)
	/* HT boxes share TLB entries, thus need to make entries non-global */
	pgent_set_global (pgent, self, size, false);
	pgent_set_cpulocal (pgent, self, size, true);
#endif
    }
#endif
}


void SECTION (".init") space_init_cpu_mappings (space_t *self, cpuid_t cpu)
{
    if (cpu == 0) return;

#if defined(CONFIG_SMP)
    word_t size;

    /* CPU 0 gets the always initialized page table */
    TRACE_INIT ("\tInitialize cpu local mappings (CPU %d)\n", cpu);

    mem_region_t reg = { start_cpu_local, end_cpu_local };
    align_memregion (&reg, KERNEL_PAGE_SIZE);
    size = X86_PGSIZE_MAX;

    /* allocate kernel top pdir */
    space_alloc_cpu_top_pdir (self, cpu);

    ASSERT (self->base.data.cpu_ptab[cpu].top_pdir && self->base.data.cpu_ptab[0].top_pdir);
    memcpy (self->base.data.cpu_ptab[cpu].top_pdir->pgent + X86_TOP_PDIR_IDX (KERNEL_AREA_START),
	    self->base.data.cpu_ptab[0].top_pdir->pgent + X86_TOP_PDIR_IDX (KERNEL_AREA_START),
	    X86_TOP_PDIR_IDX (KERNEL_AREA_SIZE) * sizeof (pgent_t));

    TRACE_INIT ("\tRemapping CPU local memory %p - %p (CPU %d)\n",
		start_cpu_local, end_cpu_local, cpu);

    pgent_t *src_pgent = space_pgent_cpu (self, page_table_index (size, reg.low), 0);
    pgent_t *dst_pgent = space_pgent_cpu (self, page_table_index (size, reg.low), cpu);

    while (size > PGSIZE_KERNEL)
    {
	src_pgent = pgent_subtree (src_pgent, self, size);
	pgent_make_cpu_subtree (dst_pgent, self, size, true);
	dst_pgent = pgent_subtree (dst_pgent, self, size);

	ASSERT (src_pgent && dst_pgent);

	memcpy (dst_pgent, src_pgent, X86_PTAB_BYTES);

	/* proceed to next lower level */
	size--;
	src_pgent = pgent_next (src_pgent, self, size, page_table_index (size, reg.low));
	dst_pgent = pgent_next (dst_pgent, self, size, page_table_index (size, reg.low));
	pgent_set_cpulocal (dst_pgent, self, size, true);
    }

    /* set entries for CPU-local memory inside the current CPU's PDIR */
    for (addr_t addr = reg.low; addr < reg.high;
	 addr = addr_offset (addr, KERNEL_PAGE_SIZE))
    {
	addr_t page = kmem_alloc (&kmem, kmem_pgtab, KERNEL_PAGE_SIZE);

	ASSERT (page);

	memcpy (page, addr, KERNEL_PAGE_SIZE);
	pgent_set_entry (dst_pgent, self, size, virt_to_phys (page), 7, 8, true);
#if defined(CONFIG_X86_PGE)
	/* HT boxes share TLB entries, thus need to make entries non-global */
	pgent_set_global (dst_pgent, self, size, false);
#endif
	pgent_set_cpulocal (dst_pgent, self, size, true);
	dst_pgent = pgent_next (dst_pgent, self, size, 1);
    }

    TRACE_INIT ("\tSwitching to CPU local pagetable %p (CPU %d)\n",
		space_get_top_pdir_phys (self, cpu), cpu);
    x86_mmu_set_active_pagetable ((word_t) space_get_top_pdir_phys (self, cpu));
    x86_mmu_flush_tlb (true);
#if defined(CONFIG_SMP)
    current_cpu = cpu;
#endif
    TRACE_INIT ("\tCPU local pagetable activated %x (CPU %d)\n",
		x86_mmu_get_active_pagetable (), cpu);
#endif
}

/**
 * initialize THE kernel space
 */
void SECTION(".init.memory") space_init_kernel_space (void)
{
    ASSERT (!kernel_space);

    kernel_space = (space_t *) kmem_alloc (&kmem, kmem_space, sizeof (space_t));
    ASSERT (kernel_space);

    kernel_space->base.data.cpu_ptab[0].top_pdir =
	(x86_top_pdir_t *) kmem_alloc (&kmem, kmem_space, sizeof (x86_top_pdir_t));

    kernel_space->base.data.cpu_ptab[0].top_pdir->space = kernel_space;
    space_init_kernel_mappings (kernel_space);

    TRACE_INIT ("\tSwitching to CPU local pagetable %p (CPU %d)\n",
		space_get_top_pdir_phys (kernel_space, 0), 0);
    x86_mmu_set_active_pagetable ((word_t) space_get_top_pdir_phys (kernel_space, 0));

    TRACE_INIT ("CPU local pagetable activated %x (CPU %d)\n",
		x86_mmu_get_active_pagetable (), 0);
}


/**********************************************************************
 *
 *                    global functions
 *
 **********************************************************************/

/**
 * exc_pagefault: trap gate for ia32 pagefault handler
 */
X86_EXCWITH_ERRORCODE(exc_pagefault, 0)
{
    word_t pf = x86_mmu_get_pagefault_address ();

    space_t *space = get_current_space_c ();

    /* if the idle thread accesses the tcb area we get a pagefault with an
       invalid space, so we use CR3 to figure out the space */
    if (EXPECT_FALSE (space == NULL))
	space = space_top_pdir_to_space (x86_mmu_get_active_pagetable ());

    ASSERT (space);

    space_handle_pagefault (space,
			    (addr_t) pf,
			    (addr_t) frame->__base.regs[X86_EXC_IPREG],
			    (frame->__base.error & X86_PAGEFAULT_BITS),
			    (frame->__base.error & 4) ? false : true);
}


/**********************************************************************
 *
 *                        SMP handling
 *
 **********************************************************************/

#if defined(CONFIG_SMP)
static word_t cpu_remote_flush UNIT("cpulocal");
static word_t cpu_remote_flush_global UNIT("cpulocal");
active_cpu_space_t active_cpu_space;

#define __FLUSH_GLOBAL__	false


static void do_xcpu_flush_tlb (cpu_mb_entry_t * entry)
{
    (void) entry;
    spin (60, get_current_cpu ());
    x86_mmu_flush_tlb (__FLUSH_GLOBAL__);
}

static void flush_tlb_remote (void)
{
    for (cpuid_t cpu = 0; cpu < cpu_count; cpu++)
	if (cpu_remote_flush & (1 << cpu))
	    sync_xcpu_request (cpu, do_xcpu_flush_tlb, NULL,
			       cpu_remote_flush_global & (1 << cpu), 0, 0);
    cpu_remote_flush = 0;
    cpu_remote_flush_global = 0;
}

static void tag_flush_remote (space_t * curspace, bool force)
{
    for (cpuid_t cpu = 0; cpu < cpu_count; cpu++)
    {
	if (cpu == get_current_cpu ())
	    continue;

	if (active_cpu_space_get (cpu) == curspace || force)
	    cpu_remote_flush |= (1 << cpu);
    }
}

void space_flush_tlb (space_t *self, space_t *curspace)
{
    if (self == curspace || IS_SPACE_SMALL (self))
	x86_mmu_flush_tlb (IS_SPACE_GLOBAL (self));
    tag_flush_remote (self, false);
}

void space_flush_tlbent (space_t *self, space_t *curspace, addr_t addr, word_t log2size)
{
    (void) log2size;
    /* js: for kernel addresses, we force an immediate remote flush */
    bool force = !space_is_user_area (addr);
    if (self == curspace || IS_SPACE_SMALL (self))
	x86_mmu_flush_tlbent ((word_t) addr);

    tag_flush_remote (self, force);
    if (force)
	flush_tlb_remote ();
}


void space_end_update (void)
{
    flush_tlb_remote ();
}


/* migration fixup */
void space_move_tcb (space_t *self, tcb_t *tcb, cpuid_t src_cpu, cpuid_t dst_cpu)
{
    /* thread_resources_t::smp_xcpu_pagetable inlined */
    ASSERT (tcb);
    ASSERT (tcb_get_space (tcb));
    if (!space_has_cpu_top_pdir (tcb_get_space (tcb), dst_cpu))
    {
	tcb->pdir_cache = (word_t) space_get_top_pdir_phys (get_kernel_space_c (), dst_cpu);
	resource_bits_add (&tcb->resource_bits, SMP_PAGE_TABLE);
    }
    else
	tcb->pdir_cache = (word_t) space_get_top_pdir_phys (tcb_get_space (tcb), dst_cpu);

    atomic_inc (&self->base.data.cpu_ptab[dst_cpu].thread_count);
    atomic_dec (&self->base.data.cpu_ptab[src_cpu].thread_count);
}

void space_alloc_cpu_top_pdir (space_t *self, cpuid_t cpu)
{
    ASSERT (cpu < CONFIG_SMP_MAX_CPUS);
    ASSERT (!self->base.data.cpu_ptab[cpu].top_pdir);

    /* Allocate PML4 */
    self->base.data.cpu_ptab[cpu].top_pdir = (x86_top_pdir_t *) kmem_alloc (&kmem, kmem_space, sizeof (x86_top_pdir_t));
    ASSERT (self->base.data.cpu_ptab[cpu].top_pdir && self->base.data.cpu_ptab[self->base.data.reference_ptab].top_pdir);

    /* Copy user entries from reference ptab */
    memcpy (self->base.data.cpu_ptab[cpu].top_pdir->pgent + X86_TOP_PDIR_IDX (USER_AREA_START),
	    self->base.data.cpu_ptab[self->base.data.reference_ptab].top_pdir->pgent + X86_TOP_PDIR_IDX (USER_AREA_START),
	    X86_TOP_PDIR_IDX (USER_AREA_SIZE) * sizeof (pgent_t));

    /* Copy kernel entries from kernel space ptab */
    memcpy (self->base.data.cpu_ptab[cpu].top_pdir->pgent + X86_TOP_PDIR_IDX (KERNEL_AREA_START),
	    get_kernel_space_c ()->base.data.cpu_ptab[cpu].top_pdir->pgent + X86_TOP_PDIR_IDX (KERNEL_AREA_START),
	    X86_TOP_PDIR_IDX (KERNEL_AREA_SIZE) * sizeof (pgent_t));

    self->base.data.cpu_ptab[cpu].top_pdir->space = self;
}

void space_free_cpu_top_pdir (space_t *self, cpuid_t cpu)
{
    ASSERT (self->base.data.cpu_ptab[cpu].top_pdir);
    ASSERT (atomic_read (&self->base.data.cpu_ptab[cpu].thread_count) == 0);
    x86_top_pdir_t *pdir = self->base.data.cpu_ptab[cpu].top_pdir;
    self->base.data.cpu_ptab[cpu].top_pdir = NULL; /* mem ordering, for X86 no barrier needed */
    kmem_free (&kmem, kmem_space, (addr_t) pdir, sizeof (x86_top_pdir_t));
}

#endif /* defined(CONFIG_SMP) */


/* C entry points wrapping the copy-area / per-CPU pdir methods for resources.c
   (declared in glue/v4-x86/space.h). */
BEGIN_DECLS
x86_top_pdir_t * space_get_top_pdir (space_t *self, cpuid_t cpu)
{ return self->base.data.cpu_ptab[cpu].top_pdir; }

word_t space_get_top_pdir_phys (space_t *self, cpuid_t cpu)
{ return (word_t) virt_to_phys (&space_get_top_pdir (self, cpu)->pgent[0].pgent); }

pgent_t * x86_top_pdir_get_kernel_pdp_pgent (x86_top_pdir_t *self)
{ return pgent_subtree (&self->kernel_pdp, (struct space_t *) self, X86_PGSIZE_512G); }

x86_kernel_pdp_t * x86_top_pdir_get_kernel_pdp (x86_top_pdir_t *self)
{ return (x86_kernel_pdp_t *) x86_top_pdir_get_kernel_pdp_pgent (self); }

void active_cpu_space_set (cpuid_t cpu, space_t *s)
{ if (s) active_cpu_space.active_space[cpu].space = s; }

space_t * active_cpu_space_get (cpuid_t cpu)
{ return active_cpu_space.active_space[cpu].space; }


bool space_has_cpu_top_pdir (space_t *self, cpuid_t cpu)
{ return self->base.data.cpu_ptab[cpu].top_pdir != NULL; }
END_DECLS


/* C forms of the pgent_t methods (declared in arch/x86/pgent.h), driving the
   page tables from generic/linear_ptab_walker.c.  Translated from the C++
   methods: delegate to the x86_pgent_t bitfield C forms + pgent_sync (F2/§62).
   size is a word_t X86_PGSIZE_* value. */

/* pgent_t::__linknode_ptr (SMP form) -- the mapping-database link slot. */
static word_t * pgent_linknode_ptr (pgent_t *self, word_t pgsize)
{
    if (pgsize == X86_PGSIZE_MAX)
    {
	word_t space = ((word_t *) ((word_t) self & X86_PAGE_MASK))[SPACE_BACKLINK >> X86_TOP_PDIR_BITS];
	return (word_t *) (space + ((word_t) self & ~X86_PAGE_MASK));
    }
    else
	return (word_t *) ((word_t) self + X86_PAGE_SIZE);
}

BEGIN_DECLS
bool pgent_is_valid (pgent_t *self, struct space_t *s, word_t pgsize)
{ (void) s; (void) pgsize; return x86_pgent_is_valid (&self->pgent); }

bool pgent_is_writable (pgent_t *self, struct space_t *s, word_t pgsize)
{ (void) s; (void) pgsize; return x86_pgent_is_writable (&self->pgent); }

bool pgent_is_readable (pgent_t *self, struct space_t *s, word_t pgsize)
{ (void) s; (void) pgsize; return x86_pgent_is_valid (&self->pgent); }

bool pgent_is_executable (pgent_t *self, struct space_t *s, word_t pgsize)
{ (void) s; (void) pgsize; return !x86_pgent_is_executable (&self->pgent); }

bool pgent_is_subtree (pgent_t *self, struct space_t *s, word_t pgsize)
{ (void) s; return !(pgsize == X86_PGSIZE_4K || (pgsize == X86_PGSIZE_SUPERPAGE && x86_pgent_is_superpage (&self->pgent))); }

addr_t pgent_address (pgent_t *self, struct space_t *s, word_t pgsize)
{ (void) s; return x86_pgent_get_address (&self->pgent, pgsize); }

word_t pgent_attributes (pgent_t *self, struct space_t *s, word_t pgsize)
{ (void) s; return (x86_pgent_is_write_through (&self->pgent) ? 1 : 0) |
		  (x86_pgent_is_cache_disabled (&self->pgent) ? 2 : 0) |
		  (x86_pgent_is_pat (&self->pgent, pgsize) ? 4 : 0); }

pgent_t * pgent_subtree (pgent_t *self, struct space_t *s, word_t pgsize)
{ (void) s; (void) pgsize; return (pgent_t *) phys_to_virt (x86_pgent_get_ptab (&self->pgent)); }

pgent_t * pgent_next (pgent_t *self, struct space_t *s, word_t pgsize, word_t num)
{ (void) s; (void) pgsize; return self + num; }

struct mapnode_t * pgent_mapnode (pgent_t *self, struct space_t *s, word_t pgsize, addr_t vaddr)
{ (void) s; return (struct mapnode_t *) (*pgent_linknode_ptr (self, pgsize) ^ (word_t) vaddr); }

void pgent_clear (pgent_t *self, struct space_t *s, word_t pgsize, bool kernel, addr_t vaddr)
{ (void) vaddr; x86_pgent_clear (&self->pgent); pgent_sync (self, s, pgsize); if (!kernel) *pgent_linknode_ptr (self, pgsize) = 0; }

void pgent_make_subtree (pgent_t *self, struct space_t *s, word_t pgsize, bool kernel)
{
    word_t size = (!kernel && (pgsize == X86_PGSIZE_4K || pgsize == X86_PGSIZE_SUPERPAGE))
	? 2 * X86_PTAB_BYTES : X86_PTAB_BYTES;
    x86_pgent_set_ptab_entry (&self->pgent, virt_to_phys (kmem_alloc (&kmem, kmem_pgtab, size)),
			      X86_PAGE_USER | X86_PAGE_WRITABLE);
    pgent_sync (self, s, pgsize);
}

void pgent_make_cpu_subtree (pgent_t *self, struct space_t *s, word_t pgsize, bool kernel)
{
    (void) kernel;  /* cpu-local subtrees are always kernel */
    x86_pgent_set_ptab_entry (&self->pgent, virt_to_phys (kmem_alloc (&kmem, kmem_pgtab, X86_PTAB_BYTES)),
			      X86_PAGE_USER | X86_PAGE_WRITABLE);
    (void) s; (void) pgsize;
}

void pgent_remove_subtree (pgent_t *self, struct space_t *s, word_t pgsize, bool kernel)
{
    addr_t ptab = (addr_t) x86_pgent_get_ptab (&self->pgent);
    x86_pgent_clear (&self->pgent);
    pgent_sync (self, s, pgsize);
    word_t size = (!kernel && (pgsize == X86_PGSIZE_4K || pgsize == X86_PGSIZE_SUPERPAGE))
	? 2 * X86_PTAB_BYTES : X86_PTAB_BYTES;
    kmem_free (&kmem, kmem_pgtab, phys_to_virt (ptab), size);
}

void pgent_set_entry (pgent_t *self, struct space_t *s, word_t pgsize, paddr_t paddr, word_t rwx, word_t attrib, bool kernel)
{
    x86_pgent_set_entry (&self->pgent, paddr, pgsize,
			 (kernel ? X86_PAGE_KERNEL : X86_PAGE_USER) |
#if defined(CONFIG_X86_PGE)
			 (kernel ? X86_PAGE_GLOBAL : 0) |
#endif
			 (attrib & 1 ? X86_PAGE_WRITE_THROUGH : 0) |
			 (attrib & 2 ? X86_PAGE_CACHE_DISABLE : 0) |
#if defined(CONFIG_X86_PAT)
			 (attrib & 4 ? (1UL << (pgsize == X86_PGSIZE_4K ? 7 : 12)) : 0) |
#endif
			 (rwx & 2 ? X86_PAGE_WRITABLE : 0) |
#if defined(CONFIG_X86_NX)
			 (rwx & 1 ? 0 : X86_PAGE_NX) |
#endif
			 X86_PAGE_VALID);
    pgent_sync (self, s, pgsize);
}

void pgent_set_linknode (pgent_t *self, struct space_t *s, word_t pgsize, struct mapnode_t *map, addr_t vaddr)
{ (void) s; *pgent_linknode_ptr (self, pgsize) = (word_t) map ^ (word_t) vaddr; }

/* was pgent_t::rights */
word_t pgent_rights (pgent_t *self, struct space_t *s, word_t pgsize)
{
    (void) s; (void) pgsize;
    return ((1<<2) |
	    (x86_pgent_is_writable (&self->pgent) ? (1<<1) : 0) |
	    (x86_pgent_is_executable (&self->pgent) ? (1<<0) : 0));
}

/* was pgent_t::set_rights.  The NX branch's `(raw | X86_PAGE_NX)' is the
   original's -- `|' where `&' reads as intended.  It makes the test reduce to
   (rwx & 1), which is the wanted behaviour anyway, so this is transcribed as
   written rather than silently corrected; see notes §112. */
void pgent_set_rights (pgent_t *self, struct space_t *s, word_t pgsize, word_t rwx)
{
    bool mod = false;

    if ((rwx & 2) && ! (self->raw & X86_PAGE_WRITABLE))
    { self->raw |= X86_PAGE_WRITABLE; mod = true; }
    else if (! (rwx & 2) && (self->raw & X86_PAGE_WRITABLE))
    { self->raw &= ~X86_PAGE_WRITABLE; mod = true; }
#if defined(CONFIG_X86_NX)
    if ((rwx & 1) && (self->raw | X86_PAGE_NX))
    { self->raw &= ~X86_PAGE_NX; mod = true; }
    else if (! (rwx & 1) && ! (self->raw & X86_PAGE_NX))
    { self->raw |= X86_PAGE_NX; mod = true; }
#endif
    if (mod) pgent_sync (self, s, pgsize);
}

/* was pgent_t::set_attributes */
void pgent_set_attributes (pgent_t *self, struct space_t *s, word_t pgsize, word_t attrib)
{
    x86_pgent_set_pat (&self->pgent, attrib, pgsize);
    pgent_sync (self, s, pgsize);
}

void pgent_update_rights (pgent_t *self, struct space_t *s, word_t pgsize, word_t rwx)
{
    if (rwx & 2) self->raw |= X86_PAGE_WRITABLE;
#if defined(CONFIG_X86_NX)
    if (rwx & 1) self->raw &= ~X86_PAGE_NX;
#endif
    pgent_sync (self, s, pgsize);
}

addr_t pgent_vaddr (pgent_t *self, struct space_t *s, word_t pgsize, struct mapnode_t *map)
{ (void) s; return (addr_t) (*pgent_linknode_ptr (self, pgsize) ^ (word_t) map); }

word_t pgent_reference_bits (pgent_t *self, struct space_t *s, word_t pgsize, addr_t vaddr)
{
#if defined(CONFIG_SMP)
    if (pgsize == X86_PGSIZE_SUPERPAGE && !x86_pgent_is_cpulocal (&self->pgent))
	return pgent_smp_reference_bits (self, s, pgsize, vaddr);
#endif
    word_t rwx = 0;
    if (x86_pgent_is_accessed (&self->pgent)) rwx |= 5;
    if (x86_pgent_is_dirty (&self->pgent)) rwx |= 6;
    return rwx;
}

void pgent_reset_reference_bits (pgent_t *self, struct space_t *s, word_t pgsize)
{ self->raw &= ~(word_t) (X86_PAGE_ACCESSED | X86_PAGE_DIRTY); pgent_sync (self, s, pgsize); }

void pgent_update_reference_bits (pgent_t *self, struct space_t *s, word_t pgsize, word_t rwx)
{ (void) s; (void) pgsize; self->raw |= ((rwx >> 1) & 0x3) << 5; }

void pgent_revoke_rights (pgent_t *self, struct space_t *s, word_t pgsize, word_t rwx)
{
    if (rwx & 2) self->raw &= ~(word_t) X86_PAGE_WRITABLE;
#if defined(CONFIG_X86_NX)
    if (rwx & 1) self->raw |= X86_PAGE_NX;
#endif
    pgent_sync (self, s, pgsize);
}

void pgent_flush (pgent_t *self, struct space_t *s, word_t pgsize, bool kernel, addr_t vaddr)
{ (void) self; (void) s; (void) pgsize; (void) kernel; (void) vaddr; }

word_t pgent_idx (pgent_t *self)
{ return (((word_t) self & (word_t) (X86_PTAB_BYTES - 1)) / sizeof (pgent_t)); }

bool pgent_is_cpulocal (pgent_t *self, struct space_t *s, word_t pgsize)
{ (void) s; (void) pgsize; return x86_pgent_is_cpulocal (&self->pgent); }
END_DECLS


/* C forms of the fpage_t methods: all mem-pages here (CONFIG_X86_IO_FLEXPAGES
   off => arch_fpage always invalid); mem.x / raw are C-visible. */
BEGIN_DECLS


END_DECLS


/* space_t C API (declared in glue/v4-x86/space.h, api/v4/space.h) -- the
   inline-method wrappers; the out-of-line methods are the space_* functions
   defined above. */
BEGIN_DECLS
pgent_t * space_pgent_cpu (space_t *self, word_t num, word_t cpu)
{
    ASSERT (cpu < CONFIG_SMP_MAX_CPUS);
    if (!self->base.data.cpu_ptab[cpu].top_pdir)
	return NULL;
    return &self->base.data.cpu_ptab[cpu].top_pdir->pgent[num];
}
pgent_t * space_pgent (space_t *self, word_t num)		{ return space_pgent_cpu (self, num, self->base.data.reference_ptab); }
void      space_begin_update (void)				{ }
bool      space_does_tlbflush_pay (word_t log2size)		{ return log2size >= 28; }
fpage_t   space_get_kip_page_area (space_t *self)		{ return self->base.data.kip_area; }
fpage_t   space_get_utcb_page_area (space_t *self)		{ return self->base.data.utcb_area; }
word_t    space_sigma0_attributes (pgent_t *pg, addr_t addr, word_t size)	{ (void) pg; (void) addr; (void) size; return 0; }


bool space_is_copy_area (addr_t addr)
{ return (((word_t) sign_ext (addr)) >= COPY_AREA_START &&
	  ((word_t) sign_ext (addr)) < COPY_AREA_END); }


void space_map_sigma0 (space_t *self, addr_t addr)
{ space_add_mapping (self, addr, addr, PGSIZE_SIGMA, true, false, false, true); }


word_t space_get_copy_limit (space_t *self, addr_t addr, word_t limit)
{
    (void) self;
    word_t end = (word_t) addr + limit;

    if (space_is_user_area (addr))
    {
	/* Do not go beyond user-area boundary. */
	if (end >= USER_AREA_END)
	    return (USER_AREA_END - (word_t) addr);
    }
    else
    {
	/* Address in copy-area: do not go beyond the current copy area. */
	ASSERT (space_is_copy_area (addr));
	if (addr_align (addr, COPY_AREA_SIZE) != addr_align ((addr_t) end, COPY_AREA_SIZE))
	    return (word_t) addr_align_up (addr, COPY_AREA_SIZE) - (word_t) addr;
    }

    return limit;
}

u8_t space_get_from_user (space_t *self, addr_t addr)		{ (void) self; return *(u8_t *) (addr); }

void space_add_tcb (space_t *self, tcb_t *tcb, cpuid_t cpu)
{
    (void) tcb;
    atomic_inc (&self->base.data.thread_count);
#if defined(CONFIG_SMP)
    atomic_inc (&self->base.data.cpu_ptab[cpu].thread_count);
#endif
}
bool space_remove_tcb (space_t *self, tcb_t *tcb, cpuid_t cpu)
{
    (void) tcb;
    ASSERT (atomic_read (&self->base.data.thread_count) != 0);
    atomic_dec (&self->base.data.thread_count);
#if defined(CONFIG_SMP)
    ASSERT (atomic_read (&self->base.data.cpu_ptab[cpu].thread_count) != 0);
    atomic_dec (&self->base.data.cpu_ptab[cpu].thread_count);
#endif
    return (atomic_read (&self->base.data.thread_count) == 0);
}

space_t * space_top_pdir_to_space (word_t ptab)
{ return phys_to_virt ((x86_top_pdir_t *) ptab)->space; }


END_DECLS


/* space_t::lookup_mapping (its out-param is a word_t X86_PGSIZE_*). */
BEGIN_DECLS


END_DECLS


/* Remaining space_t method wrappers for api/v4/space.c + the tcb_t/time_t
   bridge wrappers relocated from thread.cc. */
BEGIN_DECLS
bool space_sync_kernel_space_c (space_t *self, addr_t addr)	{ return space_sync_kernel_space (self, addr); }

/* thin bridges to the asm-named x64 space_t methods (glue/v4-x86/x64/space.c). */
paddr_t space_sigma0_translate (addr_t addr, word_t size)	{ return space_t_sigma0_translate (addr, size); }
word_t  space_readmem_phys (addr_t paddr)			{ return space_t_readmem_phys (paddr); }
word_t  space_space_control (space_t *self, word_t ctrl, fpage_t kip_area, fpage_t utcb_area, threadid_t redir)
{ return space_t_space_control (self, ctrl, kip_area, utcb_area, redir); }


void align_memregion (mem_region_t *region, word_t size)
{
    region->low = (addr_t) ((word_t) region->low & ~(size - 1));
    region->high = (addr_t) (((word_t) region->high + size - 1) & ~(size - 1));
}

/* tcb_t copy-area bridges (resources copy-area cascade translated inline;
   CONFIG_X86_SMALL_SPACES off). */
addr_t tcb_copy_area_real_address (tcb_t *self, addr_t addr)
{
    word_t copyarea_num = (((word_t) addr - COPY_AREA_START) >> X86_X64_PDP_BITS) / (COPY_AREA_SIZE >> X86_X64_PDP_BITS);
    word_t raddr = 0;
    word_t pgsize = X86_PGSIZE_MAX - COPY_AREA_PDIRS + 1;
    for (word_t i = 0; i < COPY_AREA_PDIRS; i++)
	raddr |= self->resources.pdir_idx[copyarea_num][i] << page_shift (pgsize++);
    return addr_offset ((addr_t) raddr, (word_t) addr & (COPY_AREA_SIZE - 1));
}
void tcb_adjust_for_copy_area (tcb_t *self, tcb_t *dst, addr_t *saddr, addr_t *daddr)
{
    (void) saddr;
    thread_resources_t *r = &self->resources;
    word_t n = r->last_copy_area;
    ASSERT (n < COPY_AREA_COUNT);
    r->last_copy_area++;
    if (r->last_copy_area >= COPY_AREA_COUNT)
	r->last_copy_area = 0;
    word_t pgsize = X86_PGSIZE_MAX;
    for (word_t i = 0; i < COPY_AREA_PDIRS; i++)
	r->pdir_idx[n][i] = page_table_index (pgsize--, *daddr);
    bool flush = (r->pdir_idx[n][COPY_AREA_PDIRS - 1] != ~0UL);
    for (word_t i = 0; i < COPY_AREA_COUNT; i++)
	space_populate_copy_area (self->space, i, self, dst->space, tcb_get_cpu (self));
    if (flush)
	x86_mmu_flush_tlb (false);
    resource_bits_add (&self->resource_bits, COPY_AREA);
    *daddr = addr_offset ((addr_t) (COPY_AREA_START + COPY_AREA_SIZE * n), (word_t) *daddr & (page_size (pgsize + 1) - 1));
}

void reload_user_segregs_c (void)
{
    asm volatile (
	"	movl %0, %%es	\n"
	"	movl %0, %%fs	\n"
	"	movl %1, %%gs	\n"
	:
	: "r" (X86_UDS), "r" (X86_UTCBS));
}


END_DECLS
space_t * get_kernel_space_c (void)				{ return kernel_space; }

space_t * get_current_space_c (void)				{ return tcb_get_space (get_current_tcb ()); }

bool   is_privileged_space_c (space_t *space)			{ return is_privileged_space (space); }

fpage_t space_unmap_fpage (space_t *self, fpage_t fpage, bool flush, bool all)
{
    mdb_ctrl_t ctrl;
    ctrl.raw = 0;
    ctrl.mapctrl_self	= flush;
    ctrl.unmap		= fpage_is_rwx (&fpage);
    ctrl.set_rights	= !fpage_is_rwx (&fpage);
    ctrl.reset_status	= 1;
    ctrl.deliver_status	= 1;
    fpage_set_rwx (&fpage, ~fpage_get_rwx (&fpage));
    return space_mapctrl (self, fpage, ctrl, 0, all);
}
bool space_is_tcb_area (addr_t addr)
{
#if defined(CONFIG_STATIC_TCBS)
    return false;
#else
    return (((word_t) sign_ext (addr)) >= KTCB_AREA_START &&
	    ((word_t) sign_ext (addr)) < KTCB_AREA_END);
#endif
}

bool space_is_user_area (addr_t addr)
{
#if (USER_AREA_START != 0)
    return (((word_t) sign_ext (addr)) >= USER_AREA_START &&
	    ((word_t) sign_ext (addr)) < USER_AREA_END);
#else
    return (((word_t) sign_ext (addr)) < USER_AREA_END);
#endif
}
bool space_is_initialized (space_t *self)
{ fpage_t kip = self->base.data.kip_area; return !fpage_is_nil_fpage (&kip); }

bool space_is_mappable_addr (space_t *self, addr_t addr)
{
    fpage_t kip = self->base.data.kip_area, utcb = self->base.data.utcb_area;
    return space_is_user_area (addr) &&
	!fpage_is_addr_in_fpage (&kip, addr) &&
	!fpage_is_addr_in_fpage (&utcb, addr);
}

bool space_is_mappable_fpage (space_t *self, fpage_t fp)
{
    fpage_t kip = self->base.data.kip_area, utcb = self->base.data.utcb_area;
    return space_is_user_area_fpage (fp) &&
	!fpage_is_overlapping (&kip, fp) &&
	!fpage_is_overlapping (&utcb, fp);
}

/* r_size is a pgent_t::pgsize_e* on the C++ side (asm "space_lookup_mapping"):
   a 4-byte write, so the out-param is int* here (not word_t*). */
bool space_lookup_mapping (space_t *self, addr_t vaddr, pgent_t ** r_pg, int * r_size, cpuid_t cpu)
{
    pgent_t *pg = space_pgent_cpu (self, page_table_index (X86_PGSIZE_MAX, vaddr), cpu);
    word_t pgsize = X86_PGSIZE_MAX;

    for (;;)
    {
	if (!pg)
	    return false;
	else if (pgent_is_valid (pg, self, pgsize))
	{
	    if (pgent_is_subtree (pg, self, pgsize))
	    {
		if (pgsize == 0)
		    return false;

		pg = pgent_next (pgent_subtree (pg, self, pgsize), self, pgsize - 1, page_table_index (pgsize - 1, vaddr));
		pgsize--;
	    }
	    else
	    {
		if (r_pg)
		    *r_pg = pg;
		if (r_size)
		    *r_size = (int) pgsize;
		return true;
	    }
	}
	else
	    return false;
    }
    return false;
}

bool space_lookup_mapping_c (space_t *self, addr_t vaddr, pgent_t **r_pg, word_t *r_size)
{
    int sz;
    bool r = space_lookup_mapping (self, vaddr, r_pg, &sz, (cpuid_t) self->base.data.reference_ptab);
    if (r_size) *r_size = (word_t) sz;
    return r;
}

#if defined(CONFIG_X86_IO_FLEXPAGES)
/*
 * IO permission bitmap management.  Recovered from 49fab2d^ and converted;
 * see notes §116.  These sit inside CONFIG_X86_IO_FLEXPAGES, which is why the
 * C flip dropped them without any build noticing.
 */

/* was space_t::get_io_bitmap (cpuid_t cpu = current_cpu) */
addr_t space_get_io_bitmap (space_t *self, cpuid_t cpu)
{
    pgent_t *pg;
    int pgsize;

    if (space_lookup_mapping (self, x86_tss_get_io_bitmap (&tss), &pg, &pgsize, cpu))
	return phys_to_virt (pgent_address (pg, self, (word_t) pgsize));

    TRACEF("BUG: get_io_bitmap_phys returns NULL\n");
    enter_kdebug("IO-Fpage BUG?");
    return NULL;
}

INLINE io_space_t * space_io_space_slot (space_t *self)
{ return self->base.data.io_space; }

/* was space_t::get_io_space / set_io_space (inline members) */
io_space_t * space_get_io_space (space_t *self)
{
    return space_io_space_slot (self);
}

void space_set_io_space (space_t *self, io_space_t *n)
{
    self->base.data.io_space = n;
    vrt_io_set_space (n, self);
}

/*
 * installs an allocated 8k region as IO bitmap
 */
addr_t space_install_io_bitmap (space_t *self, bool create)
{
    addr_t new_bitmap = NULL;
    cpuid_t cpu = get_current_cpu ();
    addr_t io_bitmap_mapping;
    word_t top_idx;
    pgent_t *src_pgent, *dst_pgent;
    word_t size;

    if (create)
    {
	new_bitmap = kmem_alloc (&kmem, kmem_iofp, IOPERMBITMAP_SIZE);
	if (! new_bitmap)
	    return NULL;
    }
    else
    {
	ASSERT (cpu != self->base.data.reference_ptab);
	/* Get bitmap from reference page table */
	new_bitmap = space_get_io_bitmap
	    (self, (cpuid_t) self->base.data.reference_ptab);
    }
    ASSERT (new_bitmap);

    /*
     * Allocate second level pagetables.  Do not use pgent_make_subtree, as we
     * have to set it up _before_ installing it.
     */
    io_bitmap_mapping = x86_tss_get_io_bitmap (&tss);
    top_idx = page_table_index (PGENT_SIZE_MAX, io_bitmap_mapping);
    src_pgent = space_pgent_cpu (self, top_idx, cpu);
    dst_pgent = create ?
	space_pgent_cpu (self, top_idx, self->base.data.reference_ptab) :
	space_pgent_cpu (self, top_idx, cpu);
    size = PGENT_SIZE_MAX;

    while (size > PGSIZE_KERNEL)
    {
	pgent_t *new_subtree = (pgent_t *) kmem_alloc (&kmem, kmem_iofp, X86_PAGE_SIZE);

	if (new_subtree == NULL)
	{
	    kmem_free (&kmem, kmem_iofp, new_bitmap, IOPERMBITMAP_SIZE);
	    return NULL;
	}

	/* Copy all entries from original page table */
	src_pgent = pgent_subtree (src_pgent, self, size);
	ASSERT (src_pgent);

	memcpy (new_subtree, src_pgent, X86_PTAB_BYTES);

	ASSERT (dst_pgent);
	pgent_set_entry (dst_pgent, self, X86_PGSIZE_4K,
			 virt_to_phys ((addr_t) new_subtree), 7, 0, false);
	dst_pgent = pgent_subtree (dst_pgent, self, size);

	size--;

	src_pgent = pgent_next (src_pgent, self, size,
				page_table_index (size, io_bitmap_mapping));
	dst_pgent = pgent_next (dst_pgent, self, size,
				page_table_index (size, io_bitmap_mapping));
    }

    /* Set the two special entries */
    ASSERT (size == X86_PGSIZE_4K);
    pgent_set_entry (dst_pgent, self, size, virt_to_phys (new_bitmap), 4, 0, false);
#if defined(CONFIG_X86_PGE)
    pgent_set_global (dst_pgent, self, X86_PGSIZE_4K, false);
#endif

    dst_pgent = pgent_next (dst_pgent, self, size, 1);
    pgent_set_entry (dst_pgent, self, size,
		     virt_to_phys (addr_offset (new_bitmap, X86_PAGE_SIZE)), 4, 0, false);
#if defined(CONFIG_X86_PGE)
    pgent_set_global (dst_pgent, self, X86_PGSIZE_4K, false);
#endif

    space_flush_tlbent (self, get_current_space_c (), io_bitmap_mapping,
			page_shift (X86_PGSIZE_4K));
    space_flush_tlbent (self, get_current_space_c (),
			addr_offset (io_bitmap_mapping, X86_PAGE_SIZE),
			page_shift (X86_PGSIZE_4K));

    ASSERT (space_get_io_bitmap
	    (self, (cpuid_t) self->base.data.reference_ptab) == new_bitmap);

    return new_bitmap;
}


/*
 * releases the IOPBM of a task and sets the pointers back to the default IOPBM
 */
void space_free_io_bitmap (space_t *self)
{
    space_t *kspace;
    addr_t io_bitmap, io_bitmap_mapping;
    word_t top_idx, size;
    pgent_t *orig_pgent, *new_pgent;
    unsigned cpu;

    /* Do not release the default IOPBM */
    if (space_get_io_bitmap (self, current_cpu) == x86_tss_get_io_bitmap (&tss) ||
	space_get_io_bitmap (self, current_cpu) == NULL)
	return;

    kspace = get_kernel_space_c ();
    io_bitmap = space_get_io_bitmap (self, current_cpu);
    io_bitmap_mapping = x86_tss_get_io_bitmap (&tss);

    top_idx = page_table_index (PGENT_SIZE_MAX, io_bitmap_mapping);
    size = PGENT_SIZE_MAX;

    orig_pgent = pgent_subtree (space_pgent (kspace, top_idx), kspace, size);
    new_pgent = pgent_subtree (space_pgent (self, top_idx), self, size);

    /* Restore original page entry: insert the default PGT */
    for (cpu = 0; cpu < CONFIG_SMP_MAX_CPUS; cpu++)
	if (self->base.data.cpu_ptab[cpu].top_pdir)
	    pgent_set_entry (space_pgent_cpu (self, top_idx, cpu), self, size,
			     virt_to_phys ((addr_t) orig_pgent), 6, 0, true);

    /* Release private lower level pagetables. */
    while (size-- > PGSIZE_KERNEL)
    {
	addr_t subtree = (addr_t) new_pgent;
	new_pgent = pgent_next (new_pgent, self, size + 1,
				page_table_index (size, io_bitmap_mapping));
	new_pgent = pgent_subtree (new_pgent, self, size);

	/* Release the Pagetable */
	kmem_free (&kmem, kmem_iofp, subtree, X86_PTAB_BYTES);
    }

    /* Release the IOPBM */
    kmem_free (&kmem, kmem_iofp, io_bitmap, IOPERMBITMAP_SIZE);

    /* Flush the corresponding TLB entries */
    space_flush_tlbent (self, get_current_space_c (), io_bitmap_mapping,
			page_shift (X86_PGSIZE_4K));
    space_flush_tlbent (self, get_current_space_c (),
			addr_offset (io_bitmap_mapping, 4096),
			page_shift (X86_PGSIZE_4K));
}

/*
 * synchronize IOPBM across processors
 */
bool space_sync_io_bitmap (space_t *self)
{
#if defined(CONFIG_SMP)
    /* May be that we've already created a bitmap on a different cpu */

    if (get_current_cpu () == self->base.data.reference_ptab)
	return false;

    if (space_get_io_bitmap (self, current_cpu) !=
	space_get_io_bitmap (self, (cpuid_t) self->base.data.reference_ptab))
    {
	space_install_io_bitmap (self, false);
	ASSERT (space_get_io_bitmap (self, current_cpu) ==
		space_get_io_bitmap (self, (cpuid_t) self->base.data.reference_ptab));
	return true;
    }
#else
    (void) self;
#endif
    return false;
}
#endif /* CONFIG_X86_IO_FLEXPAGES */
