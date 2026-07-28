/*********************************************************************
 *
 * Copyright (C) 2002-2007,  Karlsruhe University
 *
 * File path:     glue/v4-x86/init.c
 * Description:   ia32-specific initialization
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
#include <kmemory.h>
#include <mapping.h>
#include <ctors.h>
#include <init.h>
#include <linear_ptab.h>
#include <kdb/tracepoints.h>

#include INC_GLUE(idt.h)
#include INC_GLUE(intctrl.h)
#include INC_GLUE(space.h)
#include INC_GLUE(timer.h)
#include INC_GLUE(memory.h)
#include INC_GLUE(logging.h)

#include INC_ARCH(apic.h)
#include INC_ARCH(amdhwcr.h)

#include INC_API(smp.h)
#include INC_API(kernelinterface.h)
#include INC_API(cpu.h)
#include INC_API(schedule.h)

#include INC_PLAT(perfmon.h)

#if defined(CONFIG_X86_COMPATIBILITY_MODE)
#include INC_GLUE_SA(x32comp/kernelinterface.h)
#include INC_GLUE_SA(x32comp/init.h)
#endif /* defined(CONFIG_X86_COMPATIBILITY_MODE) */

#if defined(CONFIG_X_EVT_LOGGING)
#include INC_GLUE_SA(logging.h)
#endif

/* Defined in glue/v4-x86/x64/init.cc with C linkage; the tss/gdt wrappers hide
   the cpulocal tss global and the reference-taking setup_gdt.  init_cpu (the
   3-arg per-processor init) is declared in api/v4/cpu.h. */
void setup_msrs (void);
void init_meminfo (void);
void check_cpu_features (void);
void x86_tss_setup_c (word_t kernel_ds);
void setup_gdt_c (cpuid_t cpuid);
#if defined(CONFIG_X_X86_HVM)
void setup_vmx (cpuid_t cpuid);
#endif


#if defined(CONFIG_SMP)
/**************************************************************************
 *
 * SMP functions.
 *
 *************************************************************************/
void _start_ap(void);
void setup_smp_boot_gdt(void);

spinlock_t smp_boot_lock;
/* commence to sync TSC */
spinlock_t smp_commence_lock;

#if defined(CONFIG_DEBUG)
/* C-visible forms of the debug.h reboot hooks (declared there only for C++). */
extern void x86_reset(void);
extern bool x86_reboot_scheduled;
#endif


static u8_t get_apic_id (void)
{
    return (u8_t) apic_get_id ();
}


static void smp_ap_commence (void)
{
    spinlock_unlock (&smp_boot_lock);

    /* finally we sync the time-stamp counters */
    while( spinlock_is_locked (&smp_commence_lock) );

    x86_settsc(0);
}


static void smp_bp_commence (void)
{
    // wait for last processor to call in
    spinlock_lock (&smp_boot_lock);

    // now release all at once
    spinlock_unlock (&smp_commence_lock);

    x86_settsc(0);
}


/* forward decl: the per-CPU init (renamed from the C++ glue init_cpu(void) to
   avoid clashing with the C api/v4 init_cpu(cpuid, freq, freq) in cpu.h). */
static cpuid_t init_cpu_local (void);

/**
 * startup_cpu
 */
void SECTION(SEC_INIT) startup_cpu (void)
{
#if defined(CONFIG_DEBUG)
    if (x86_reboot_scheduled)
	x86_reset();
#endif

    TRACE_INIT("\tAP processor is alive\n");
    x86_mmu_set_active_pagetable((word_t) space_get_top_pdir_phys (get_kernel_space_c(), get_current_cpu()));
    TRACE_INIT("\tAP switched to kernel ptab\n");

    // first thing -- check CPU features
    check_cpu_features();

    /* perform processor local initialization */
    cpuid_t cpuid = init_cpu_local();

    sched_init (false);
    tcb_notify (get_idle_tcb_c (), smp_ap_commence);
    sched_start (cpuid);

    spin_forever_c (cpuid);
}
#endif /* defined(CONFIG_SMP) */



