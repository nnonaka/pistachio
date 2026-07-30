/*********************************************************************
 *                
 * Copyright (C) 2007-2010,  Karlsruhe University
 *                
 * File path:     kdb/arch/x86/x86.c
 * Description:   
 *                
 * @LICENSE@
 *                
 * $Id:$
 *                
 ********************************************************************/
#include <debug.h>
#include <kdb/kdb.h>
#include <kdb/input.h>
#include INC_API(tcb.h)
#include INC_API(cpu.h)
#include INC_ARCH(cpu.h)
#include INC_ARCH(trapgate.h)
#include INC_ARCH(ioport.h)
#include INC_ARCH(segdesc.h)
#include INC_GLUE(config.h)
#include INC_GLUE(schedule.h)

/* K8 flush filter support  */
#if defined(CONFIG_CPU_X86_K8)
#include INC_ARCH(amdhwcr.h)
#endif 
#include INC_PLAT(nmi.h)
#include INC_GLUE(idt.h)
#if defined(CONFIG_IOAPIC)
#include INC_ARCH(apic.h)
#endif
#if defined(CONFIG_X_X86_HVM)
#include INC_ARCH_SA(vmx.h)
#endif

DECLARE_CMD (cmd_reset, root, '6', "reset", "Reset system");


#if defined(CONFIG_SUBARCH_X32)
#define __BITS_WORD	32	
#else
#define __BITS_WORD	64
#endif

bool x86_reboot_scheduled;

extern void x86_reset(void);
void x86_reset_wrapper(void)
{
    asm volatile (
	".global x86_reset				\n\t"			
	".type x86_reset,@function			\n\t"			
	".section .init					\n\t"			
  	"x86_reset:					\n\t"
	"movb	$0xFE, %al	        		\n\t"
	"outb	%al, $0x64	        		\n\t"
	".previous					\n\t"			
	);	
}
CMD(cmd_reset, cg)
{  
#if defined(CONFIG_IOAPIC)
    local_apic_disable();
#endif
#if defined(CONFIG_X_X86_HVM)
    // Have to disable VMX Root Mode to reboot CPU.
    if (x86_x32_vmx_is_enabled ())
	x86_x32_vmx_disable ();
#endif
    x86_reboot_scheduled = true;
    x86_reset();
    /* NOTREACHED */
    ASSERT(false);
    return CMD_NOQUIT;
}


DECLARE_CMD(cmd_show_ctrlregs, arch, 'c', "ctrlregs",
	    "show X86 control registers");

CMD(cmd_show_ctrlregs, cg)
{
    word_t cr0, cr2, cr3, cr4;
    __asm__ __volatile__ (
	"mov	%%cr0, %0	\n"
	"mov	%%cr2, %1	\n"
	"mov	%%cr3, %2	\n"
	"mov	%%cr4, %3	\n"
	: "=r"(cr0), "=r"(cr2), "=r"(cr3), "=r"(cr4));
    
    printf("CR0: %wx\n", cr0);
    printf("CR2: %wx\n", cr2);
    printf("CR3: %wx\n", cr3);
    printf("CR4: %wx\n", cr4);
    
    return CMD_NOQUIT;
}

    DECLARE_CMD (cmd_dump_msrs, arch, 'm', "dumpmsrs",
		 "dump model specific registers");

CMD (cmd_dump_msrs, cg)
{
#if defined(CONFIG_CPU_X86_I686)
     printf("LASTBRANCH_FROM_IP: %x\n", x86_rdmsr (X86_MSR_LASTBRANCHFROMIP));
     printf("LASTBRANCH_TO_IP:   %x\n", x86_rdmsr (X86_MSR_LASTBRANCHTOIP));
     printf("LASTINT_FROM_IP:    %x\n", x86_rdmsr (X86_MSR_LASTINTFROMIP));
     printf("LASTINT_TO_IP:      %x\n", x86_rdmsr (X86_MSR_LASTINTTOIP));
#endif

#if defined(CONFIG_CPU_X86_P4)
    for (int i = 0; i < 18; i++) {
	u64_t pmc = x86_rdmsr (X86_MSR_COUNTER_BASE + i);
	u64_t cccr = x86_rdmsr (X86_MSR_CCCR_BASE + i);
	printf("PMC/CCCR %02u: 0x%08x%08x/0x%08x%08x\n",
	       i,
	       (u32_t)(pmc >> 32), (u32_t)pmc,
	       (u32_t)(cccr >> 32), (u32_t)cccr);
    }
#endif

    return CMD_NOQUIT;
}


DECLARE_CMD (cmd_dump_current_frame, root, ' ', "frame",
	     "show current exception frame");

CMD (cmd_dump_current_frame, cg)
{ 
    debug_param_t * param = (debug_param_t*)kdb.kdb_param;
    x86_exceptionframe_dump (param->frame);
    return CMD_NOQUIT;
}


/**
 * cmd_ports - read or write X86's I/O space
 */
DECLARE_CMD (cmd_ports, arch, 'p', "ports", "IO port access");

