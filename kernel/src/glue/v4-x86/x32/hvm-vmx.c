/*********************************************************************
 *
 * Copyright (C) 2006-2007,  Karlsruhe University
 *
 * File path:     glue/v4-x86/x32/hvm-vmx.cc
 * Description:   Vanderpool Virtual Machine Extensions
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, self list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, self list of conditions and the following disclaimer in the
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
#include <kdb/tracepoints.h>
#include <debug.h>
#include INC_API(tcb.h)
#include INC_ARCH(traps.h)
#include INC_ARCH(trapgate.h)
#include INC_ARCH(segdesc.h)
#include INC_ARCH_SA(vmx.h)
#include INC_GLUE(hvm.h)
#include INC_GLUE(ipc.h)
#include INC_GLUE(idt.h)


/*
 * The 64-bit VMCS fields are read and written as two 32-bit halves by the
 * ctrlxfer items.  The macros took a `vmcs->area.field' path; they now take the
 * area and field separately and expand to the generated accessor pair.
 */
#define GET64_VMCS_LOW(area, field)  ((u32_t) vmcs_##area##_get_##field (self->vmcs))
#define GET64_VMCS_HIGH(area, field) ((u32_t) (vmcs_##area##_get_##field (self->vmcs) >> 32))

#define SET64_VMCS_LOW(area, field, val)			\
    do {							\
	u64_t r = vmcs_##area##_get_##field (self->vmcs);	\
	r &= 0xffffffff00000000ULL;				\
	r |= (val);						\
	vmcs_##area##_set_##field (self->vmcs, r);		\
    } while (0);

#define SET64_VMCS_HIGH(area, field, val)			\
    do {							\
	u64_t r = vmcs_##area##_get_##field (self->vmcs);	\
	r &= 0xffffffffULL;					\
	r |= (u64_t) (val) << 32;				\
	vmcs_##area##_set_##field (self->vmcs, r);		\
    } while (0);



FEATURESTRING ("x86-hvm");

DECLARE_TRACEPOINT(X86_HVM_EXIT);
DECLARE_TRACEPOINT(X86_HVM_EXIT_EXTINT);
DECLARE_TRACEPOINT(X86_HVM_ENTRY);
DECLARE_TRACEPOINT(X86_HVM_ENTRY_EXC);


/* Was the static member x32_hvm_vmx_t::host_dr. */
word_t x86_hvm_host_dr[8];
void vmexit_entry_point (void);
/* Declared where it is defined (glue/v4-x86/resources.c); exception.c carries
   the same local prototype. */
void tcb_resources_x86_no_math_exception (thread_resources_t *self, tcb_t *tcb);

#if defined(CONFIG_DEBUG)
extern bool x86_kdb_singlestep, x86_kdb_branchstep;
extern word_t x86_kdb_dr_mask;
extern word_t x86_kdb_last_ip;
#endif

/**********************************************************************
 *
 *                        x86 HVM -- VMX-specific functions
 *
 **********************************************************************/


void arch_hvm_ktcb_init_vmcs (arch_hvm_ktcb_t *self)
{
    tcb_t         *tcb   = addr_to_tcb(self);
    space_t       *space = tcb_get_space (tcb);

    ASSERT (space);
    ASSERT (self->vmcs);

    vmcs_init (self->vmcs);

    vmcs_hs_set_cr0 (self->vmcs, x86_cr0_read ());
    vmcs_hs_set_cr3 (self->vmcs, (word_t) space_get_top_pdir_phys (space, tcb_get_cpu (tcb)));
    vmcs_hs_set_cr4 (self->vmcs, x86_cr4_read ());


    vmcs_hs_set_rsp (self->vmcs, (word_t) tcb_get_stack_top (tcb) - 6 * sizeof (word_t));
    vmcs_hs_set_rip (self->vmcs, (word_t) vmexit_entry_point);

    /*
     * Configuration taken from init.cc.
     */

    extern x86_segdesc_t gdt[];

    vmcs_hs_set_cs_sel (self->vmcs, X86_KCS);

    vmcs_hs_set_ss_sel (self->vmcs, X86_KDS);

    // VMX does not allow user-accessible segments.
    // So we reload them manually on a VM exit.
    vmcs_hs_set_ds_sel (self->vmcs, X86_KDS);
    vmcs_hs_set_es_sel (self->vmcs, X86_KDS);

    vmcs_hs_set_gs_sel (self->vmcs, X86_KDS);
    vmcs_hs_set_gs_base (self->vmcs, x86_segdesc_get_base (&gdt[X86_KDS >> 3]));

    vmcs_hs_set_fs_sel (self->vmcs, X86_KDS);
    vmcs_hs_set_fs_base (self->vmcs, x86_segdesc_get_base (&gdt[X86_KDS >> 3]));

    vmcs_hs_set_tr_sel (self->vmcs, X86_TSS);
    vmcs_hs_set_tr_base (self->vmcs, x86_segdesc_get_base (&gdt[X86_TSS >> 3]));

    vmcs_hs_set_gdtr_base (self->vmcs, (word_t) &gdt);
    vmcs_hs_set_idtr_base (self->vmcs, (word_t) &idt);

#if defined(CONFIG_IO_FLEXPAGES)
    vmcs_hs_set_sysenter_esp (self->vmcs, ((u32_t) (TSS_MAPPING) + 4));
#else
    extern x86_x32_tss_t tss;
    vmcs_hs_set_sysenter_esp (self->vmcs, ((u32_t) (&tss)) + 4);
#endif

    vmcs_hs_set_sysenter_eip (self->vmcs, (u32_t) (exc_user_sysipc));
    vmcs_hs_set_sysenter_cs (self->vmcs, X86_KCS);


    //bit X in X86_MSR_VMX_CR0_FIXED0 = 1 -> bit must be 1
    //bit X in X86_MSR_VMX_CR0_FIXED1 = 0 -> bit must be 0
    
    
    u64_t cr0_fixed0 = x86_rdmsr (X86_MSR_VMX_CR0_FIXED0);
    u64_t cr0_fixed1 = x86_rdmsr (X86_MSR_VMX_CR0_FIXED1);
    u64_t cr4_fixed0 = x86_rdmsr (X86_MSR_VMX_CR4_FIXED0);
    u64_t cr4_fixed1 = x86_rdmsr (X86_MSR_VMX_CR4_FIXED1);
    
    
    self->guest_cr0_mask = X86_HVM_CR0_MASK & (cr0_fixed0 ^ cr0_fixed1);
    self->guest_cr4_mask = X86_HVM_CR4_MASK & (cr4_fixed0 ^ cr4_fixed1);
    // self->guest_cr4_mask |= X86_CR4_VME;
    
    word_t gcr0 = x86_cr0_read() & ~X86_CR0_WP;
    gcr0 |= cr0_fixed0 & cr0_fixed1;
    gcr0 &= cr0_fixed0 | cr0_fixed1;
    
    word_t gcr4 = x86_cr4_read() & ~X86_CR4_PAE & ~X86_CR4_VMXE;
    gcr4 |= cr4_fixed0 & cr4_fixed1;
    gcr4 &= cr4_fixed0 | cr4_fixed1;

    vmcs_gs_set_cr0 (self->vmcs, gcr0);
    vmcs_gs_set_cr3 (self->vmcs, x86_hvm_vtlb_get_active_top_pdir (&self->vtlb));
    vmcs_gs_set_cr4 (self->vmcs, gcr4);
    vmcs_gs_set_dr7 (self->vmcs, 0x400);
    
}

#if defined(CONFIG_IO_FLEXPAGES)
void arch_hvm_ktcb_set_io_pbm (arch_hvm_ktcb_t *self, addr_t paddr)
{
    vmcs_exectr_cpubased_t i_cpubased;

    i_cpubased = vmcs_exec_ctr_get_cpubased (self->vmcs);
    i_cpubased.iobitm = 1;
    vmcs_exec_ctr_set_cpubased (self->vmcs, i_cpubased);

    // Set bitmap address.
    vmcs_exec_ctr_set_iobmpa (self->vmcs, (u32_t) paddr);
    vmcs_exec_ctr_set_iobmpb (self->vmcs, (u32_t) paddr + X86_X32_PAGE_SIZE);
}
#endif




