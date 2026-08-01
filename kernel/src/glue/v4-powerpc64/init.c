/****************************************************************************
 *
 * Copyright (C) 2003-2004,  National ICT Australia (NICTA)
 *
 * File path:	glue/v4-powerpc64/init.c
 * Description:	Kernel second stage initialization.
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
 * $Id: init.cc,v 1.13 2006/11/17 17:04:18 skoglund Exp $
 *
 ***************************************************************************/

#include <debug.h>
#include <kmemory.h>
#include <mapping.h>


// Debug consoles
#if defined(CONFIG_KDB_CONS_RTAS)
extern void init_rtas_console(void);
#endif
extern void init_serial_console(void);

#include INC_PLAT(prom.h)

#include INC_ARCH(msr.h)
#include INC_ARCH(string.h)
#include INC_ARCH(cache.h)
#include INC_ARCH(ppc64_registers.h)

#include INC_API(tcb.h)
#include INC_API(kernelinterface.h)
#include INC_API(schedule.h)
#include INC_API(cpu.h)

#include INC_GLUE(intctrl.h)
#include INC_GLUE(timer.h)
#include INC_ARCH(segment.h)

DECLARE_KMEM_GROUP(kmem_cpu);

/* api/v4 refers to this; glue/v4-powerpc keeps it in cpu.c, which powerpc64
   has no equivalent of.  Uniprocessor is the only configuration this port
   has. */
word_t cpu_count = 1;

/*****************************************************************************
 *
 *                            Kip init
 *
 *****************************************************************************/

SECTION(".init") addr_t kip_get_phys_mem( kernel_interface_page_t *kip )
    /* Search through the kip's memory descriptors for the size
     * of physical memory.  We assume that physical memory always starts at 0.
     */
{
    addr_t max = 0;
    word_t i;

    max = kip->main_mem.high;

    for( i = 0; i < memory_info_get_num_descriptors (&kip->memory_info); i++ )
    {
	memdesc_t *mdesc = memory_info_get_memdesc( &kip->memory_info, i );
	if( (memdesc_type (mdesc) == MEMDESC_CONVENTIONAL)
		&& !memdesc_is_virtual (mdesc)
		&& (memdesc_high (mdesc) > max) )
	{
	    max = memdesc_high (mdesc);
	}
    }

    return max;
}

SECTION(".init") static void kip_mem_init( kernel_interface_page_t *kip )
{
    extern char _start_kernel_phys[];
    extern char _end_kernel_phys[];

    // Define the user's virtual address space.
    memory_info_insert( &kip->memory_info, MEMDESC_CONVENTIONAL, 0, true,
	    (addr_t)0, (addr_t)USER_AREA_END );

    // Define the area reserved for the exception vectors.
    memory_info_insert( &kip->memory_info, MEMDESC_RESERVED, 0, false,
	    (addr_t)0, (addr_t)KERNEL_PHYS_START );

    // Define the area reserved for kernel code.
    memory_info_insert( &kip->memory_info, MEMDESC_RESERVED, 0, false,
	    _start_kernel_phys, _end_kernel_phys );

    // Reserve all other physical memory
    memory_info_insert( &kip->memory_info, MEMDESC_RESERVED, 0, false,
	    addr_align_up (kip->main_mem.high, KB(4)), (addr_t)~0ul);

    TRACEF( "Inserted kernel regions\n" );
}

/*****************************************************************************
 *
 *                More init functions, run on the boot stack
 *
 *****************************************************************************/

