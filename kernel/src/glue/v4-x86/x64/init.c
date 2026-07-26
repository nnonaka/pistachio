/*********************************************************************
 *
 * Copyright (C) 2002-2010,  Karlsruhe University
 *
 * File path:     glue/v4-x86/x64/init.c
 * Description:   x86-64 specific initialization
 *
 * @LICENSE@
 *
 * $Id$
 *
 ********************************************************************/

#include <debug.h>
#include <kmemory.h>
#include <mapping.h>
#include <ctors.h>


#include INC_API(smp.h)

#include INC_API(kernelinterface.h)
#include INC_API(types.h)
#include INC_API(cpu.h)
#include INC_API(schedule.h)

#include INC_ARCH(cpu.h)
#include INC_ARCH(amdhwcr.h)
#include INC_ARCH(fpu.h)
#include INC_ARCH(apic.h)
#include INC_ARCH(segdesc.h)
#include INC_ARCH_SA(tss.h)

#include INC_GLUE(intctrl.h)
#include INC_GLUE(timer.h)
#include INC_GLUE(idt.h)
#include INC_GLUE(memory.h)

#include INC_PLAT(rtc.h)
#include INC_PLAT(perfmon.h)


#if defined(CONFIG_X86_COMPATIBILITY_MODE)
#include INC_GLUE_SA(x32comp/kernelinterface.h)
#include INC_GLUE_SA(x32comp/init.h)
#endif /* defined(CONFIG_X86_COMPATIBILITY_MODE) */

/* extern asm entry points referenced by setup_msrs. */
extern void syscall_entry (void);

x86_x64_cpu_features_t boot_cpu_ft UNIT("x86.cpulocal");
x86_tss_t tss UNIT("x86.cpulocal");
bool tracebuffer_initialized UNIT("x86.cpulocal");

/* boot_cpu_ft was CTORPRIO(CTORPRIO_GLOBAL, 1) -- init_priority
   65535-(10000+1) = 55534, emitted into .init_array.55534.  init_priority is
   C++-only, so register the init function into the same slot with the C
   constructor attribute; it runs at the identical point. */
static void boot_cpu_ft_ctor (void) __attribute__((constructor(55534)));
static void boot_cpu_ft_ctor (void)
{
    x86_x64_cpu_features_init (&boot_cpu_ft);
}


struct gdt_struct {
    x86_segdesc_t segdsc[GDT_SIZE - 2];	/* 6 entries a  8 byte */
    x86_tssdesc_t tssdsc;		/* 1 entries a 16 byte */
} gdt UNIT("x86.cpulocal");

u8_t x86_x64_cache_line_size;


/* Called from glue/v4-x86/init.c (also C now). */
void check_cpu_features (void);
void setup_msrs (void);
void init_meminfo (void);
void setup_smp_boot_gdt (void);

// from glue/v4-x86/
void clear_bss (void);


/**********************************************************************
 *
 * SMP specific code and data
 *
 **********************************************************************/

#if defined(CONFIG_SMP)
x86_segdesc_t	smp_boot_gdt[3];
void setup_smp_boot_gdt (void)
{
    /* segment descriptors in long mode and legacy mode are almost identical.
     *  However, in long mode, most of the fields are ignored, thus we can set
     *  up those segments although the APs are not yet in long mode when they
     *  are used.
     */
#   define gdt_idx(x) ((x) >> 3)
  x86_segdesc_set_seg (&smp_boot_gdt[gdt_idx(X86_KCS)], (u64_t)0, X86_SEGDESC_CODE, 0, X86_SEGDESC_M_COMP, X86_SEGDESC_MSR_NONE);
  x86_segdesc_set_seg (&smp_boot_gdt[gdt_idx(X86_KDS)], (u64_t)0, X86_SEGDESC_DATA, 0, X86_SEGDESC_M_COMP, X86_SEGDESC_MSR_NONE);
#   undef gdt_idx
}
#endif



/**
 * Check CPU features
 *
 */