void arch_hvm_ktcb_save_guest_drs (arch_hvm_ktcb_t *self)
{
    X86_GET_DR(0, self->guest_dr[0]); X86_GET_DR(1, self->guest_dr[1]);
    X86_GET_DR(2, self->guest_dr[2]); X86_GET_DR(3, self->guest_dr[3]);
    X86_GET_DR(6, self->guest_dr[6]); self->guest_dr[7] = vmcs_gs_get_dr7 (self->vmcs);

    X86_SET_DR(0, x86_hvm_host_dr[0]); X86_SET_DR(1, x86_hvm_host_dr[1]);
    X86_SET_DR(2, x86_hvm_host_dr[2]); X86_SET_DR(3, x86_hvm_host_dr[3]);
    X86_SET_DR(6, x86_hvm_host_dr[6]); X86_SET_DR(7, x86_hvm_host_dr[7]);

}

void arch_hvm_ktcb_restore_guest_drs (arch_hvm_ktcb_t *self)
{
    X86_GET_DR(0, x86_hvm_host_dr[0]); X86_GET_DR(1, x86_hvm_host_dr[1]);
    X86_GET_DR(2, x86_hvm_host_dr[2]); X86_GET_DR(3, x86_hvm_host_dr[3]);
    X86_GET_DR(6, x86_hvm_host_dr[6]); X86_GET_DR(7, x86_hvm_host_dr[7]);

    //x86_set_kdb_dr(0, x86_bp_instr, 0xfe05b, true, true);
    
#if defined(CONFIG_DEBUG)
    if (x86_kdb_singlestep || (x86_kdb_branchstep ^ self->kdb_branchstep))
    {

	vmcs_gs_ias_t ias = vmcs_gs_get_ias (self->vmcs);
	vmcs_gs_as_t  as  = vmcs_gs_get_as (self->vmcs);
	    
	if (ias.bl_sti == 1 || ias.bl_movss == 1 || as.state == VMCS_AS_HLT)
	{
	    vmcs_gs_pend_dbg_except_t dbge = vmcs_gs_get_pend_dbg_except (self->vmcs);
	    dbge.bs = 1;
	    vmcs_gs_set_pend_dbg_except (self->vmcs, dbge);
	}
	
	x86_kdb_last_ip = vmcs_read_register (self->vmcs, VMCS_IDX_G_CS_BASE) + 
	    get_user_frame(addr_to_tcb(self))->__base.regs[X86_EXC_IPREG];

    }

    if (x86_kdb_branchstep ^ self->kdb_branchstep)
    {
	if (x86_kdb_branchstep)
	{
	    printf("enable L4-KDB branchstepping %x\n");
	    word_t dbg_ctl = GET64_VMCS_LOW(gs, dbg_ctl);
	    SET64_VMCS_LOW(gs, dbg_ctl, dbg_ctl | 0x3);
	}
	else 
	{
	    printf("disable L4-KDB branchstepping %x\n");
	    word_t dbg_ctl = GET64_VMCS_LOW(gs, dbg_ctl);
	    SET64_VMCS_LOW(gs, dbg_ctl, dbg_ctl & ~0x3);
	}
	self->kdb_branchstep = x86_kdb_branchstep;
    }
    
    word_t kdb_drs = x86_kdb_dr_mask ^ self->kdb_dr_mask;
    if (kdb_drs)
    {
	word_t on = kdb_drs & x86_kdb_dr_mask;
	word_t off = kdb_drs & ~x86_kdb_dr_mask;
	self->kdb_dr_mask |= on;
	self->kdb_dr_mask &= ~off;
	
	for (word_t dr=(word_t)x86_lsb(off); off!=0; 
	     off>>=(word_t)x86_lsb(off)+1, dr+=(word_t)x86_lsb(off)+1)
	{
	    self->guest_dr[7] &=  ~(2 << (dr * 2)); /* disable */
	    self->guest_dr[7] &= ~(0x000F0000 << (dr * 4));
	}
	
	for (word_t dr=(word_t)x86_lsb(on); on!=0; 
	     on>>=(word_t)x86_lsb(on)+1, dr+=(word_t)x86_lsb(on)+1)
	{
	    self->guest_dr[dr] = x86_dr_read(dr);
	    /* Copy type from host dr7 */
	    word_t type = (x86_hvm_host_dr[7] >> (dr * 4)) & 0x30000;
	    self->guest_dr[7] |=  (2 << (dr * 2)); /* enable */
	    self->guest_dr[7] &= ~(0x000F0000 << (dr * 4));
	    self->guest_dr[7] |= (type << (dr * 4));
	}
	
	if (!self->kdb_dr_mask)
	{
	    // Redisable VMM DRs if needed	    
	    if (!self->flags.movdr)
	    {
		vmcs_exectr_cpubased_t cb = vmcs_exec_ctr_get_cpubased (self->vmcs);
		cb.movdr = 0;
		vmcs_exec_ctr_set_cpubased (self->vmcs, cb);
	    }
	    if (!self->flags.db || !self->flags.bp)
	    {
		vmcs_exectr_excbmp_t ebmp = vmcs_exec_ctr_get_except_bmp (self->vmcs);
		ebmp.db = self->flags.db;
		ebmp.bp = self->flags.bp;
		vmcs_exec_ctr_set_except_bmp (self->vmcs, ebmp);
	    }
	}
    }
#endif
    
    X86_SET_DR(0, self->guest_dr[0]); X86_SET_DR(1, self->guest_dr[1]);
    X86_SET_DR(2, self->guest_dr[2]); X86_SET_DR(3, self->guest_dr[3]);
    X86_SET_DR(6, self->guest_dr[6]); vmcs_gs_set_dr7 (self->vmcs, self->guest_dr[7]);
}

extern void do_enter_kdebug(x86_exceptionframe_t *frame, const word_t exception);
bool arch_hvm_ktcb_handle_debug_exit (arch_hvm_ktcb_t *self, vmcs_ei_qual_t qual)
{
#if defined(CONFIG_DEBUG)
    if (qual.dbg.bs)
    {
	if (x86_kdb_branchstep)
	{
	    word_t dbg_ctl = GET64_VMCS_LOW(gs, dbg_ctl);
	    SET64_VMCS_LOW(gs, dbg_ctl, dbg_ctl & ~0x3);
	    X86_SET_DR(6, qual.raw);
	    do_enter_kdebug(get_user_frame(addr_to_tcb(self)), X86_EXC_DEBUG);
	    return true;
	}
    
	if (x86_kdb_singlestep)		 
	{
	    vmcs_gs_ias_t ias = vmcs_gs_get_ias (self->vmcs);
	    
	    ias.bl_sti = ias.bl_movss = 0;
	    vmcs_gs_set_ias (self->vmcs, ias);
	    
	    vmcs_gs_as_t  as  = vmcs_gs_get_as (self->vmcs);
	    ASSERT(as.state != VMCS_AS_HLT);
	    
	    X86_SET_DR(6, qual.raw);
	    do_enter_kdebug(get_user_frame(addr_to_tcb(self)), X86_EXC_DEBUG);
	    return true;
	}
    }
    if (qual.dbg.bx)
    {
	X86_SET_DR(6, qual.raw);
	do_enter_kdebug(get_user_frame(addr_to_tcb(self)), X86_EXC_DEBUG);
	return true;
    }
    
    printf("HVM dbg exit no l4");
    enter_kdebug("UNTESTED");
#endif
    return false;
}

bool arch_hvm_ktcb_handle_nomath_exit (arch_hvm_ktcb_t *self)
{
    /* FPU access logic:
     * guest NE bit = 1 -> VMM has disabled FPU
     * virtual guest TS bit = 1 -> VMM wants NM exception
     * virtual guest TS bit = 0 -> L4 virtualizes FPU
     */
    
    if (self->flags.fpu_ts_bit)
	return false;
    
    word_t gcr0 = vmcs_gs_get_cr0 (self->vmcs);

    if (gcr0 & X86_CR0_EM)
	return false;
	
    ASSERT(x86_cr0_read() & X86_CR0_TS);
    tcb_t *tcb = addr_to_tcb (self);
    tcb_resources_x86_no_math_exception (&tcb->resources, tcb);
    return true;
}