CMD(cmd_ports, cg)
{
    char dir  = get_choice ("Access mode", "In/Out", 'i');
    char width = get_choice ("Access width", "Byte/Word/Dword", 'b');
    // x86 I/O port numbers are 16 bit, so truncating the input is correct
    u16_t port = (u16_t) get_hex ("Port", 0x80, NULL);

    u32_t val = 0;

    switch (dir) {
    case 'i':
	switch (width) {
	case 'b': val = in_u8(port); break;
	case 'w': val = in_u16(port); break;
	case 'd': val = in_u32(port); break;
	};
	printf("Value = %x\n", val);
	break;
    case 'o':
	// A dword is the widest possible port access; the byte and word
	// accesses below deliberately use only the low bits of the value
	val = (u32_t) get_hex ("Value", 0, NULL);
	switch (width) {
	case 'b': out_u8(port, (u8_t) val); break;
	case 'w': out_u16(port, (u16_t) val); break;
	case 'd': out_u32(port, val); break;
	}; break;
    };
    return CMD_NOQUIT;
}


/**
 * enable/disable NMI handling
 */
DECLARE_CMD (cmd_enable_nmi, arch, 'n', "enable_nmi", "enable/disable NMI in chipset");

CMD(cmd_enable_nmi, cg)  
{  
    switch (get_choice("NMI", "Enable/Disable", 'd')) 
    {
    case 'd': 
	nmi_mask(); 
	break;      
    case 'e': 
	nmi_unmask(); 
	break;    
    }
    return CMD_NOQUIT; 
} 

#if defined(CONFIG_SMP)

/**
 * send NMI via IPI to (remote) processors
 */
DECLARE_CMD (cmd_send_nmi, arch, 'N', "send_nmi", "send NMI to CPU");

CMD(cmd_send_nmi, cg)
{
    word_t cpuid = get_dec("CPU id", 0, NULL);
    // cpu_t::get() is an unchecked index into cpu_descriptors[], so the
    // user supplied id has to be validated first -- same guard as
    // cmd_switch_cpus below.
    if (cpuid >= CONFIG_SMP_MAX_CPUS ||
	!cpu_is_valid (cpu_get ((cpuid_t) cpuid)))
	return CMD_NOQUIT;
    cpu_t* cpu = cpu_get ((cpuid_t) cpuid);
    // don't nmi ourselfs
    if (cpu_get_id (cpu) == local_apic_id())
	return CMD_NOQUIT;
    local_apic_send_nmi((u8_t) cpu_get_id (cpu));
    return CMD_NOQUIT;
}

DECLARE_CMD (cmd_switch_cpus, arch, 'S', "switch_cpu", "switch CPU");

extern atomic_t kdb_current_cpu;
extern void kdb_wait_for_cpu();

CMD(cmd_switch_cpus, cg)
{
    cpuid_t cpu = get_current_cpu();
    word_t dst_cpu = get_dec("CPU id", 0, NULL);
    if (dst_cpu >= CONFIG_SMP_MAX_CPUS ||
	dst_cpu == cpu || 
	!cpu_is_valid (cpu_get ((cpuid_t) dst_cpu)))
	return CMD_NOQUIT;

    atomic_set (&kdb_current_cpu, dst_cpu);
    local_apic_send_nmi((u8_t) dst_cpu);
    
    /* Execute a dummy iret to receive NMIs again, then sleep */
    x86_iret_self();
    x86_sleep_uninterruptible();
    /* Unmask NMIs again */

    if (atomic_read (&kdb_current_cpu) == cpu)
    {
	printf("--- Switched to CPU %d ---\n", cpu);
	return CMD_ABORT;
    }
    
    return CMD_QUIT;
    
}


#endif /* defined(CONFIG_SMP) */

#if defined(CONFIG_IOAPIC)
DECLARE_CMD(cmd_show_lvt, arch, 'l', "lvt",
	    "show APIC local vector table");

CMD(cmd_show_lvt, cg)
{

    printf("  timer:   0x%8x\n", local_apic_read_vector (LAPIC_LVT_TIMER));
    printf("  lin0:    0x%8x\n", local_apic_read_vector (LAPIC_LVT_LINT0));
    printf("  lin1:    0x%8x\n", local_apic_read_vector (LAPIC_LVT_LINT1));
    printf("  error:   0x%8x\n", local_apic_read_vector (LAPIC_LVT_ERROR));
    printf("  perf:    0x%8x\n", local_apic_read_vector (LAPIC_LVT_PERFCOUNT));
    printf("  thermal: 0x%8x\n", local_apic_read_vector (LAPIC_LVT_THERMAL_MONITOR));

    return CMD_NOQUIT;
}
#endif


/* space_is_hvm_space and space_get_hvm_space are in glue/v4-x86/space.h; the
   VTLB lookup is x86_hvm_space_lookup_gphys_addr (notes §134). */
#if defined(CONFIG_X_X86_HVM)
DECLARE_CMD(cmd_dump_gva, arch, 'd', "d",
	    "dump HVM virtual address");

extern void memdump_loop (space_t * space, addr_t addr);

CMD(cmd_dump_gva, cg)
{
    word_t addr = get_hex ("Dump GV address", kdb.last_dump, NULL);
     
    if (addr == ABORT_MAGIC)
	return CMD_NOQUIT;

    kdb.last_dump = addr;

    addr_t gvaddr = (addr_t) addr;
    
    space_t *space = get_space ("Space");
    if (!space_is_hvm_space (space))
	return CMD_NOQUIT;

    addr_t gpaddr;
    
    if (! x86_hvm_space_lookup_gphys_addr (space_get_hvm_space (space), gvaddr, &gpaddr))
	return CMD_NOQUIT;
    
    memdump_loop (space, gpaddr);
    
    return CMD_NOQUIT;
}
#endif