void SECTION(SEC_INIT) check_cpu_features(void)
{

#if 0
    boot_cpu_ft.dump_features();
#endif
    x86_x64_cache_line_size = boot_cpu_ft.l1_cache.d.dcache.l_size;
}

void SECTION(SEC_INIT) init_meminfo (void)
{

    extern word_t _memory_descriptors_size[];

    if (memory_info_get_num_descriptors (&get_kip()->memory_info) == (word_t) &_memory_descriptors_size)
    {
	TRACE_INIT("\tBootloader did not patch memory info...\n");
	get_kip()->memory_info.n = 0;
    }

    /*
     * reserve ourselves
     */

    memory_info_insert (&get_kip()->memory_info, MEMDESC_RESERVED, 0, false,
			start_text_phys, end_text_phys);

    memory_info_insert (&get_kip()->memory_info, MEMDESC_RESERVED, 0, false,
			start_bootmem_phys, end_bootmem_phys);

    memory_info_insert (&get_kip()->memory_info, MEMDESC_RESERVED, 0, false,
			start_syscalls_phys, end_syscalls_phys);

    /*
     * add user area
     */

    memory_info_insert (&get_kip()->memory_info, MEMDESC_CONVENTIONAL, 0, true,
			(void *) USER_AREA_START, (void *) USER_AREA_END);

}

/**********************************************************************
 *
 *  processor local initialization, performed by all CPUs
 *
 **********************************************************************/


/**
 * Setup global descriptor table
 *
 */

void SECTION(SEC_INIT) setup_gdt(x86_tss_t *tss, cpuid_t cpuid)
{

    /* Initialize GDT */
    x86_segdesc_set_seg (&gdt.segdsc[GDT_IDX(X86_X64_INVS)], (u64_t) 0, X86_SEGDESC_INV, 0, X86_SEGDESC_M_LONG, X86_SEGDESC_MSR_NONE);
    x86_segdesc_set_seg (&gdt.segdsc[GDT_IDX(X86_KCS)], (u64_t) 0, X86_SEGDESC_CODE, 0, X86_SEGDESC_M_LONG, X86_SEGDESC_MSR_NONE);
    x86_segdesc_set_seg (&gdt.segdsc[GDT_IDX(X86_KDS)], (u64_t) 0, X86_SEGDESC_DATA, 0, X86_SEGDESC_M_LONG, X86_SEGDESC_MSR_NONE);
    x86_segdesc_set_seg (&gdt.segdsc[GDT_IDX(X86_UCS)], (u64_t) 0, X86_SEGDESC_CODE, 3, X86_SEGDESC_M_LONG, X86_SEGDESC_MSR_NONE);
    x86_segdesc_set_seg (&gdt.segdsc[GDT_IDX(X86_UDS)], (u64_t) 0, X86_SEGDESC_DATA, 3, X86_SEGDESC_M_LONG, X86_SEGDESC_MSR_NONE);

#if defined(CONFIG_X86_COMPATIBILITY_MODE)
    gdt.segdsc[GDT_IDX(X86_UCS32)].set_seg((u64_t) 0, x86_segdesc_t::code, 3, x86_segdesc_t::m_comp);
#endif /* defined(CONFIG_X86_COMPATIBILITY_MODE) */

    /* TODO: Assertion correct ? */
    ASSERT((unsigned)(cpuid * X86_X64_CACHE_LINE_SIZE) < X86_SUPERPAGE_SIZE);

    /* Set TSS */
    x86_tssdesc_set_seg (&gdt.tssdsc, (u64_t) tss, sizeof(x86_x64_tss_t) - 1);

    /* Load descriptor registers */
    x86_descreg_t gdtr;
    x86_descreg_set (&gdtr, (word_t) &gdt, sizeof(gdt));
    x86_descreg_setdescreg (&gdtr, X86_DESCREG_GDTR);
    x86_descreg_t tr;
    x86_descreg_set_sel (&tr, X86_TSS);
    x86_descreg_setselreg (&tr, X86_DESCREG_TR);


    /*
     * As reloading fs/gs clobbers the upper 32bit of the segment descriptor
     * registers, we have to set them twice:
     * - before loading the segment selectors (otherwise #GP because of invalid segment)
     * - after reloading the segment selectors  (otherwise upper 32 bits = 0)
     * registers
     */

    x86_segdesc_set_seg (&gdt.segdsc[GDT_IDX(X86_UTCBS)], UTCB_MAPPING + (cpuid * X86_X64_CACHE_LINE_SIZE),
				           X86_SEGDESC_DATA,
				           3,
				           X86_SEGDESC_M_LONG,
				           X86_SEGDESC_MSR_GS);


    __asm__ __volatile__ ("" ::: "memory");


    /* Load segment registers */
    __asm__ __volatile__(
        "mov  %0, %%ds		\n\t"		// load data  segment (DS)
	"mov  %0, %%es		\n\t"		// load extra segment (ES)
	"mov  %0, %%ss		\n\t"		// load stack segment (SS)
	"mov  %1, %%gs		\n\t"		// load UTCB segment  (GS)
	"mov  %0, %%fs		\n\t"	        // no tracebuffer
 	"pushq  %2	      	\n\t"		// new CS
 	"pushq $1f		\n\t"		// new IP
 	"lretq			\n\t"
 	"1:			\n\t"
	: /* No Output */ : "r" (0), "r" (X86_UTCBS), "r" ((u64_t) X86_KCS)
	);


    x86_segdesc_set_seg (&gdt.segdsc[GDT_IDX(X86_UTCBS)], UTCB_MAPPING + (cpuid * X86_X64_CACHE_LINE_SIZE),
					   X86_SEGDESC_DATA,
					   3,
					   X86_SEGDESC_M_LONG,
				           X86_SEGDESC_MSR_GS);

}