bool arch_hvm_ktcb_handle_pagefault_exit (arch_hvm_ktcb_t *self, vmcs_ei_qual_t qual)
{
    // Check if self is just a VTLB miss, not a real page fault.
    return  x86_hvm_vtlb_handle_vtlb_miss (&self->vtlb, (addr_t) qual.faddr, self->exc.exit_eec);
}



void arch_hvm_ktcb_handle_invlpg_exit (arch_hvm_ktcb_t *self, vmcs_ei_qual_t qual)
{
    x86_hvm_vtlb_flush_gvirt_addr (&self->vtlb, (addr_t) qual.faddr);
}

void arch_hvm_ktcb_handle_invd_exit (arch_hvm_ktcb_t *self)
{
    x86_wbinvd();
}


/**********************************************************************
 *
 *                        x86 HVM -- implementation for VMX
 *
 **********************************************************************/

bool arch_hvm_ktcb_enable_hvm (arch_hvm_ktcb_t *self)
{
    if (self->hvm_enabled) 
        return true;
    
   
    if (!x86_x32_vmx_is_enabled ())
	return false;


    tcb_t   *tcb   = addr_to_tcb(self);
    space_t *space = tcb_get_space (tcb);
    ASSERT (space);

    if (!x86_hvm_vtlb_alloc (&self->vtlb, space))
	return false;

    self->vmcs = vmcs_alloc_vmcs ();
    if (!self->vmcs || !vmcs_load (self->vmcs))
    {
	x86_hvm_vtlb_free (&self->vtlb);
	return false;
    }

#if defined(CONFIG_DEBUG)
    self->kdb_dr_mask = 0;
    self->kdb_branchstep = false;
#endif

    for (word_t i=0; i < 7; i++)
	self->guest_dr[i] = 0;
    
    self->guest_cr2 = self->guest_cr0_mask = self->guest_cr4_mask = 0;
    
    for (word_t i=0; i < 7; i++)
	((word_t *) &self->exc)[i] = 0;
    
    self->flags.injecting = self->flags.cr2_write = self->flags.fpu_ts_bit = 0;
    
    arch_hvm_ktcb_init_vmcs (self);
	
#if defined(CONFIG_IO_FLEXPAGES)
    arch_hvm_ktcb_set_io_pbm (self, virt_to_phys (space_get_io_bitmap (space)));
#endif
    self->hvm_enabled = true;

    return true;
}

void arch_hvm_ktcb_disable_hvm (arch_hvm_ktcb_t *self)
{
    if (!self->hvm_enabled) 
        return;
    
    self->hvm_enabled = false;

    if (self->vmcs)
    {
	vmcs_free_vmcs (self->vmcs);
	self->vmcs = NULL;
    }
    x86_hvm_vtlb_free (&self->vtlb);
}


word_t arch_ktcb_get_x86_hvm_cregs (arch_ktcb_t *ktcb, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr) 
{
    /* The C++ receiver was the arch_hvm_ktcb_t base; these entries are
       pointer-to-member of arch_ktcb_t, so reach the base explicitly. */
    arch_hvm_ktcb_t *self = &ktcb->hvm;
    if (!self->hvm_enabled || !arch_hvm_ktcb_load_vmcs (self)) return 0;    
    
    /* transfer from frame to dst */
    word_t num = 0;
    
    for (word_t reg=(word_t)lsb(mask); mask!=0; mask>>=(word_t)lsb(mask)+1,reg+=(word_t)lsb(mask)+1,num++)
    {
        word_t value;
        switch (reg)
        {
        case creg_cr0:
            value = vmcs_gs_get_cr0 (self->vmcs);
            break;
        case creg_cr0_rd:
            value = vmcs_exec_ctr_get_cr0shadow (self->vmcs);
            break;
        case creg_cr0_msk:
            value = vmcs_exec_ctr_get_cr0mask (self->vmcs);
            break;
        case creg_cr2:
            value = self->guest_cr2;
            break;
        case creg_cr3:
            value = (word_t) x86_hvm_vtlb_get_guest_top_pdir (&self->vtlb);
            break;
        case creg_cr4:
            value =  vmcs_gs_get_cr4 (self->vmcs);
            break;
        case creg_cr4_rd:
            value = vmcs_exec_ctr_get_cr4shadow (self->vmcs);
            break;
        case creg_cr4_msk:
            value = vmcs_exec_ctr_get_cr4mask (self->vmcs);
            break;
        default:
            value = 0;
            printf("x86-hvm: unsupported read from exception reg %d\n", reg);
            enter_kdebug("hvm write");
            break;
        }

        TRACE_CTRLXFER_DETAILS( "\t (f%06d/%06d/%8s->m%06d): %08x", 
                                reg, 0, ctrlxfer_get_hwregname(id, reg), 
                                *dst_mr, value);

        tcb_set_mr (dst, (*dst_mr)++, value);
    }
    return num;
}

word_t arch_ktcb_set_x86_hvm_cregs (arch_ktcb_t *ktcb, word_t id, word_t mask, tcb_t *src, word_t *src_mr) 
{
    /* The C++ receiver was the arch_hvm_ktcb_t base; these entries are
       pointer-to-member of arch_ktcb_t, so reach the base explicitly. */
    arch_hvm_ktcb_t *self = &ktcb->hvm;
    if (!self->hvm_enabled || !arch_hvm_ktcb_load_vmcs (self)) return 0;
    
    /* transfer from src to frame */
    word_t num = 0;
    
 
    for (word_t reg=(word_t)lsb(mask); mask!=0; mask>>=(word_t)lsb(mask)+1,reg+=(word_t)lsb(mask)+1,num++)
    {
        word_t value =tcb_get_mr (src, (*src_mr)++);
        TRACE_CTRLXFER_DETAILS( "\t (m%06d->f%06d/%06d/%8s): %08x", 
                                *src_mr, reg, 0, ctrlxfer_get_hwregname(id, reg),
                                value);
        
        switch (reg)
        {
        case creg_cr0:
        {
            word_t cr0 = vmcs_gs_get_cr0 (self->vmcs);
	
            x86_hvm_vtlb_set_pe (&self->vtlb, value & X86_CR0_PG);
            x86_hvm_vtlb_set_wp (&self->vtlb, value & X86_CR0_WP);
            x86_hvm_vtlb_flush_gvirt (&self->vtlb);
	
            vmcs_gs_set_cr0 (self->vmcs, (value & self->guest_cr0_mask) | (cr0 & ~self->guest_cr0_mask));
        }
        break;
        case creg_cr0_rd:
            vmcs_exec_ctr_set_cr0shadow (self->vmcs, value);
            break;
        case creg_cr0_msk:
            vmcs_exec_ctr_set_cr0mask (self->vmcs, value | ~self->guest_cr0_mask);
            break;
        case creg_cr2:
            self->guest_cr2 = value;
            self->flags.cr2_write = true;
            break;
        case creg_cr3:
            x86_hvm_vtlb_set_guest_top_pdir (&self->vtlb, (pgent_t*)value);
            x86_hvm_vtlb_flush_gvirt (&self->vtlb);
            break;
        case creg_cr4:
        {
            word_t cr4 = vmcs_gs_get_cr4 (self->vmcs);
            vmcs_gs_set_cr4 (self->vmcs, (value & self->guest_cr4_mask) | (cr4 & ~self->guest_cr4_mask));
            x86_hvm_vtlb_set_pg (&self->vtlb, value & X86_CR4_PGE);
            x86_hvm_vtlb_flush_gvirt (&self->vtlb);
        }
        break;
        case creg_cr4_rd:
            vmcs_exec_ctr_set_cr4shadow (self->vmcs, value);
            break;
        case creg_cr4_msk:
            vmcs_exec_ctr_set_cr4mask (self->vmcs, value | ~self->guest_cr4_mask);
            x86_hvm_vtlb_flush_gvirt (&self->vtlb);
            break;
        default:
            printf("x86-hvm: unsupported write to exception reg %d value %x\n", reg, value);
            enter_kdebug("hvm write");
            break;
        }
    }
    return num;
}