void SECTION(SEC_INIT) clear_bss (void)
{
    extern u8_t _start_bss[];
    extern u8_t _end_bss[];
    for (u8_t* p = _start_bss; p < _end_bss; p++)
	*p = 0;
}

/**
 * init_bootmem: initializes the boot memory
 *
 * At system startup, a fixed amount of kernel memory is allocated
 * to allow basic initialization of the system before sigma0 is up.
 */

void SECTION(SEC_INIT) init_bootmem (void)
{
    for (addr_t p = start_bootmem; p < end_bootmem; p = addr_offset(p, sizeof(word_t)))
	* (word_t *)p = 0;

    kmem_init(&kmem, start_bootmem, end_bootmem);
    /* now do reservations */

    // Mark the kernel code as reserved
    mem_region_set (&get_kip()->reserved_mem0, start_text_phys, end_bootmem_phys);

#if defined(CONFIG_SUBARCH_X32)
    // Were we booted via RMGR?
    if (!get_kip()->main_mem.is_empty())
    {
        word_t end = (word_t)get_kip()->main_mem.high;

        /* Allocate from end of physical memory or from end of kernel
         *accessible physical memory, whatever is lower  */
	if (end < virt_to_phys (KERNEL_AREA_END))
	    end = virt_to_phys(KERNEL_AREA_END);

        get_kip()->reserved_mem1.set((addr_t) (end - ADDITIONAL_KMEM_SIZE),
                                     (addr_t) end);
    }
#endif

}

void SECTION(SEC_INIT) add_more_kmem (void)
{
    /* Scan memory descriptors for a block of reserved physical memory
     * that is within the range of (to be) contiguously mapped
     * physical memory.  If there's one, it has been set up by the
     * boot loader for us. */
    bool found = false;
    for (word_t i = 0;
         i < memory_info_get_num_descriptors (&get_kip()->memory_info);
         i++)
    {
        memdesc_t* md = memory_info_get_memdesc (&get_kip()->memory_info, i);

        if (!memdesc_is_virtual (md) &&
            (memdesc_type (md) == MEMDESC_RESERVED) &&
            (word_t) memdesc_high (md) <= KERNEL_AREA_END)
	{
	    TRACE_INIT("\tfound  %dM kmem (%x-%x) -> (%x-%x)\n",
		       memdesc_size (md) / (1024*1024), memdesc_low (md), memdesc_high (md),
		       phys_to_virt(memdesc_low (md)), phys_to_virt(memdesc_high (md)));

#if defined(CONFIG_X_EVT_LOGGING)
	    add_logging_kmem(md);
#endif

	    // Align to kernel page size
	    mem_region_t alloc = { addr_align_up(memdesc_low (md), KERNEL_PAGE_SIZE),
				   addr_align(addr_offset(memdesc_low (md), memdesc_size (md)), KERNEL_PAGE_SIZE) };

	    // If KMEM is larger than 32 MB, allocate it chunkwise
	    const word_t chunksize = 32 * 1024 * 1024;
	    word_t allocsize = 0;

	    while (mem_region_get_size (&alloc))
	    {
		if (mem_region_get_size (&alloc) >= chunksize )
		    allocsize = chunksize - (word_t) addr_mask(alloc.low, (chunksize-1));
		else
		    allocsize = mem_region_get_size (&alloc);


		// Map region kernel writable
		//TRACE("\tadd %x %dM\n", alloc.low, allocsize / (1024 * 1024));
		space_remap_area (get_kernel_space_c (),
		    phys_to_virt(alloc.low), alloc.low,
		    PGSIZE_KERNEL,
		    allocsize, true, true, true);

		// Add it to allocator
		kmem_add(&kmem, phys_to_virt(alloc.low), allocsize);

		alloc.low = addr_offset(alloc.low, allocsize);
	    }
            found = true;
        }
    }

    /* Fall back to ol'style if no memory descriptors can be
       found */
    if (!found)
    {
        if (!mem_region_is_empty (&get_kip()->reserved_mem1))
        {
            kmem_add(&kmem, phys_to_virt(get_kip()->reserved_mem1.low),
                     mem_region_get_size (&get_kip()->reserved_mem1));
        }
    }
}