/* C entry points over the cpulocal `tss` global + setup_gdt, called from
   glue/v4-x86/init.c. */
void x86_tss_setup_c (word_t kernel_ds)	{ x86_tss_setup (&tss, (u16_t) kernel_ds); }
void setup_gdt_c (cpuid_t cpuid)	{ setup_gdt (&tss, cpuid); }

/**
 * setup_msrs: initializes all model specific registers for CPU
 */
void setup_msrs (void)
{
#if defined(CONFIG_X86_FXSR)
    x86_fpu_enable_osfxsr();
#endif

    /* sysret (63..48) / syscall (47..32)  CS/SS MSR */
    x86_wrmsr(X86_X64_MSR_STAR, ((X86_SYSRETCS << 48) | (X86_SYSCALLCS << 32)));

    /* long mode syscalls MSR */
    x86_wrmsr(X86_X64_MSR_LSTAR, (u64_t)(syscall_entry));

    /* compatibility mode syscalls MSR */
#if defined(CONFIG_X86_COMPATIBILITY_MODE)
#if defined(CONFIG_X86_EM64T)
    x86_wrmsr(X86_MSR_SYSENTER_CS, X86_SYSCALLCS);
    x86_wrmsr(X86_MSR_SYSENTER_EIP, (u64_t)(sysenter_entry_32));
    x86_wrmsr(X86_MSR_SYSENTER_ESP, (u64_t)(&tss) + 4);

#else /* !defined(CONFIG_X86_EM64T) */
    x86_wrmsr(X86_X64_MSR_CSTAR, (u64_t)(syscall_entry_32));
#endif /* !defined(CONFIG_X86_EM64T) */
#endif /* defined(CONFIG_X86_COMPATIBILITY_MODE) */

    /* long mode syscall RFLAGS MASK  */
    x86_wrmsr(X86_X64_MSR_SFMASK, (u64_t)(X86_X64_SYSCALL_FLAGMASK));

    /* enable syscall/sysret in EFER */
    word_t efer = x86_rdmsr(X86_MSR_EFER);
    efer |= X86_MSR_EFER_SCE;
    x86_wrmsr(X86_MSR_EFER, efer);

}