word_t arch_ktcb_get_x86_hvm_dregs (arch_ktcb_t *ktcb, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr) 
{
    /* The C++ receiver was the arch_hvm_ktcb_t base; these entries are
       pointer-to-member of arch_ktcb_t, so reach the base explicitly. */
    arch_hvm_ktcb_t *self = &ktcb->hvm;
    if (!self->hvm_enabled || !arch_hvm_ktcb_load_vmcs (self)) return 0;
    
    /* transfer from frame to dst */
    const word_t *hwreg = ctrlxfer_hwregs[id];
    word_t num = 0;
    
    for (word_t reg=(word_t)lsb(mask); mask!=0; mask>>=(word_t)lsb(mask)+1,reg+=(word_t)lsb(mask)+1,num++)
    {   
        word_t value = self->guest_dr[hwreg[reg]];
        if (hwreg[reg] > 7 || hwreg[reg] == 4 || hwreg[reg] == 5)
        {
            printf("x86-hvm: invalid access to dreg %d reg %d\n", hwreg[reg], reg);
            continue;
        }
            
        TRACE_CTRLXFER_DETAILS( "\t (f%06d/%06d/%8s->m%06d): %08x", 
                                reg, hwreg[reg], ctrlxfer_get_hwregname(id, reg), 
                                *dst_mr, value);
        tcb_set_mr (dst, (*dst_mr)++, value);;
    }
    return num;
}

word_t arch_ktcb_set_x86_hvm_dregs (arch_ktcb_t *ktcb, word_t id, word_t mask, tcb_t *src, word_t *src_mr) 
{
    /* The C++ receiver was the arch_hvm_ktcb_t base; these entries are
       pointer-to-member of arch_ktcb_t, so reach the base explicitly. */
    arch_hvm_ktcb_t *self = &ktcb->hvm;
    if (!self->hvm_enabled || !arch_hvm_ktcb_load_vmcs (self)) return 0;
    
    /* transfer from src to frame */
    const word_t *hwreg = ctrlxfer_hwregs[id];
    word_t num = 0;
    
    for (word_t reg=(word_t)lsb(mask); mask!=0; mask>>=(word_t)lsb(mask)+1,reg+=(word_t)lsb(mask)+1,num++)
    {
        if (hwreg[reg] > 7 || hwreg[reg] == 4 || hwreg[reg] == 5)
        {
            printf("x86-hvm: invalid access to dreg %d reg %d\n", hwreg[reg], reg);
            continue;
        }
        TRACE_CTRLXFER_DETAILS( "\t (m%06d->f%06d/%06d/%8s): %08x", 
                                *src_mr, reg, hwreg[reg], ctrlxfer_get_hwregname(id, reg),
                                tcb_get_mr (src, *src_mr));
        
        self->guest_dr[hwreg[reg]] = tcb_get_mr (src, (*src_mr)++);
    }
    return num;
}


word_t arch_ktcb_get_x86_hvm_segregs (arch_ktcb_t *ktcb, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr) 
{
    /* The C++ receiver was the arch_hvm_ktcb_t base; these entries are
       pointer-to-member of arch_ktcb_t, so reach the base explicitly. */
    arch_hvm_ktcb_t *self = &ktcb->hvm;
    if (!self->hvm_enabled || !arch_hvm_ktcb_load_vmcs (self)) return 0;
    
    /* transfer from frame to dst */
    const word_t *hwreg = ctrlxfer_hwregs[id];
    word_t num = 0;
    
    for (word_t reg=(word_t)lsb(mask); mask!=0; mask>>=(word_t)lsb(mask)+1,reg+=(word_t)lsb(mask)+1,num++)
    {
        word_t value = vmcs_read_register (self->vmcs, hwreg[reg]);
        
        TRACE_CTRLXFER_DETAILS( "\t (f%06d/%06d/%8s->m%06d): %08x", 
                                reg, hwreg[reg], ctrlxfer_get_hwregname(id, reg), 
                                *dst_mr, value);;
        tcb_set_mr (dst, (*dst_mr)++, value);
    }
    return num;
}

word_t arch_ktcb_set_x86_hvm_segregs (arch_ktcb_t *ktcb, word_t id, word_t mask, tcb_t *src, word_t *src_mr) 
{
    /* The C++ receiver was the arch_hvm_ktcb_t base; these entries are
       pointer-to-member of arch_ktcb_t, so reach the base explicitly. */
    arch_hvm_ktcb_t *self = &ktcb->hvm;
    if (!self->hvm_enabled || !arch_hvm_ktcb_load_vmcs (self)) return 0;
    /* transfer from src to frame */
    const word_t *hwreg = ctrlxfer_hwregs[id];
    word_t num = 0;
    
    for (word_t reg=(word_t)lsb(mask); mask!=0; mask>>=(word_t)lsb(mask)+1,reg+=(word_t)lsb(mask)+1,num++)
    {
        TRACE_CTRLXFER_DETAILS( "\t (m%06d->f%06d/%06d/%8s): %08x", 
                                *src_mr, reg, hwreg[reg], ctrlxfer_get_hwregname(id, reg),
                                tcb_get_mr (src, *src_mr));
        vmcs_write_register (self->vmcs, hwreg[reg], tcb_get_mr (src, (*src_mr)++));
    }
    return num;
}


word_t arch_ktcb_get_x86_hvm_nonregexc (arch_ktcb_t *ktcb, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr) 
{
    /* The C++ receiver was the arch_hvm_ktcb_t base; these entries are
       pointer-to-member of arch_ktcb_t, so reach the base explicitly. */
    arch_hvm_ktcb_t *self = &ktcb->hvm;
    if (!self->hvm_enabled || !arch_hvm_ktcb_load_vmcs (self)) return 0;
    /* transfer from frame to dst */
    word_t num = 0;
    
    for (word_t reg=(word_t)lsb(mask); mask!=0; mask>>=(word_t)lsb(mask)+1,reg+=(word_t)lsb(mask)+1,num++)
    {
        word_t value;
        
        switch (reg)
        {
        case activity_state:
        {
            vmcs_gs_as_t as = vmcs_gs_get_as (self->vmcs);
            value = as.raw;
        }
        break;
        case interruptibility_state:
        {	
            vmcs_gs_ias_t ias = vmcs_gs_get_ias (self->vmcs);
            value = ias.raw;
        }
        break;
        case pending_debug_exc:
        {
            vmcs_gs_pend_dbg_except_t dbge = vmcs_gs_get_pend_dbg_except (self->vmcs);
            value = dbge.raw;
        }
        break;
        case entry_info:
            value = self->exc.entry_info.raw;
            break;
        case entry_eec:
            value = self->exc.entry_eec;
            break;
        case entry_ilen:
            value = self->exc.entry_ilen;
            break;
        case exit_info:
            value = self->exc.exit_info.raw;
            break;
        case exit_eec:
            value = self->exc.exit_eec;
            break;
        case idt_info:
            value = self->exc.idt_info.raw;
            break;
        case idt_eec:
            value = self->exc.idt_eec;
            break;
        default:
        {
            value = 0;
            printf("x86-hvm: unsupported read from nonreg/self->exc %d\n", reg);
            enter_kdebug("hvm read");
            break;
        }
        }
        TRACE_CTRLXFER_DETAILS( "\t (f%06d/%06d/%8s->m%06d): %08x", 
                                reg, 0, ctrlxfer_get_hwregname(id, reg), 
                                *dst_mr, value);
    

        tcb_set_mr (dst, (*dst_mr)++, value);
    }
    return num;
}

word_t arch_ktcb_set_x86_hvm_nonregexc (arch_ktcb_t *ktcb, word_t id, word_t mask, tcb_t *src, word_t *src_mr) 
{
    /* The C++ receiver was the arch_hvm_ktcb_t base; these entries are
       pointer-to-member of arch_ktcb_t, so reach the base explicitly. */
    arch_hvm_ktcb_t *self = &ktcb->hvm;
    if (!self->hvm_enabled || !arch_hvm_ktcb_load_vmcs (self)) return 0;
    /* transfer from src to frame */
    word_t num = 0;
    
    for (word_t reg=(word_t)lsb(mask); mask!=0; mask>>=(word_t)lsb(mask)+1,reg+=(word_t)lsb(mask)+1,num++)
    {
        word_t value = tcb_get_mr (src, (*src_mr)++);
        TRACE_CTRLXFER_DETAILS( "\t (m%06d->f%06d/%06d/%8s): %08x", 
                                *src_mr, reg, 0, ctrlxfer_get_hwregname(id, reg),
                                value);
        switch (reg)
        {
        case activity_state:
        {
            vmcs_gs_as_t as;
            as.raw = value;
            vmcs_gs_set_as (self->vmcs, as);
        }
        break;
        case interruptibility_state:
        {	
            vmcs_gs_ias_t ias;
            ias.raw = value;
            vmcs_gs_set_ias (self->vmcs, ias);
        }
        break;
        case pending_debug_exc:
        {
            vmcs_gs_pend_dbg_except_t dbge;
            dbge.raw = value;
            vmcs_gs_set_pend_dbg_except (self->vmcs, dbge);
        }
        break;
        case entry_info:
            self->exc.entry_info.raw = value;
            self->flags.injecting = true;
            break;
        case entry_eec:
            self->exc.entry_eec = value;
            self->flags.injecting = true;
            break;
        case entry_ilen:
            self->exc.entry_ilen = value;
            self->flags.injecting = true;
            break;
        case exit_info:
        case exit_eec:
        case idt_info:
        case idt_eec:
            // Read-only
            break;
        default:
        {
            value = 0;
            printf("x86-hvm: unsupported write to nonreg/self->exc %d value %x\n", reg, value);
            enter_kdebug("hvm write");
            break;
        }
        }

    }
    return num;
}