#if defined(CONFIG_TRACEBUFFER)
FEATURESTRING ("tracebuffer");
tracebuffer_t * tracebuffer;
EXTERN_KMEM_GROUP (kmem_misc);
void setup_tracebuffer (void)
{
    tracebuffer = (tracebuffer_t *) kmem_alloc(&kmem, kmem_misc, TRACEBUFFER_SIZE);
    if (!tracebuffer)
        return;
    for (word_t p = 0; p < TRACEBUFFER_SIZE; p += KERNEL_PAGE_SIZE)
    {
	//TRACEF("add tbuf mapping %t -> %t\n", addr_offset(tracebuffer, p),
        //     virt_to_phys(addr_offset(tracebuffer, p)));
	space_add_mapping (get_kernel_space_c (), addr_offset(tracebuffer, p),
			   (paddr_t) virt_to_phys(addr_offset(tracebuffer, p)),
			   PGSIZE_KERNEL, true, false, true, true);
    }
    memory_info_insert (&get_kip()->memory_info, MEMDESC_RESERVED, 0, true,
			tracebuffer,
			addr_offset(tracebuffer, TRACEBUFFER_SIZE -1));

    tracebuffer_initialize (tracebuffer);
}
#endif /* CONFIG_TRACEBUFFER */

/**
 * init_cpu: initializes the processor
 *
 * this function is called once for each processor to initialize
 * the processor specific data and registers
 */
static cpuid_t SECTION(".init.cpu") init_cpu_local (void)
{
    cpuid_t cpuid = 0;

    /* configure IRQ hardware - local part
     * this has to be done before reading the cpuid since it may change
     * when having one of those broken BIOSes like ServerWorks */
    intctrl_init_cpu ();

#if defined(CONFIG_SMP)
    word_t id = get_apic_id();
    for (cpuid = 0; cpuid < cpu_count; cpuid++)
	if (cpu_get_id (cpu_get (cpuid)) == id)
	    break;
    if (cpuid > CONFIG_SMP_MAX_CPUS)
	panic("unconfigured CPU started (LAPIC id %d)\n", id);
#endif

    TRACE_INIT("\tActivating TSS (CPU %d)se\n", cpuid);
    x86_tss_setup_c (X86_KDS);


    TRACE_INIT("\tInitializing GDT (CPU %d)\n", cpuid);
    setup_gdt_c (cpuid);

    /* can take exceptions from now on,
     * idt is initialized by idt_init() in startup_system() */
    TRACE_INIT("\tActivating IDT (CPU %d)\n", cpuid);;
    idt_activate (&idt);

#if defined(CONFIG_PERFMON)

#if defined(CONFIG_TBUF_PERFMON)
    /* initialize performance monitoring counters */
    TRACE_INIT("\tInitializing Tracebuffer PMCs (CPU %d)\n", cpuid);
    setup_perfmon_cpu(cpuid);
#endif

     /* Allow performance counters for users */
    TRACE_INIT("\tEnabling performance monitoring at user level (CPU %d)\n", cpuid);
    x86_cr4_set(X86_CR4_PCE);
#endif /* defined(CONFIG_PERFMON) */

#if defined(CONFIG_X86_PGE)
    TRACE_INIT("\tEnabling global pages (CPU %d)\n", cpuid);
    x86_mmu_enable_global_pages();
#endif

#if defined(CONFIG_CPU_X86_K8)
#if defined(CONFIG_K8_FLUSHFILTER)
    TRACE_INIT("\tEnabling K8 Flush Filter\n");
    amdhwcr_enable_flushfilter ();
#else
    TRACE_INIT("\tDisabling K8 Flush Filter\n");
    amdhwcr_disable_flushfilter ();
#endif
#endif

    TRACE_INIT("\tActivating MSRS (CPU %d)\n", cpuid);
    setup_msrs();


#if defined(CONFIG_X_X86_HVM)
    /* initialize virtualization extensions */
    TRACE_INIT("\tInitializing VMX (CPU %d)\n", cpuid);
    setup_vmx(cpuid);
#endif

    TRACE_INIT("\tInitializing Timer (CPU %d)\n", cpuid);
    timer_init_cpu (get_timer(), cpuid);

    /* initialize V4 processor info */
    TRACE_INIT("\tInitializing Processor (CPU %d)\n", cpuid);
    init_cpu (cpuid, timer_get_bus_freq (get_timer()),
		    timer_get_proc_freq (get_timer()));

    /* CPU specific mappings */
    space_init_cpu_mappings (get_kernel_space_c (), cpuid);

    /* initialize logging */
#if defined(CONFIG_X_EVT_LOGGING)
    TRACE_INIT("\tInitializing EVT logging (CPU %d)\n", cpuid);
    init_logging_cpu(cpuid);
#endif


    call_cpu_ctors();

    return cpuid;
}