SECTION(".init")
word_t find_memory_area( word_t size )
{
    word_t phys_start = 0;
    bool busy = true;
    word_t i;

    for (; busy; phys_start += POWERPC64_PAGE_SIZE)
    {
	kernel_interface_page_t *kip = get_kip();
    
	word_t phys_end = phys_start + size;

	if ((word_t)get_kip()->sigma0.mem_region.high > phys_start)
	    continue;
	if ((word_t)get_kip()->sigma1.mem_region.high > phys_start)
	    continue;
	if ((word_t)get_kip()->root_server.mem_region.high > phys_start)
	    continue;

	busy = false;
	// Walk through the KIP's memory descriptors and search for any
	// reserved memory regions that collide with our intended memory
	// allocation.
	for( i = 0; i < memory_info_get_num_descriptors (&kip->memory_info); i++ )
	{
	    memdesc_t *mdesc = memory_info_get_memdesc( &kip->memory_info, i );
	    word_t low, high;

	    if( (memdesc_type (mdesc) == MEMDESC_CONVENTIONAL) || memdesc_is_virtual (mdesc) )
		continue;

	    low = (word_t)memdesc_low (mdesc);
	    high = (word_t)memdesc_high (mdesc);

	    if( (phys_start < low) && (phys_end > high) )
		{ busy = true; break; }
	    if( (phys_start >= low) && (phys_start < high) )
		{ busy = true; break; }
	    if( (phys_end > low) && (phys_end <= high) )
		{ busy = true; break; }
	}
    }
    return phys_start;
}


SECTION(".init") static void do_kmem_init(void)
{
    word_t bootmem_size = CONFIG_BOOTMEM_PAGES << POWERPC64_PAGE_BITS;

    word_t bootmem_start = phys_to_virt(find_memory_area( bootmem_size ));
    word_t bootmem_end = bootmem_start + bootmem_size;

    kmem_init(&kmem,  (addr_t)bootmem_start, (addr_t)bootmem_end );

    // Define the area reserved for the exception vectors.
    memory_info_insert( &get_kip()->memory_info, MEMDESC_RESERVED, 0, false,
	    (addr_t)virt_to_phys( bootmem_start ),
	    (addr_t)virt_to_phys( bootmem_end ) );
}

#if defined(CONFIG_SMP)
# define CPU_SPILL_SIZE	(POWERPC64_CACHE_LINE_SIZE * CONFIG_SMP_MAX_CPUS)
#else
# define CPU_SPILL_SIZE	(POWERPC64_CACHE_LINE_SIZE)
#endif
#if (POWERPC64_CACHE_LINE_SIZE < 64)
# error "Expecting a cache line size of 64-bytes or larger."
#endif
static char spill_area[CPU_SPILL_SIZE] __attribute__ ((aligned(POWERPC64_CACHE_LINE_SIZE)));

static SECTION(".init") void cpu_init( cpuid_t cpu )
{
    /* Give the cpu some extra storage to spill state during an exception.
     * The storage must be safe to access at any time via a physical address.
     * Each cpu requires its own spill area, on non-conflicting cache lines.
     */
    char *cpu_spill = &spill_area[ cpu * POWERPC64_CACHE_LINE_SIZE ];
    int i;

    for (i=0; i<CPU_SPILL_SIZE; i++)
	spill_area[i] = 0;

    ppc64_set_sprg( SPRG_LOCAL, (word_t)cpu_spill );
    ppc64_set_sprg( SPRG_TCB, (word_t)get_idle_tcb_c ());
}

static SECTION(".init") void cpulocal_init( cpuid_t cpu )
{
    extern char _start_cpu_[];
    extern char _end_cpu_[];
    addr_t cpu_area = kmem_alloc(&kmem,  kmem_cpu, (word_t)
		    addr_align_up( (addr_t)(_end_cpu_ - _start_cpu_), POWERPC64_PAGE_SIZE ) );

    pgent_t pg;
    word_t i;

    /* We assume cpu local area is less than 256 MB */
    /* XXX - this must change for SMP - assembler handled */
    segment_insert_entry( get_kernel_space(),
		    space_get_vsid( get_kernel_space(), (addr_t)KERNEL_CPU_OFFSET ),
		    ESID( (word_t)KERNEL_CPU_OFFSET ), false );

    for ( i=0; i < (word_t)addr_align_up(
		(addr_t)( _end_cpu_ - _start_cpu_), POWERPC64_PAGE_SIZE );
		i+= POWERPC64_PAGE_SIZE )
    {
	/* Create a dummy page table entry */
	pgent_set_entry( &pg, get_kernel_space(), size_4k,
			virt_to_phys((addr_t)((word_t)cpu_area + i)),
		      6, (l4default), true );
	/* Insert the kernel mapping, bolted */
	pghash_insert_mapping_bolted( get_pghash(), get_kernel_space(),
			(addr_t)(KERNEL_CPU_OFFSET + i),
			&pg, size_4k, true );
    }

    TRACEF( "Allocated cpu(%d) area %p\n", cpu, cpu_area );
}