word_t arch_ktcb_get_x86_hvm_execctrl (arch_ktcb_t *ktcb, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr) 
{
    /* The C++ receiver was the arch_hvm_ktcb_t base; these entries are
       pointer-to-member of arch_ktcb_t, so reach the base explicitly. */
    arch_hvm_ktcb_t *self = &ktcb->hvm;
    if (!self->hvm_enabled || !arch_hvm_ktcb_load_vmcs (self)) return 0;
    /* transfer from frame to dst */
    
    word_t num = 0;
    
    for (word_t reg=(word_t)lsb(mask); mask!=0; mask>>=(word_t)lsb(mask)+1,reg+=(word_t)lsb(mask)+1,num++)
    {
        word_t value;
        switch (reg)
        {
        case pin_exec_ctrl:
        {
            vmcs_exectr_pinbased_t pb = vmcs_exec_ctr_get_pinbased (self->vmcs);
            value = pb.raw;
        }
        break;
        case cpu_exec_ctrl:
        {	
            vmcs_exectr_cpubased_t cb = vmcs_exec_ctr_get_cpubased (self->vmcs);
            value = cb.raw;
        }
        break;
        case exc_bitmap:
        {
            vmcs_exectr_excbmp_t ebmp = vmcs_exec_ctr_get_except_bmp (self->vmcs);
            value = ebmp.raw;
        }
        break;
        default:
        {
            value = 0;
            printf("x86-hvm: unsupported read from execctrl reg %d\n", reg);
            enter_kdebug("hvm read");
            break;
        }
        }
        
        TRACE_CTRLXFER_DETAILS( "\t (f%06d/%06d/%8s->m%06d): %08x", 
                                reg, 0, ctrlxfer_get_hwregname(id, reg), 
                                *dst_mr, value);

        tcb_set_mr (dst, (*dst_mr)++, value);
    }
    return num;
}

word_t arch_ktcb_set_x86_hvm_execctrl (arch_ktcb_t *ktcb, word_t id, word_t mask, tcb_t *src, word_t *src_mr) 
{
    /* The C++ receiver was the arch_hvm_ktcb_t base; these entries are
       pointer-to-member of arch_ktcb_t, so reach the base explicitly. */
    arch_hvm_ktcb_t *self = &ktcb->hvm;
    if (!self->hvm_enabled || !arch_hvm_ktcb_load_vmcs (self)) return 0;
    /* transfer from src to frame */
    word_t num = 0;
    
    for (word_t reg=(word_t)lsb(mask); mask!=0; mask>>=(word_t)lsb(mask)+1,reg+=(word_t)lsb(mask)+1,num++)
    {
        word_t value = tcb_get_mr (src, (*src_mr)++);
        TRACE_CTRLXFER_DETAILS( "\t (m%06d->f%06d/%06d/%8s): %08x", 
                                *src_mr, reg, 0, ctrlxfer_get_hwregname(id, reg), 
                                value);
        
        switch (reg)
        {
        case pin_exec_ctrl:
        {
            vmcs_exectr_pinbased_t pb;
            pb.raw = value;
            vmcs_exec_ctr_set_pinbased (self->vmcs, pb);
        }
        break;
        case cpu_exec_ctrl:
        {	
            vmcs_exectr_cpubased_t cb;
            cb.raw = value;
	
#if defined(CONFIG_X86_IO_FLEXPAGES)
            // Don't allow IO passthrough
            cb.io = 1;
#endif
#if defined(CONFIG_DEBUG)
            // Logic to merge KDB and VMM DRs
            if (cb.movdr == 0)
            {
                self->flags.movdr = 0;
                cb.movdr = (self->kdb_dr_mask != 0);
            }
#endif	
            if (cb.iobitm || cb.msrbitm)
                UNIMPLEMENTED();
	
            //Don't allow invlpg or hlt passthrough
            cb.invlpg = 1;
            cb.hlt = 1;
	
            vmcs_exec_ctr_set_cpubased (self->vmcs, cb);
        }
        break;
        case exc_bitmap:
        {
            vmcs_exectr_excbmp_t ebmp;
            ebmp.raw = value;
#if defined(CONFIG_DEBUG)
            // Logic to merge KDB and VMM DRs
            if (ebmp.db == 0 || ebmp.bp == 0)
            {
                self->flags.db = ebmp.db;
                self->flags.bp = ebmp.bp;
                ebmp.db = ebmp.bp = (self->kdb_dr_mask != 0);
            }
#endif	
            vmcs_exec_ctr_set_except_bmp (self->vmcs, ebmp);
        }
        break;
        default:
        {
            value = 0;
            printf("x86-hvm: unsupported write to execctrl reg %d value %x\n", reg, value);
            enter_kdebug("hvm write");
            break;
        }
        }
    }
    return num;
}


word_t arch_ktcb_get_x86_hvm_otherregs (arch_ktcb_t *ktcb, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr) 
{
    /* The C++ receiver was the arch_hvm_ktcb_t base; these entries are
       pointer-to-member of arch_ktcb_t, so reach the base explicitly. */
    arch_hvm_ktcb_t *self = &ktcb->hvm;
    if (!self->hvm_enabled || !arch_hvm_ktcb_load_vmcs (self)) return 0;
    /* transfer from frame to dst */
    word_t num = 0;
    
    for (word_t reg=(word_t)lsb(mask); mask!=0; mask>>=(word_t)lsb(mask)+1,reg+=(word_t)lsb(mask)+1,num++)
    {
        word_t value;
        switch (reg)
        {
        case sysenter_cs_msr:
            value = vmcs_gs_get_sysenter_cs (self->vmcs);
            break;
        case sysenter_eip_msr:
            value = vmcs_gs_get_sysenter_eip (self->vmcs);
            break;
        case sysenter_esp_msr:
            value = vmcs_gs_get_sysenter_esp (self->vmcs);
            break;
        case debugctl_msr_low:
            value = GET64_VMCS_LOW(gs, dbg_ctl);
            break;
            break;
        case debugctl_msr_high:
            value = GET64_VMCS_HIGH(gs, dbg_ctl);
            break;
        case rdtsc_ofs_low:
            value = GET64_VMCS_LOW(exec_ctr, tscoff);
            break;
        case rdtsc_ofs_high:
            value = GET64_VMCS_HIGH(exec_ctr, tscoff);
            break;
        case vapic_address:
            value = vmcs_exec_ctr_get_vapicaddr (self->vmcs);
            break;
        case tpr_threshold:
        {
            vmcs_exectr_tprth_t i_tprth;
            i_tprth = vmcs_exec_ctr_get_tprthr (self->vmcs);
            value = i_tprth.raw;
        }
        break;
        default:
            value = 0;
            printf ("x86-hvm: unsupported read from guest state reg %d\n", reg);
            enter_kdebug("hvm msr read");
        }
        TRACE_CTRLXFER_DETAILS( "\t (f%06d/%06d/%8s->m%06d): %08x", 
                                reg, 0, ctrlxfer_get_hwregname(id, reg), 
                                *dst_mr, value);
        tcb_set_mr (dst, (*dst_mr)++, value);
    }
    return num;
}