/**
 * Entry Point into C Kernel
 *
 * Precondition:
 *   - paging is initialized via init_paging
 *   - idempotent mapping at KERNEL_OFFSET
 *
 * The startup happens in two steps
 *   1) all global initializations are performed
 *      this includes initializing necessary devices and
 *      the kernel debugger. The kernel memory allocator
 *      is set up (see init_arch).
 *
 *   2) the boot processor itself is initialized
 */

#if defined(CONFIG_IS_64BIT)
void SECTION(".init.init64") startup_system(u32_t is_ap)
#else
    void SECTION(SEC_INIT) startup_system (void)
#endif
{
#if defined(CONFIG_IS_64BIT) && defined(CONFIG_SMP)
    /* check if we are running on the BSP or on an AP */
    if ( is_ap )
	startup_cpu();
#endif

    clear_bss();

    init_console();

    call_global_ctors();
    call_node_ctors();

    /* Build the IDT gate table.  This was formerly a CTORPRIO_GLOBAL static
       constructor run by call_global_ctors(); it is now an explicit call. */
    idt_init(&idt);

    init_hello();

    TRACE_INIT("Checking CPU features\n");
    check_cpu_features();

    TRACE_INIT("virtual memory layout:\n"
	       "\tuser area     %wx - %wx\n"
	       "\tcopy area     %wx - %wx\n"
	       "\tktcb area     %wx - %wx\n"
	       "\tkernel area   %wx - %wx\n"
	       "\tcpulocal data %wx - %wx\n"
#if defined(CONFIG_IOAPIC)
	       "\tapic area     %wx - %wx\n"
#endif
	       "\tutcb pgarea   %wx\n"
	       "\tspace link    %wx\n",
	       USER_AREA_START, USER_AREA_END,
	       COPY_AREA_START, COPY_AREA_END,
	       KTCB_AREA_START, KTCB_AREA_END,
	       KERNEL_AREA_START, KERNEL_AREA_END,
	       start_cpu_local, end_cpu_local,
#if defined(CONFIG_IOAPIC)
	       APIC_MAPPINGS_START, APIC_MAPPINGS_END,
#endif
	       UTCB_MAPPING,  SPACE_BACKLINK
	);


    /* feed the kernel memory allocator */
    init_bootmem();

    TRACE_INIT("Initializing kernel space\n");
    space_init_kernel_space ();

    TRACE_INIT("Initializing TCBs\n");
    tcb_init_tcbs ();

    TRACE_INIT("Activating TSS (Preliminary)\n");
    x86_tss_setup_c (X86_KDS);

    TRACE_INIT("Initializing GDT (Preliminary)\n");
    setup_gdt_c (0);

    TRACE_INIT("Activating IDT (Preliminary)\n");
    idt_activate (&idt);

    TRACE_INIT("Initializing kernel interface page (%p)\n", get_kip());
    kernel_interface_page_init (get_kip());

    TRACE_INIT("Adding more kernel memory\n");
    add_more_kmem();

    TRACE_INIT("Initializing memory info\n");
    init_meminfo();

    TRACE_INIT("Initializing mapping database\n");
    init_mdb();

#if defined(CONFIG_X86_IO_FLEXPAGES)
    TRACE_INIT("Initializing IO port space\n");
    init_io_space();
#endif

#if defined(CONFIG_TRACEBUFFER)
    /* allocate and setup tracebuffer */
    TRACE_INIT("Initializing Tracebuffer (%dM)\n", TRACEBUFFER_SIZE / (1024 * 1024));
    setup_tracebuffer();
#endif

    TRACE_INIT("Initializing kernel debugger\n");
    if (get_kip()->kdebug_init)
	get_kip()->kdebug_init();

    /* configure IRQ hardware - global part */
    TRACE_INIT("Initializing IRQ hardware\n");
    intctrl_init_arch ();

    /* initialize the kernel's timer source */
    TRACE_INIT("Initializing Timer\n");
    timer_init_global ();

#if defined(CONFIG_SMP)
    /* start APs on an SMP + rendezvous */
    {
	TRACE_INIT("Starting %d application processors (%p->%p)\n",
		   cpu_count, _start_ap, SMP_STARTUP_ADDRESS);

	// aqcuire commence lock before starting any processor
	spinlock_init (&smp_commence_lock, 1);

	// boot gdt
	setup_smp_boot_gdt();

	// IPI trap gates
	init_xcpu_handling ();

	// copy startup code to startup page
	for (word_t i = 0; i < X86_PAGE_SIZE / sizeof(word_t); i++)
	    ((word_t*)SMP_STARTUP_ADDRESS)[i] = ((word_t*)_start_ap)[i];

	/* at this stage we still have our 1:1 mapping at 0.  These are the
	   fixed-address BIOS warm-reset-vector writes; -Warray-bounds gives a
	   known false positive on absolute-address derefs, so silence it. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
	*((volatile unsigned short *) 0x469) = (SMP_STARTUP_ADDRESS >> 4);
	*((volatile unsigned short *) 0x467) = (SMP_STARTUP_ADDRESS) & 0xf;
#pragma GCC diagnostic pop

	word_t id = get_apic_id();

	for (cpuid_t cpuid = 0; cpuid < cpu_count; cpuid++)
        {
	    cpu_t* cpu = cpu_get (cpuid);

	    // don't start ourselfs
	    if (cpu_get_id (cpu) == id)
		continue;

	    spinlock_lock (&smp_boot_lock); // unlocked by AP
	    TRACE_INIT("Sending startup IPI to CPU#%d APIC %d\n",
		       cpuid, cpu_get_id (cpu));
	    apic_send_init_ipi (cpu_get_id (cpu), true);
            x86_wait_cycles(1000000);
	    apic_send_init_ipi (cpu_get_id (cpu), false);
	    apic_send_startup_ipi (cpu_get_id (cpu), (void(*)(void))SMP_STARTUP_ADDRESS);

#warning VU: time out on AP call in
	}
    }

#endif /* CONFIG_SMP */

    /* Initialize CPU */
    cpuid_t cpuid = init_cpu_local();

#if defined(CONFIG_SMP)
    smp_bp_commence ();
#else
    cpu_add_cpu (0);
#endif

#if defined(CONFIG_X86_COMPATIBILITY_MODE)
    TRACE_INIT("\tInitializing 32-bit kernel interface page (%p)\n", x32::get_kip());
    x32::get_kip()->init();
    init_kip_32();
#endif /* defined(CONFIG_X86_COMPATIBILITY_MODE) */

    /* initialize the scheduler */
    sched_init (true);
    /* get the thing going - we should never return */
    sched_start (cpuid);

    /* make sure we don't fall off the edge */
    spin_forever_c (cpuid);
}