SECTION(".init") static void finish_api_init( void )
{
    timer_init_global ();

#if defined(CONFIG_SMP)
    init_cpu( boot_cpu, boot_buskhz, boot_cpukhz );
#else
    init_cpu( 0, boot_buskhz, boot_cpukhz );
#endif

    intctrl_init_arch ();

    timer_init_cpu (get_timer ());
}

static SECTION(".init") void install_exception_handlers( void )
{
    /* Deactivate machine check exceptions while we install the exception
     * vectors.  We need valid exception vectors to handle machine check
     * exceptions.
     */
    word_t msr;
    extern char _except_start_[];
    extern char _except_end_[];

    msr = ppc64_get_msr();
    msr = msr & (~MSR_ME);
    ppc64_set_msr( msr );
    isync();	// Enable the msr change.

    memcpy_cache_flush( (word_t *)(KERNEL_OFFSET),
	    (word_t *)_except_start_,
	    (word_t)_except_end_ - (word_t)_except_start_ );
 
    /* Reenable machine check exceptions.
     */
    msr = MSR_KERNEL_MODE;	/* Now we can be in real kernel mode */
    ppc64_set_msr( msr );
    isync();
}

extern void init_plat (word_t);

extern void early_kernel_map (void);

/****************************************************************************
 *
 *                  The kernel's C entry point.
 *
 ****************************************************************************/

SECTION(".init") void start_kernel( word_t r3, word_t r4, word_t ofentry )
{
    /* We are called either real or virtual mode :(
     * First thing is to initialise the platform and map kernel data
     * to enable relocated operation
     */
    kernel_interface_page_t *kip;

    init_plat( ofentry );

    kip = PTRRELOC(get_kip());

    /* Setup the Hash Page Table */
    if( !pghash_init( PTRRELOC(get_pghash()), (word_t)kip_get_phys_mem(kip)) )
	prom_exit( "unable to find a suitable location for the page hash." );

    prom_puts( "Inserting kernel bolted hash table entry\n\r" );
    early_kernel_map();

#if defined(CONFIG_KDB_CONS_RTAS)
    init_rtas_console();
#endif

    /* Install the Hash Page Table and jump to virtmode_call() */
    ppc64_htab_activate( pghash_get_htab( PTRRELOC(get_pghash()) ) );

    /* We should never get here */
    while (1);
}


void switch_console( const char *name );
SECTION(".init") void virtmode_call(void)
{
    /* --- Running in MAPPED VIRTUAL MODE from here! --- */

    cpu_init( 0 );

    install_exception_handlers();

    /* Init kdb.  It should be completely independent of the kernel.
     */
    if( get_kip()->kdebug_init )
	get_kip()->kdebug_init();

    do_kmem_init();
    kip_mem_init( get_kip() );

    cpulocal_init( 0 );

#if defined(CONFIG_KDB_CONS_RTAS)
    printf( "L4 - Pistachio  \n" );
    printf( "PPC64 %dMHz   ", boot_cpukhz/1000 );
#endif

#if defined(CONFIG_DEBUG)
    init_serial_console();
#endif

    init_hello();

    /* initialize kernel interface page syscalls */
    kernel_interface_page_init (get_kip());

    init_mdb();
    init_kernel_space ();

    /* Initialize the idle tcb, and push notify frames for starting
     * the idle thread. */
    sched_init (true);

    /* Push a notify frame for the second stage of initialization, which
     * executes in the context of the idle thread.  This must execute
     * before the scheduler's notify frames. */
    tcb_notify (get_idle_tcb_c (), finish_api_init );
    sched_start (0); /* Does not return. */

    /* we should never get here! */
    while (1);
}