word_t arch_ktcb_set_x86_hvm_otherregs (arch_ktcb_t *ktcb, word_t id, word_t mask, tcb_t *src, word_t *src_mr) 
{
    /* The C++ receiver was the arch_hvm_ktcb_t base; these entries are
       pointer-to-member of arch_ktcb_t, so reach the base explicitly. */
    arch_hvm_ktcb_t *self = &ktcb->hvm;
    if (!self->hvm_enabled || !arch_hvm_ktcb_load_vmcs (self)) return 0;
    /* transfer from src to frame */
    word_t num = 0;
    
    for (word_t reg=(word_t)lsb(mask); mask!=0; mask>>=(word_t)lsb(mask)+1,reg+=(word_t)lsb(mask)+1,num++)
    {
        word_t value = tcb_get_mr (src, (*src_mr)++);
        
        TRACE_CTRLXFER_DETAILS( "\t (m%06d->f%06d/%06d/%8s): %08x", 
                                *src_mr, reg, 0, ctrlxfer_get_hwregname(id, reg),
                                value);
        
        switch (reg)
        {
        case sysenter_cs_msr:
            vmcs_gs_set_sysenter_cs (self->vmcs, value);
            break;
        case sysenter_eip_msr:
            vmcs_gs_set_sysenter_eip (self->vmcs, value);
            break;
        case sysenter_esp_msr:
            vmcs_gs_set_sysenter_esp (self->vmcs, value);
            break;
        case debugctl_msr_low:
            SET64_VMCS_LOW(gs, dbg_ctl, value);
            break;
            break;
        case debugctl_msr_high:
            SET64_VMCS_HIGH(gs, dbg_ctl, value);
            break;
        case rdtsc_ofs_low:
            SET64_VMCS_LOW(exec_ctr, tscoff, value);
            break;
        case rdtsc_ofs_high:
            SET64_VMCS_HIGH(exec_ctr, tscoff, value);
            break;
        case vapic_address:
            vmcs_exec_ctr_set_vapicaddr (self->vmcs, value);
            break;
        case tpr_threshold:
        {
            vmcs_exectr_tprth_t i_tprth;
            i_tprth.th = value;
            vmcs_exec_ctr_set_tprthr (self->vmcs, i_tprth);
        }
        break;
        default:
            printf("x86-hvm: unsupported write to guest state reg %d value %x\n", reg, value);
            enter_kdebug("hvm msrreg write");
            break;
        }
    }
    return num;
}

#if defined(CONFIG_DEBUG)
word_t arch_ktcb_get_x86_hvm_ctrlxfer_reg (arch_ktcb_t *ktcb, word_t id, word_t reg) 
{
    /* The C++ receiver was the arch_hvm_ktcb_t base; these entries are
       pointer-to-member of arch_ktcb_t, so reach the base explicitly. */
    arch_hvm_ktcb_t *self = &ktcb->hvm;
    if (!self->hvm_enabled || !arch_hvm_ktcb_load_vmcs (self)) return 0;
    
    word_t value = 0;
    const word_t *hwreg = ctrlxfer_hwregs[id];

    switch (id)
    {
    case id_cregs:
        switch (reg)
        {
        case creg_cr0:
            value = vmcs_gs_get_cr0 (self->vmcs);
            break;
        case creg_cr0_rd:
            value = vmcs_exec_ctr_get_cr0shadow (self->vmcs);
            break;
        case creg_cr0_msk:
            value = vmcs_exec_ctr_get_cr0mask (self->vmcs);
            break;
        case creg_cr2:
            value = self->guest_cr2;
            break;
        case creg_cr3:
            value = (word_t) x86_hvm_vtlb_get_guest_top_pdir (&self->vtlb);
            break;
        case creg_cr4:
            value =  vmcs_gs_get_cr4 (self->vmcs);
            break;
        case creg_cr4_rd:
            value = vmcs_exec_ctr_get_cr4shadow (self->vmcs);
            break;
        case creg_cr4_msk:
            value = vmcs_exec_ctr_get_cr4mask (self->vmcs);
            break;
        default:
            value = 0;
            printf("x86-hvm: unsupported reg to exception reg %d value %x\n", reg, value);
            enter_kdebug("hvm write");
            break;
        }
        break;
    case id_dregs:
        value = self->guest_dr[hwreg[reg]];
        break;
    case id_csregs:
    case id_ssregs:
    case id_dsregs:
    case id_esregs:
    case id_fsregs:
    case id_gsregs:
    case id_trregs:
    case id_ldtrregs:
    case id_idtrregs:
    case id_gdtrregs:
        /* transfer from frame to dst */
        value = vmcs_read_register (self->vmcs, hwreg[reg]);
        break;
    case id_nonregexc:
        switch (reg)
        {
        case activity_state:
        {
            vmcs_gs_as_t as = vmcs_gs_get_as (self->vmcs);
            value = as.raw;
        }
        break;
        case interruptibility_state:
        {	
            vmcs_gs_ias_t ias = vmcs_gs_get_ias (self->vmcs);
            value = ias.raw;
        }
        break;
        case pending_debug_exc:
        {
            vmcs_gs_pend_dbg_except_t dbge = vmcs_gs_get_pend_dbg_except (self->vmcs);
            value = dbge.raw;
        }
        break;
        case entry_info:
            value = self->exc.entry_info.raw;
            break;
        case entry_eec:
            value = self->exc.entry_eec;
            break;
        case entry_ilen:
            value = self->exc.entry_ilen;
            break;
        case exit_info:
            value = self->exc.exit_info.raw;
            break;
        case exit_eec:
            value = self->exc.exit_eec;
            break;
        case idt_info:
            value = self->exc.idt_info.raw;
            break;
        case idt_eec:
            value = self->exc.idt_eec;
            break;
        default:
        {
            value = 0;
            printf("x86-hvm: unsupported read from nonreg/self->exc %d\n", reg);
            enter_kdebug("hvm read");
            break;
        }
        }
        break;
    case id_execctrl:
        switch (reg)
        {
        case pin_exec_ctrl:
        {
            vmcs_exectr_pinbased_t pb = vmcs_exec_ctr_get_pinbased (self->vmcs);
            value = pb.raw;
        }
        break;
        case cpu_exec_ctrl:
        {	
            vmcs_exectr_cpubased_t cb = vmcs_exec_ctr_get_cpubased (self->vmcs);
            value = cb.raw;
        }
        break;
        case exc_bitmap:
        {
            vmcs_exectr_excbmp_t ebmp = vmcs_exec_ctr_get_except_bmp (self->vmcs);
            value = ebmp.raw;
        }
        break;
        default:
        {
            value = 0;
            printf("x86-hvm: unsupported read from execctrl reg %d\n", reg);
            enter_kdebug("hvm read");
            break;
        }
        }
        break;
    case id_otherregs:
        switch (reg)
        {
        case sysenter_cs_msr:
            vmcs_gs_set_sysenter_cs (self->vmcs, value);
            break;
        case sysenter_eip_msr:
            vmcs_gs_set_sysenter_eip (self->vmcs, value);
            break;
        case sysenter_esp_msr:
            vmcs_gs_set_sysenter_esp (self->vmcs, value);
            break;
        case debugctl_msr_low:
            SET64_VMCS_LOW(gs, dbg_ctl, value);
            break;
            break;
        case debugctl_msr_high:
            SET64_VMCS_HIGH(gs, dbg_ctl, value);
            break;
        case rdtsc_ofs_low:
            SET64_VMCS_LOW(exec_ctr, tscoff, value);
            break;
        case rdtsc_ofs_high:
            SET64_VMCS_HIGH(exec_ctr, tscoff, value);
            break;
        case vapic_address:
            vmcs_exec_ctr_set_vapicaddr (self->vmcs, value);
            break;
        case tpr_threshold:
        {
            vmcs_exectr_tprth_t i_tprth;
            i_tprth.th = value;
            vmcs_exec_ctr_set_tprthr (self->vmcs, i_tprth);
        }
        break;
        default:
            printf("unsupported hvm ctrlxfer item read id %d reg %d\n", id, reg);
            enter_kdebug("hvm ctrlxfer read");
            break;
        }
    }
    return value;
}
#endif
word_t saved_rfl;

void arch_hvm_ktcb_resume_hvm (arch_hvm_ktcb_t *self)
{
    tcb_t *tcb		= addr_to_tcb(self);
    x86_exceptionframe_t *frame	= get_user_frame(tcb);
    
    ASSERT(self->hvm_enabled);
    ASSERT(self->vmcs);
    vmcs_load (self->vmcs);
    ASSERT(vmcs_is_loaded (self->vmcs));
	   
    vmcs_gs_set_rflags (self->vmcs, frame->__base.eflags);
    vmcs_gs_set_rip (self->vmcs, frame->__base.eip);
    vmcs_gs_set_rsp (self->vmcs, frame->__base.esp);

    arch_hvm_ktcb_restore_guest_drs (self);
   
    /* Set FPU access permissions:
     * virtual guest TS bit = 1 -> gcr0 = 1
     * virtual guest TS bit = 0 -> gcr0 = hcr0
     */
    word_t gcr0 = vmcs_gs_get_cr0 (self->vmcs);

    if (self->flags.fpu_ts_bit)
	gcr0 |= X86_CR0_TS;
    else
	gcr0 = (gcr0 & ~X86_CR0_TS) | (x86_cr0_read() & X86_CR0_TS);

    vmcs_gs_set_cr0 (self->vmcs, gcr0);
    vmcs_gs_set_cr3 (self->vmcs, x86_hvm_vtlb_get_active_top_pdir (&self->vtlb));

    if (1 || self->flags.cr2_write)
    {
	x86_cr2_write (self->guest_cr2);
	self->flags.cr2_write = false;
    }
    
    if (self->flags.injecting)
    {
	vmcs_entry_ctr_set_iif (self->vmcs, self->exc.entry_info);
	vmcs_entry_ctr_set_eec (self->vmcs, self->exc.entry_eec);
	vmcs_entry_ctr_set_instr_len (self->vmcs, self->exc.entry_ilen);
	
	TRACEPOINT(X86_HVM_ENTRY_EXC, 
		   "x86-hvm: inject exception %x (type %d vec %d eecv %c), eec %d, ilen %d", 
		   self->exc.entry_info.raw, self->exc.entry_info.type, self->exc.entry_info.vector,
		   self->exc.entry_info.err_code_valid ? 'y' : 'n', self->exc.entry_eec, self->exc.entry_ilen);
    }
    else
	ASSERT(!self->exc.entry_info.valid);
    
#if defined(CONFIG_DEBUG)
    vmcs_do_vmentry_checks (self->vmcs);
#endif
    UNUSED vmcs_int_t iif = ((vmcs_int_t)vmcs_entry_ctr_get_iif (self->vmcs));
    UNUSED word_t eec = vmcs_entry_ctr_get_eec (self->vmcs);
    UNUSED word_t ilen = vmcs_entry_ctr_get_instr_len (self->vmcs);
    UNUSED word_t rfl = vmcs_gs_get_rflags (self->vmcs);
    UNUSED word_t csb = vmcs_read_register (self->vmcs, VMCS_IDX_G_CS_BASE);
    UNUSED word_t rip = vmcs_gs_get_rip (self->vmcs);
    UNUSED vmcs_gs_pend_dbg_except_t dbge = vmcs_gs_get_pend_dbg_except (self->vmcs);
    
    if ((rfl != saved_rfl) && rfl == 0x46)
	printf("x86-hvm: entry old_fl %x new_fl %x\n",  saved_rfl, rfl);
	
    TRACEPOINT (X86_HVM_ENTRY, "x86-hvm: entry csbase %x ip %x fl %x iif %x eec %x ilen %x dbge %x",
		csb, rip, rfl, iif.raw, eec, ilen, dbge.raw);
}


void arch_hvm_ktcb_handle_hvm_exit (arch_hvm_ktcb_t *self)
{
    tcb_t *tcb	= addr_to_tcb(self);
    vmcs_ei_reason_t reason = vmcs_exitinfo_get_reason (self->vmcs);
    word_t basic_reason = reason.basic_reason;
    vmcs_ei_qual_t qual = vmcs_exitinfo_get_qual (self->vmcs);
    word_t ilen= vmcs_exitinfo_get_instr_len (self->vmcs);
    word_t ia_info = 0;
    bool handled = false;

    x86_exceptionframe_t *frame	= get_user_frame(tcb);
    frame->__base.eip = vmcs_gs_get_rip (self->vmcs);
    frame->__base.esp = vmcs_gs_get_rsp (self->vmcs);
    frame->__base.eflags = vmcs_gs_get_rflags (self->vmcs);
    saved_rfl = frame->__base.eflags;
    
    UNUSED word_t csb = vmcs_read_register (self->vmcs, VMCS_IDX_G_CS_BASE);

    self->exc.exit_info.raw = self->exc.exit_eec = 0;
    self->exc.idt_info = vmcs_exitinfo_get_idtvect_info (self->vmcs);
    self->exc.idt_eec = vmcs_exitinfo_get_idtvect_ec (self->vmcs);

    self->guest_cr2 = x86_cr2_read();

    if (self->flags.injecting)
    {
	/* VMX clears valid bit on entry, mirror fields */
	self->exc.entry_info = vmcs_entry_ctr_get_iif (self->vmcs);
	self->exc.entry_eec = vmcs_entry_ctr_get_eec (self->vmcs);
	self->exc.entry_ilen = vmcs_entry_ctr_get_instr_len (self->vmcs);
	self->flags.injecting = false;
    }
    
    arch_hvm_ktcb_save_guest_drs (self);

    if(reason.is_entry_fail)
    {
	enter_kdebug("vmexit due to entry failed");
    }
    
    switch (basic_reason)
    {
    case VMCS_BE_EXP_NMI:	// Exception or NMI.
    {
	self->exc.exit_info = vmcs_exitinfo_get_int_info (self->vmcs);
	self->exc.exit_eec = vmcs_exitinfo_get_int_ec (self->vmcs);
	
	switch (self->exc.exit_info.vector)
	{
	case X86_EXC_DEBUG:
	    TRACEPOINT (X86_HVM_EXIT, "x86-hvm: breakpoint exit %d qual %x  cs %x ip %x",
			basic_reason, qual.raw,  csb, frame->__base.eip); 
	    handled = arch_hvm_ktcb_handle_debug_exit (self, qual);
	    break;
	case X86_EXC_NOMATH_COPROC:	// No math exception.
	    TRACEPOINT (X86_HVM_EXIT, "x86-hvm: nomath exit %d qual %x  cs %x ip %x",
			basic_reason, qual.raw,  csb, frame->__base.eip); 
	    handled = arch_hvm_ktcb_handle_nomath_exit (self);
	    break;
	case X86_EXC_PAGEFAULT:		// Page fault.
	    if (qual.raw == 0x88)
		ENABLE_TRACEPOINT(X86_HVM_EXIT, ~0UL, ~0UL);
                
	    TRACEPOINT (X86_HVM_EXIT, "x86-hvm: pf exit %d qual %x  eec %d cs %x ip %x",
			basic_reason, qual.raw,  self->exc.exit_eec, csb, frame->__base.eip); 
	    handled = arch_hvm_ktcb_handle_pagefault_exit (self, qual);
	    break;
	default:
	    TRACEPOINT (X86_HVM_EXIT, "x86-hvm: self->exc exit %d (self->exc %d:%x) vec %d qual %x  cs %x ip %x",
			basic_reason, self->exc.exit_info.err_code_valid, self->exc.exit_eec, self->exc.exit_info.vector,
			qual.raw,  csb, frame->__base.eip);
	    break;
	}
	break;
    }
    case VMCS_BE_EXT_INT:	// External interrupt.
    {
	TRACEPOINT (X86_HVM_EXIT_EXTINT, "x86-hvm:  reason %d (extint) cs %x ip %x",
		    reason.basic_reason, csb, frame->__base.eip);

	// Allow ourselves to be preempted on an external interrupt.
	asm (
	    "	sti	\n"
	    "	nop	\n"
	    "	cli	\n"
	    );

	vmcs_load (self->vmcs);
	handled = true;
    }
    break;
    case VMCS_BE_IW:	// Interrupt Window.
	TRACEPOINT(X86_HVM_EXIT, "x86-hvm: interrupt window exit: eflags %x cs %x ip %x", 
		   (word_t) frame->__base.eflags,  csb, frame->__base.eip);
	break;
    case VMCS_BE_DR:
    case VMCS_BE_CR:
	ia_info = vmcs_exitinfo_get_linear_addr (self->vmcs);
	    
	TRACEPOINT (X86_HVM_EXIT, "x86-hvm: cr/dr exit: %d (%cr%c) qual %x,  cs %x ip %x", 
		    basic_reason,  (basic_reason == VMCS_BE_CR ? 'c' : 'd'),
		    qual.mov_cr.cr_num + '0', qual.raw,  csb, frame->__base.eip);
	break;
    case VMCS_BE_TASKSW:
	TRACEPOINT (X86_HVM_EXIT, "x86-hvm: tasksw exit: %d qual %x,  cs %x ip %x", 
		    basic_reason, qual.raw,  csb, frame->__base.eip);
	break;
    case VMCS_BE_IO:
	ia_info = vmcs_exitinfo_get_linear_addr (self->vmcs);

	TRACEPOINT (X86_HVM_EXIT, "x86-hvm: i/o exit: qual %x (%c, p %x, st %d sz %d, r %d i %d) cs %x ip %x",
		    qual.raw, (qual.io.dir == VMCS_DIR_OUT ? 'o' : 'i'),
		    qual.io.port_num, qual.io.string, (qual.io.soa + 1) * 8, 
		    qual.io.rep, qual.io.op_encoding, csb, frame->__base.eip);
	break;
    case VMCS_BE_RDMSR:
    case VMCS_BE_WRMSR:
	TRACEPOINT (X86_HVM_EXIT, "x86-hvm: msr exit %d (msr %x) qual %x  cs %x ip %x",
		    basic_reason, frame->__base.ecx, qual.raw,  csb, frame->__base.eip);
	break;
    case VMCS_BE_VMCALL:
    case VMCS_BE_VMCLEAR ... VMCS_BE_VMXON:
    {
	vmcs_ei_vm_instr_t vm_instr_info = vmcs_exitinfo_get_vm_instr_info (self->vmcs);
	ia_info = vm_instr_info.raw;
	
	TRACEPOINT (X86_HVM_EXIT, "x86-hvm: vmx exit %d (vmcall/vmclear...vmxon) qual %x  cs %x ip %x",
		    basic_reason, qual.raw,  csb, frame->__base.eip);
    }
    break;
    case VMCS_BE_CPUID:
	TRACEPOINT (X86_HVM_EXIT, "x86-hvm: cpuid exit %d qual %x  cs %x ip %x",
		    basic_reason, qual.raw,  csb, frame->__base.eip);
	break;
    case VMCS_BE_HLT:
	TRACEPOINT (X86_HVM_EXIT, "x86-hvm: hlt exit %d qual %x  cs %x ip %x",
		    basic_reason, qual.raw,  csb, frame->__base.eip);
	break;
    case VMCS_BE_INVD:	// Invalidate Caches
	arch_hvm_ktcb_handle_invd_exit (self);
	TRACEPOINT (X86_HVM_EXIT, "x86-hvm: invd exit %d qual %x  cs %x ip %x",
		    basic_reason, qual.raw,  csb, frame->__base.eip);
	break;
	
    case VMCS_BE_RDPMC:
    case VMCS_BE_RDTSC:
    case VMCS_BE_RSM:
    case VMCS_BE_MONITOR:
    case VMCS_BE_MWAIT:
    case VMCS_BE_PAUSE:
	TRACEPOINT (X86_HVM_EXIT, "x86-hvm: cpuid/hlt/invd/rdpmc/rdtsc exit %d qual %x  cs %x ip %x",
		    basic_reason, qual.raw,  csb, frame->__base.eip);
	break;
    case VMCS_BE_INVLPG:	// Invalidate TLB Entry.
	TRACEPOINT (X86_HVM_EXIT, "x86-hvm: invlpg exit %d qual %x  cs %x ip %x",
		    basic_reason, qual.raw,  csb, frame->__base.eip);
	arch_hvm_ktcb_handle_invlpg_exit (self, qual);
	break;
    case VMCS_BE_ENTRY_INVG: // Invalid Guest State
	TRACEPOINT (X86_HVM_EXIT, "x86-hvm: invalid gueststate exit %d qual %x  cs %x ip %x",
		    basic_reason, qual.raw,  csb, frame->__base.eip);
	break;
    case VMCS_BE_ENTRY_MSRLD:	// Invalid MSR
	TRACEPOINT (X86_HVM_EXIT, "x86-hvm: invalid msr load exit %d qual %x  cs %x ip %x",
		    basic_reason, qual.raw,  csb, frame->__base.eip);
	break;
    default:
	TRACEPOINT (X86_HVM_EXIT, "x86-hvm: exit %d qual %x  cs %x ip %x",  
		    basic_reason, qual.raw,  csb, frame->__base.eip);
	break;
    }
    
    if (!handled || self->exc.idt_info.valid)
        arch_hvm_ktcb_send_hvm_fault (self, (word_t) basic_reason, qual.raw, ilen, ia_info, handled);
    
    arch_hvm_ktcb_resume_hvm (self);

}



NORETURN void arch_hvm_ktcb_enter_hvm_loop (arch_hvm_ktcb_t *self)
{
    tcb_t *tcb = addr_to_tcb(self);
    msg_tag_t msgtag = tcb_get_tag (tcb);
    if(msg_tag_get_untyped (&msgtag))
	printf("Received untyped items in startup reply\n");
    
    ASSERT(self->hvm_enabled);
    arch_hvm_ktcb_resume_hvm (self);
    
    // First entry into the VM needs to be a vmlaunch.
    x86_x32_vmx_vmlaunch (get_user_frame(tcb)); 
}



NORETURN void do_handle_vmexit ()
{
#if defined(CONFIG_IO_FLEXPAGES)
    /*
     * VT predefined the size of the TSS segment to 67h bytes
     *  self excludes the IOPBMP.
     *
     * To use the IOPBMP, we define an other TSS to be loaded at
     * the VM-Exit. (X86_TSS_VMX)
     *
     * Here we switch to the L4 TSS, which handles IOBMPs.
     */
    extern x86_x32_segdesc_t gdt[];

    gdt[X86_TSS >> 3].set_sys((u32_t) TSS_MAPPING, sizeof (x86_x32_tss_t) - 1,
			       0, X86_X32_SEGDESC_TSS);

    asm (
	"	ltr  %%ax	\n"
	:
	: "a" (X86_TSS));
#endif

    
    
    tcb_t *current		= get_current_tcb ();
    
    // Handle VM exit
    arch_hvm_ktcb_handle_hvm_exit (&current->arch.hvm);

    x86_mmu_flush_tlb (true);

    // Reenter the VM.
    x86_x32_vmx_vmresume (get_user_frame(current));
}


void vmexit_entry_point_wrapper ()
{
    word_t *do_handle_vmexit_ptr = (word_t *) do_handle_vmexit;
    asm (
	".globl vmexit_entry_point	\n"
	".type vmexit_entry_point,@function \n"
	"vmexit_entry_point:		\n"
	"	push %%eax		\n"
	"	push %%ecx		\n"
	"	push %%edx		\n"
	"	push %%ebx		\n"
	"	pushf			\n"
	"	push %%ebp		\n"
	"	push %%esi		\n"
	"	push %%edi		\n"
	// ds, es, reason
	"	subl  $12, %%esp	\n"

	/*****************************************
	 * Restore kernel context.
	 *****************************************/

	// Load segment selectors.
	// Selectors set in VMCS host state cannot be user-accessible.

	// DS, ES.
#if !defined(CONFIG_X86_X32_SMALL_SPACES)
	"	mov %0, %%bx		\n"
	"	mov %%bx, %%ds		\n"
	"	mov %%bx, %%es		\n"
	"	mov %%bx, %%fs		\n"
#endif
	// GS.
	"	mov %1, %%bx		\n"
	"	mov %%bx, %%gs		\n"

	// EFLAGS.
	//  VM-Exit has a cleared eflags (except bit 1, which is always 1)
	"       pushl %2		\n"
	"	popfl			\n"

	// Call do_handle_vmexit.
	"	jmp %3			\n"
	:
	: "i" (X86_UDS),					// %0
	  "i" (X86_UTCBS),					// %1
	  "i" (X86_KERNEL_FLAGS),				// %2
	  "m" (*do_handle_vmexit_ptr)                           // %3
	);
}
