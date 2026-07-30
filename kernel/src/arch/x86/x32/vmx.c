/*********************************************************************
 *
 * Copyright (C) 2006-2007,  Karlsruhe University
 *
 * File path:     arch/ia32/vmx/vmcs.cc
 * Description:   Vanderpool Virtual Machine Extensions
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

#include <kmemory.h>
#include <debug.h>
#include INC_ARCH(segdesc.h)
#include INC_ARCH_SA(vmx.h)

DECLARE_KMEM_GROUP (kmem_vmcs);

/*
 * The vmcs_t and per-area methods this file defines are declared in
 * arch/x86/vmx.h; see the comment there on how a VMCS field became a pair of
 * accessors over an explicit vmcs_t *.  Every `field = x' below is now
 * vmcs_<area>_set_<field> (self, x) and every read the matching get, so the
 * VMREADs and VMWRITEs happen in the same order, and the same number of times,
 * as they did through the template's operator= / operator T.
 */

INLINE bool is_canonical_address (word_t addr)
{
    (void) addr;
    return true;
}


enum x86_x32_vmcs_access_e {
    uc = 1,
    wb = 6
};

typedef union {
    u64_t raw;
    struct {
	u64_t vmcs_rev_id	: 32;
	u64_t vmcs_sz		: 13;
	u64_t res1		:  5;
	u64_t vmcs_memtype	:  4;
	u64_t res2		: 10;
    };
} basic_msr_t;

INLINE u16_t get_vmcs_sz (void)
{
    basic_msr_t msr;
    msr.raw = x86_rdmsr (X86_MSR_VMX_BASIC);
    return (u16_t) msr.vmcs_sz;
}

INLINE enum x86_x32_vmcs_access_e get_vmcs_access_mode (void)
{
    basic_msr_t msr;
    msr.raw = x86_rdmsr (X86_MSR_VMX_BASIC);
    return (enum x86_x32_vmcs_access_e) msr.vmcs_memtype;
}

INLINE u32_t get_vmcs_rev_id (void)
{
    basic_msr_t msr;
    msr.raw = x86_rdmsr (X86_MSR_VMX_BASIC);
    return (u32_t) msr.vmcs_rev_id;
}


/*******************************************************************************
 *
 * VMCS
 *
 *******************************************************************************/

vmcs_t *current_vmcs UNIT("cpulocal");


vmcs_t *vmcs_alloc_vmcs (void)
{
    word_t sz;
    addr_t vmcs;
    u32_t *ptr;

    // Kernel Memory Allocator gives WB memory.
    if (get_vmcs_access_mode() != wb)
	return NULL;

    // Allocate VMCS Region, must be aligned to page boundary
    sz = X86_PAGE_SIZE;
    vmcs = kmem_alloc(&kmem, kmem_vmcs, sz);
    if (!vmcs)
	return NULL;

    // Set Revision.
    ptr = (u32_t *) vmcs;
    *ptr = get_vmcs_rev_id();

    //TRACEF ("New VMCS created: va: %p  pa: %p  sz: %x rev: %lx\n",
    //       vmcs, virt_to_phys(vmcs), sz, *ptr);

    return (vmcs_t *) virt_to_phys(vmcs);
}


void vmcs_free_vmcs (vmcs_t *vmcs)
{
    kmem_free(&kmem, kmem_vmcs, phys_to_virt(vmcs), get_vmcs_sz());
}


void vmcs_init (vmcs_t *self)
{
    vmcs_gsarea_init (self);
    vmcs_exectrarea_init (self);
    vmcs_exitctrarea_init (self);
    vmcs_entryctrarea_init (self);
}


/*******************************************************************************
 *
 * VMCS Entry Control
 *
 *******************************************************************************/

void vmcs_entryctrarea_init (vmcs_t *self)
{
    vmcs_entry_ctr_set_entryctr (self, vmcs_entryctr_new ());

    vmcs_entry_ctr_set_iif (self, vmcs_int_new ());

    vmcs_entry_ctr_set_msr_ld_cnt (self, 0);
    vmcs_entry_ctr_set_msr_ld_addr (self, 0);

    vmcs_entry_ctr_set_instr_len (self, 0);

    vmcs_entry_ctr_set_eec (self, 0);
}


/*******************************************************************************
 *
 * VMCS Execution Control
 *
 *******************************************************************************/

void vmcs_exectrarea_init (vmcs_t *self)
{
    vmcs_exec_ctr_set_pinbased (self, vmcs_exectr_pinbased_new ());

    vmcs_exec_ctr_set_cpubased (self, vmcs_exectr_cpubased_new ());

    vmcs_exec_ctr_set_except_bmp (self, vmcs_exectr_excbmp_new ());

    vmcs_exec_ctr_set_pferrmask (self, 0);
    vmcs_exec_ctr_set_pferrmatch (self, 0);

    vmcs_exec_ctr_set_tprthr (self, vmcs_exectr_tprth_new ());

    vmcs_exec_ctr_set_iobmpa (self, 0);
    vmcs_exec_ctr_set_iobmpb (self, 0);

    vmcs_exec_ctr_set_tscoff (self, 0);
    vmcs_exec_ctr_set_vapicaddr (self, 0);

    // Masks: Owned by the host.
    vmcs_exec_ctr_set_cr0mask (self, ~0UL);
    vmcs_exec_ctr_set_cr4mask (self, ~0UL);

    // Shadow: The values seen by the guest, if all bits in the masks are set.
    vmcs_exec_ctr_set_cr0shadow (self, X86_CR0_ET | X86_CR0_NW | X86_CR0_CD);
    vmcs_exec_ctr_set_cr4shadow (self, 0);

    // cr3
    vmcs_exec_ctr_set_cr3targetcnt (self, 0);
    vmcs_exec_ctr_set_cr3val0 (self, 0);
    vmcs_exec_ctr_set_cr3val1 (self, 0);
    vmcs_exec_ctr_set_cr3val2 (self, 0);
    vmcs_exec_ctr_set_cr3val3 (self, 0);
}


/*******************************************************************************
 *
 * VMCS Exit Control
 *
 *******************************************************************************/

void vmcs_exitctrarea_init (vmcs_t *self)
{
    vmcs_exit_ctr_set_exitctr (self, vmcs_exitctr_new ());

    vmcs_exit_ctr_set_msr_st_cnt (self, 0);
    vmcs_exit_ctr_set_msr_ld_cnt (self, 0);
    vmcs_exit_ctr_set_msr_st_addr (self, 0);
    vmcs_exit_ctr_set_msr_ld_addr (self, 0);
}


/*******************************************************************************
 *
 * VMCS Guest State
 *
 *******************************************************************************/

void vmcs_gsarea_init (vmcs_t *self)
{
    /* The chained assignments the C++ body used -- cs_sel = ds_sel = ... = 0 --
       each issued one VMWRITE per field through operator=; they are spelled out
       one call per field here. */
    vmcs_segattr_t i_attr = vmcs_segattr_new ();
    vmcs_gs_as_t i_as;

    vmcs_gs_set_rflags (self, X86_FLAGS_VM | X86_FLAGS_IOPL(3) | 0x2);
    vmcs_gs_set_rip (self, 0);
    vmcs_gs_set_rsp (self, 0);

    vmcs_gs_set_cs_sel (self, 0);
    vmcs_gs_set_ds_sel (self, 0);
    vmcs_gs_set_es_sel (self, 0);
    vmcs_gs_set_fs_sel (self, 0);
    vmcs_gs_set_gs_sel (self, 0);
    vmcs_gs_set_ss_sel (self, 0);
    vmcs_gs_set_tr_sel (self, 0);
    vmcs_gs_set_ldtr_sel (self, 0);

    vmcs_gs_set_cs_base (self, 0);
    vmcs_gs_set_ds_base (self, 0);
    vmcs_gs_set_es_base (self, 0);
    vmcs_gs_set_fs_base (self, 0);
    vmcs_gs_set_gs_base (self, 0);
    vmcs_gs_set_ss_base (self, 0);
    vmcs_gs_set_tr_base (self, 0);
    vmcs_gs_set_ldtr_base (self, 0);
    vmcs_gs_set_gdtr_base (self, 0);
    vmcs_gs_set_idtr_base (self, 0);

    vmcs_gs_set_cs_lim (self, 0xffff);
    vmcs_gs_set_ds_lim (self, 0xffff);
    vmcs_gs_set_es_lim (self, 0xffff);
    vmcs_gs_set_fs_lim (self, 0xffff);
    vmcs_gs_set_gs_lim (self, 0xffff);
    vmcs_gs_set_ss_lim (self, 0xffff);
    vmcs_gs_set_tr_lim (self, 0xffff);
    vmcs_gs_set_ldtr_lim (self, 0xffff);
    vmcs_gs_set_gdtr_lim (self, 0xffff);
    vmcs_gs_set_idtr_lim (self, 0xffff);

    i_attr.raw		= 0xf3;
    vmcs_gs_set_cs_attr (self, i_attr);
    vmcs_gs_set_ds_attr (self, i_attr);
    vmcs_gs_set_es_attr (self, i_attr);
    vmcs_gs_set_fs_attr (self, i_attr);
    vmcs_gs_set_gs_attr (self, i_attr);
    vmcs_gs_set_ss_attr (self, i_attr);

    i_attr.raw		= 0x0808b;
    vmcs_gs_set_tr_attr (self, i_attr);

    i_attr.raw		= 0x10082;
    vmcs_gs_set_ldtr_attr (self, i_attr);

    vmcs_gs_set_ias (self, vmcs_gs_ias_new ());

    i_as		= vmcs_gs_as_new ();
    i_as.state		= VMCS_AS_ACTIVE;
    vmcs_gs_set_as (self, i_as);

    vmcs_gs_set_pend_dbg_except (self, vmcs_gs_pend_dbg_except_new ());

    vmcs_gs_set_linkptr (self, ~0ULL);

    vmcs_gs_set_dbg_ctl (self, 0);

    vmcs_gs_set_sysenter_esp (self, 0);
    vmcs_gs_set_sysenter_eip (self, 0);
}


/*******************************************************************************
 *
 * Debugging
 *
 *******************************************************************************/

#if defined(CONFIG_DEBUG)

static const u32_t BITS_15_03 = ((1UL<<14) - 1) << 3;
static const u32_t BITS_15_02 = ((1UL<<15) - 1) << 2;
static const u32_t BITS_31_04 = ((1UL<<29) - 1) << 4;
static const u32_t BITS_31_15 = ((1UL<<18) - 1) << 15;
static const u32_t BITS_31_16 = ((1UL<<17) - 1) << 16;
static const u32_t BITS_31_17 = ((1UL<<16) - 1) << 17;
static const u32_t BITS_31_20 = ((1UL<<13) - 1) << 20;
static const u32_t BITS_30_12 = ((1UL<<19) - 1) << 12;
static const u32_t BITS_11_00 = ((1UL<<12) - 1);
static const u32_t BITS_11_04 = ((1UL<<8) - 1) << 4;
static const u32_t BITS_11_08 = ((1UL<<4) - 1) << 8;
static const u64_t BITS_63_15 =	((1ULL<<50) - 1) << 15;
static const u64_t BITS_63_32 = ((1ULL<<33) - 1) << 32;
static const u64_t BITS_63_22 = ((1ULL<<43) - 1) << 22;


/* Table Indicator; was vmcs_segsel_t::ti_e. */
enum vmcs_segsel_ti_e {
   VMCS_SEGSEL_GDT = 0, VMCS_SEGSEL_LDT = 1
};

struct vmcs_segsel_t {
    union {
	u16_t raw;
	struct {
	    u16_t rpl   : 2;
	    u16_t ti    : 1; // vmcs_segsel_ti_e
	    u16_t idx   :13;
	};
    };
};
typedef struct vmcs_segsel_t vmcs_segsel_t;

INLINE vmcs_segsel_t vmcs_segsel_new (void)
{ vmcs_segsel_t v; v.raw = 0; return v; }


void vmcs_do_vmentry_checks (vmcs_t *self)
{
    vmcs_entryctr_t i_entryctr;
    vmcs_int_t i_iif;
    vmcs_exitctr_t i_exitctr;

    // VT-Specification Chapter 4.1

    // 1. Virtual-8068 or Compatibility Mode (-> invalid-opcode)
    // 2. CPL is != 0.
    // 3. No Current VMCS.
    ASSERT(vmcs_is_loaded (self));
    // 4a. MOV-SS blocking.
    //vmcs_gs_ias_t interruptililty = vmcs_gs_get_ias (self);
    //ASSERT(interruptililty.bl_movss == 0);
    // 4b. VMLAUNCH on non-clear VMCS.
    // 4c. VMRESUME on non-launched VMCS.

    i_entryctr = vmcs_entry_ctr_get_entryctr (self);
    i_iif = vmcs_entry_ctr_get_iif (self);
    i_exitctr = vmcs_exit_ctr_get_exitctr (self);

    vmcs_gsarea_do_vmentry_checks (self, i_entryctr, i_iif);
    vmcs_hsarea_do_vmentry_checks (self, i_entryctr, i_exitctr);
    vmcs_exectrarea_do_vmentry_checks (self);
    vmcs_exitctrarea_do_vmentry_checks (self);
    vmcs_entryctrarea_do_vmentry_checks (self);
}


void vmcs_entryctrarea_do_vmentry_checks (vmcs_t *self)
{
    // Entry Control.		(22.2.1.3)
    vmcs_entryctr_t i_entryctr;
    u64_t i_entrydefs;
    u32_t ALLOWED0, ALLOWED1;
    vmcs_entryctr_t i_entryctr_check;
    vmcs_int_t i_iif;
    u32_t i_msr_ld_cnt;

    i_entryctr	      = vmcs_entry_ctr_get_entryctr (self);
    i_entrydefs = x86_rdmsr (X86_MSR_VMX_ENTRY_CTLS);
    ALLOWED0 = (u32_t)i_entrydefs;
    ALLOWED1 = (u32_t) (i_entrydefs >> 32);

    i_entryctr_check.raw = i_entryctr.raw | ALLOWED0;
    i_entryctr_check.raw = i_entryctr_check.raw & ALLOWED1;
    if (i_entryctr_check.raw != i_entryctr.raw)
    {
	printf("ALLOWED0   %lx\n", ALLOWED0);
	printf("ALLOWED1   %lx\n", ALLOWED1);
	printf("i_entryctr %lx\n", i_entryctr.raw);
	printf("..._check  %lx\n", i_entryctr_check.raw);
	ASSERT(i_entryctr_check.raw == i_entryctr.raw);
    }

    // Event Injection.		(22.2.1.3)
    i_iif	= vmcs_entry_ctr_get_iif (self);
    if (i_iif.valid) {
	u32_t i_eec;
	u32_t i_instr_len;

	ASSERT((i_iif.type != 1) &&
	       (i_iif.type != 7));

	i_eec	= vmcs_entry_ctr_get_eec (self);
	//	printf("iif: %x, eec: %x\n", i_iif.raw, i_eec);
	if (i_iif.type == VMCS_INT_HW_NMI)
	    ASSERT(i_iif.vector == 2);
	if (i_iif.type == VMCS_INT_HW_EXCEPT)
	    ASSERT(i_iif.vector <= 31);

	if (i_iif.err_code_valid == 1) {
	    ASSERT(i_iif.type == VMCS_INT_HW_EXCEPT);
	}

	if (i_iif.raw & (BITS_30_12))
	    printf("i_iif.raw %p BITS_30_12 %p\n", i_iif.raw, BITS_30_12);
	ASSERT((i_iif.raw & (BITS_30_12)) == 0);
	if (i_iif.err_code_valid == 1) {
	    ASSERT((i_eec & (BITS_31_15)) == 0);
	}

	i_instr_len	= vmcs_entry_ctr_get_instr_len (self);
	if (i_iif.type >= VMCS_INT_SW_INT)
	{
	    ASSERT((i_instr_len >= 1) && (i_instr_len <= 15));
	}
    }

    // MSRs.
    i_msr_ld_cnt	= vmcs_entry_ctr_get_msr_ld_cnt (self);
    if(i_msr_ld_cnt != 0) {
	UNIMPLEMENTED();
    }

    // SMM checks missing.

    ASSERT( !((i_entryctr.entry_smm == 1) && (i_entryctr.deact_dmt == 1)) );
}


void vmcs_exectrarea_do_vmentry_checks (vmcs_t *self)
{
    // Pin Based.		(22..2.1.1)
    vmcs_exectr_pinbased_t i_pb;
    u64_t i_pbctls;
    u32_t ALLOWED0, ALLOWED1;
    vmcs_exectr_pinbased_t i_pb_check;
    vmcs_exectr_cpubased_t i_cb, i_cb_check;
    u64_t i_cbctls;
    u32_t i_cr3_count;

    i_pb	= vmcs_exec_ctr_get_pinbased (self);
    i_pbctls = x86_rdmsr (X86_MSR_VMX_PINBASED_CTLS);
    ALLOWED0 = (u32_t)i_pbctls;
    ALLOWED1 = (u32_t) (i_pbctls >> 32);

    i_pb_check.raw = i_pb.raw | ALLOWED0;
    i_pb_check.raw = i_pb_check.raw & ALLOWED1;
    if (i_pb_check.raw != i_pb.raw)
    {
	printf("ALLOWED0   %lx\n", ALLOWED0);
	printf("ALLOWED1   %lx\n", ALLOWED1);
	printf("i_pb       %lx\n", i_pb.raw);
	printf("i_pb_check %lx\n", i_pb_check.raw);
	ASSERT(i_pb_check.raw == i_pb.raw);
    }

    // CPU Based.
    i_cb		= vmcs_exec_ctr_get_cpubased (self);
    i_cbctls	= x86_rdmsr (X86_MSR_VMX_CPUBASED_CTLS);
    ALLOWED0 = (u32_t)i_cbctls;
    ALLOWED1 = (u32_t) (i_cbctls >> 32);
    i_cb_check.raw = i_cb.raw | ALLOWED0;
    i_cb_check.raw = i_cb_check.raw & ALLOWED1;
    if (i_cb_check.raw != i_cb.raw)
    {
	printf("ALLOWED0   %lx\n", ALLOWED0);
	printf("ALLOWED1   %lx\n", ALLOWED1);
	printf("i_cb       %lx\n", i_cb.raw);
	printf("i_cb_check %lx\n", i_cb_check.raw);
	ASSERT(i_cb_check.raw == i_cb.raw);
    }

    // Cr3.
    i_cr3_count	= vmcs_exec_ctr_get_cr3targetcnt (self);
    ASSERT(i_cr3_count <= 4);

    // IO-Bitmap.
    if (i_cb.iobitm)
    {
	u64_t i_iobmpa = vmcs_exec_ctr_get_iobmpa (self);
	ASSERT((i_iobmpa & ~X86_PAGE_MASK) == 0);
    }

    // TPR Shadow.
    if (i_cb.tpr_shadow)
	UNIMPLEMENTED();

    // MSR-Bitmaps.
    if (i_cb.msrbitm)
	UNIMPLEMENTED();
}


void vmcs_exitctrarea_do_vmentry_checks (vmcs_t *self)
{
    // Exit Control.		(22.2.1.2)
    vmcs_exitctr_t i_exitctr;
    u64_t i_exitdefs;
    u32_t ALLOWED0, ALLOWED1;
    vmcs_exitctr_t i_exitctr_check;

    i_exitctr		= vmcs_exit_ctr_get_exitctr (self);
    i_exitdefs	= x86_rdmsr (X86_MSR_VMX_EXIT_CTLS);
    ALLOWED0	= (u32_t)i_exitdefs;
    ALLOWED1	= (u32_t) (i_exitdefs >> 32);

    i_exitctr_check.raw = i_exitctr.raw | ALLOWED0;
    i_exitctr_check.raw = i_exitctr_check.raw & ALLOWED1;
    if (i_exitctr_check.raw != i_exitctr.raw)
    {
	printf("ALLOWED0  %lx\n", ALLOWED0);
	printf("ALLOWED1  %lx\n", ALLOWED1);
	printf("i_exitctr %lx\n", i_exitctr.raw);
	printf("..._check %lx\n", i_exitctr_check.raw);
	ASSERT(i_exitctr_check.raw == i_exitctr.raw);
    }

    // MSRs.
    if (vmcs_exit_ctr_get_msr_st_cnt (self) != 0)
	UNIMPLEMENTED();
    if (vmcs_exit_ctr_get_msr_ld_cnt (self) != 0)
	UNIMPLEMENTED();
}


void vmcs_gsarea_do_vmentry_checks (vmcs_t *self, vmcs_entryctr_t i_entryctr, vmcs_int_t i_iif)
{
    vmcs_gs_ias_t i_ias;
    vmcs_gs_as_t  i_as;
    word_t i_rip, i_cr0, i_cr3, i_dr7, i_sysenter_esp, i_sysenter_eip;
    u32_t i_gdtr_limit, i_idtr_limit;
    word_t i_rfl, i_cr4;
    bool virt8086, ia32e;
    vmcs_segattr_t i_cs_attr, i_ss_attr, i_ds_attr, i_es_attr;
    vmcs_segattr_t i_fs_attr, i_gs_attr, i_tr_attr, i_ldtr_attr;
    vmcs_segsel_t i_cs_sel, i_ss_sel, i_ds_sel, i_es_sel;
    vmcs_segsel_t i_fs_sel, i_gs_sel, i_tr_sel, i_ldtr_sel;
    u32_t i_cs_lim, i_ss_lim, i_ds_lim, i_es_lim, i_fs_lim, i_gs_lim;
    u32_t i_tr_lim, i_ldtr_lim;
    u64_t i_cs_base, i_ss_base, i_ds_base, i_es_base, i_fs_base, i_gs_base;
    vmcs_gs_pend_dbg_except_t i_pend_dbg_except;

    i_ias	= vmcs_gs_get_ias (self);

    i_as	= vmcs_gs_get_as (self);

    i_rip	= vmcs_gs_get_rip (self);
    i_cr0	= vmcs_gs_get_cr0 (self);
    i_cr3	= vmcs_gs_get_cr3 (self);
    i_dr7	= vmcs_gs_get_dr7 (self);
    i_sysenter_esp = vmcs_gs_get_sysenter_esp (self);
    i_sysenter_eip = vmcs_gs_get_sysenter_eip (self);
    i_gdtr_limit	= vmcs_gs_get_gdtr_lim (self);
    i_idtr_limit	= vmcs_gs_get_idtr_lim (self);

    i_rfl	= vmcs_gs_get_rflags (self);
    i_cr4	= vmcs_gs_get_cr4 (self);

    virt8086 = (i_rfl & X86_FLAGS_VM);
    ia32e = (i_entryctr.ia32e_mode);
    ASSERT(ia32e == false);

    // CR0		(22.3.1.1)
    ASSERT(x86_x32_vmx_check_fixed_bits_cr0 (i_cr0));

    // CR3
    ASSERT(is_canonical_address (i_cr3));

    // CR4
    ASSERT(x86_x32_vmx_check_fixed_bits_cr4 (i_cr4));
    if (i_entryctr.ia32e_mode)
	ASSERT((i_cr4 & X86_CR4_PAE) != 0);

    // DRs		(22.3.1.1)
    ASSERT((i_dr7 & (BITS_63_32)) == 0);

    // Sysenter / -exit (22.3.1.1)
    ASSERT(is_canonical_address (i_sysenter_esp));
    ASSERT(is_canonical_address (i_sysenter_eip));

    // Segments		(22.3.1.2)
    i_cs_attr	= vmcs_gs_get_cs_attr (self);
    i_ss_attr	= vmcs_gs_get_ss_attr (self);
    i_ds_attr	= vmcs_gs_get_ds_attr (self);
    i_es_attr	= vmcs_gs_get_es_attr (self);
    i_fs_attr	= vmcs_gs_get_fs_attr (self);
    i_gs_attr	= vmcs_gs_get_gs_attr (self);
    i_tr_attr	= vmcs_gs_get_tr_attr (self);
    i_ldtr_attr	= vmcs_gs_get_ldtr_attr (self);

    i_cs_sel.raw	= vmcs_gs_get_cs_sel (self);
    i_ss_sel.raw	= vmcs_gs_get_ss_sel (self);
    i_ds_sel.raw	= vmcs_gs_get_ds_sel (self);
    i_es_sel.raw	= vmcs_gs_get_es_sel (self);
    i_fs_sel.raw	= vmcs_gs_get_fs_sel (self);
    i_gs_sel.raw	= vmcs_gs_get_gs_sel (self);
    i_tr_sel.raw	= vmcs_gs_get_tr_sel (self);
    i_ldtr_sel.raw	= vmcs_gs_get_ldtr_sel (self);

    i_cs_lim		= vmcs_gs_get_cs_lim (self);
    i_ss_lim		= vmcs_gs_get_ss_lim (self);
    i_ds_lim		= vmcs_gs_get_ds_lim (self);
    i_es_lim		= vmcs_gs_get_es_lim (self);
    i_fs_lim		= vmcs_gs_get_fs_lim (self);
    i_gs_lim		= vmcs_gs_get_gs_lim (self);
    i_tr_lim		= vmcs_gs_get_tr_lim (self);
    i_ldtr_lim		= vmcs_gs_get_ldtr_lim (self);

    i_cs_base	= vmcs_gs_get_cs_base (self);
    i_ss_base	= vmcs_gs_get_ss_base (self);
    i_ds_base	= vmcs_gs_get_ds_base (self);
    i_es_base	= vmcs_gs_get_es_base (self);
    i_fs_base	= vmcs_gs_get_fs_base (self);
    i_gs_base	= vmcs_gs_get_gs_base (self);
    // 22.3.1.2 Chapter 22-8
    ASSERT (i_tr_sel.ti == 0);
    if (i_ldtr_attr.uu == 0)
	ASSERT (i_ldtr_sel.ti == 0);
    if (!virt8086)
	//ASSERT(i_ss_sel.rpl == i_cs_sel.rpl);
      	if(i_ss_sel.rpl != i_cs_sel.rpl)
	{
	    i_cs_sel.rpl = i_ss_sel.rpl = 0;
	    vmcs_gs_set_cs_sel (self, i_cs_sel.raw);
	    vmcs_gs_set_ss_sel (self, i_ss_sel.raw);
	    i_cs_attr.dpl = 0;
	    i_ss_attr.dpl = 0;
	    vmcs_gs_set_cs_attr (self, i_cs_attr);
	    vmcs_gs_set_ss_attr (self, i_ss_attr);
	}

    ASSERT ((i_cs_base & (BITS_63_32)) == 0);

    if (i_ss_attr.uu == 0)
	ASSERT ((vmcs_gs_get_ss_base (self) & (BITS_63_32)) == 0);
    if (i_ds_attr.uu == 0)
	ASSERT ((vmcs_gs_get_ds_base (self) & (BITS_63_32)) == 0);
    if (i_es_attr.uu == 0)
	ASSERT ((vmcs_gs_get_es_base (self) & (BITS_63_32)) == 0);

    // Limit fields, access rights.
    if (virt8086)
    {
	if  (i_cs_base != ((u32_t) i_cs_sel.raw) << 4)
	    printf("%x vs %x\n", i_cs_base, i_cs_sel.raw);
	ASSERT(i_cs_base == ((u32_t) i_cs_sel.raw) << 4);
	ASSERT(i_ss_base == ((u32_t) i_ss_sel.raw) << 4);
	ASSERT(i_ds_base == ((u32_t) i_ds_sel.raw) << 4);
	ASSERT(i_es_base == ((u32_t) i_es_sel.raw) << 4);
	ASSERT(i_fs_base == ((u32_t) i_fs_sel.raw) << 4);
	ASSERT(i_gs_base == ((u32_t) i_gs_sel.raw) << 4);

	ASSERT(i_cs_lim == 0x0000FFFF);
	ASSERT(i_ss_lim == 0x0000FFFF);
	ASSERT(i_ds_lim == 0x0000FFFF);
	ASSERT(i_es_lim == 0x0000FFFF);
	ASSERT(i_fs_lim == 0x0000FFFF);
	ASSERT(i_gs_lim == 0x0000FFFF);

	ASSERT(i_cs_attr.raw == 0x000000F3);
	ASSERT(i_ss_attr.raw == 0x000000F3);
	ASSERT(i_ds_attr.raw == 0x000000F3);
	ASSERT(i_es_attr.raw == 0x000000F3);
	ASSERT(i_fs_attr.raw == 0x000000F3);
	ASSERT(i_gs_attr.raw == 0x000000F3);
    }
    else
    {
	// CS.
	ASSERT ((i_cs_attr.type & (1<<0)) != 0);
	ASSERT ((i_cs_attr.type & (1<<3)) != 0);
	// SS:
	if (i_ss_attr.uu == 0)
	    ASSERT((i_ss_attr.type == 3) ||
		   (i_ss_attr.type == 7));
	// DS.
	if (i_ds_attr.uu == 0) {
	    ASSERT((i_ds_attr.type & (1<<0)) != 0);
	    if ((i_ds_attr.type & (1<<3)) == 1)
		ASSERT((i_ds_attr.type & (1<<1)) != 0);
	}
	// ES.
	if (i_es_attr.uu == 0) {
	    ASSERT((i_es_attr.type & (1<<0)) != 0);
	    if ((i_es_attr.type & (1<<3)) != 0)
		ASSERT((i_es_attr.type & (1<<1)) != 0);
	}

	// FS.
	if (i_fs_attr.uu == 0) {
	    ASSERT((i_fs_attr.type & (1<<0)) != 0);
	    if ((i_fs_attr.type & (1<<3)) != 0)
		ASSERT((i_fs_attr.type & (1<<1)) != 0);
	}
	// GS.
	if (i_gs_attr.uu == 0) {
	    ASSERT((i_gs_attr.type & (1<<0)) != 0);
	    if ((i_gs_attr.type & (1<<3)) != 0)
		ASSERT((i_gs_attr.type & (1<<1)) != 0);
	}

	// S bit.
	ASSERT (i_cs_attr.s == 1);
	if (i_ss_attr.uu == 0)
	    ASSERT (i_ss_attr.s == 1);
	if (i_ds_attr.uu == 0)
	    ASSERT (i_ds_attr.s == 1);
	if (i_es_attr.uu == 0)
	    ASSERT (i_es_attr.s == 1);
	if (i_fs_attr.uu == 0)
	    ASSERT (i_fs_attr.s == 1);
	if (i_gs_attr.uu == 0)
	    ASSERT (i_gs_attr.s == 1);

	// DPL for CS.
	if ((i_cs_attr.type >= 8) && (i_cs_attr.type <= 11))
	    ASSERT(i_cs_attr.dpl == i_cs_sel.rpl);
	if ((i_cs_attr.type >= 13) && (i_cs_attr.type <= 15))
	    ASSERT(i_cs_attr.dpl <= i_cs_sel.rpl);
	// DPL for SS
	ASSERT(i_ss_attr.dpl == i_ss_sel.rpl);
	// DPL for DS,ES,FS,GS
	if ((i_ds_attr.uu == 0) && (i_ds_attr.type  <= 11))
	    ASSERT(i_ds_attr.dpl >= i_ds_sel.rpl);
	if ((i_es_attr.uu == 0) && (i_es_attr.type  <= 11))
	    ASSERT(i_es_attr.dpl >= i_es_sel.rpl);
	if ((i_fs_attr.uu == 0) && (i_fs_attr.type  <= 11))
	    ASSERT(i_fs_attr.dpl >= i_fs_sel.rpl);
	if ((i_gs_attr.uu == 0) && (i_gs_attr.type  <= 11))
	    ASSERT(i_gs_attr.dpl >= i_gs_sel.rpl);

	// P.
	if (i_cs_attr.uu == 0)
	    ASSERT(i_cs_attr.p == 1);
	// 11:8 Reserved.
	ASSERT((i_cs_attr.raw & (BITS_11_08)) == 0);
	if (i_ss_attr.uu == 0)
	    ASSERT((i_ss_attr.raw & (BITS_11_08)) == 0);
	if (i_ds_attr.uu == 0)
	    ASSERT((i_ds_attr.raw & (BITS_11_08)) == 0);
	if (i_es_attr.uu == 0)
	    ASSERT((i_es_attr.raw & (BITS_11_08)) == 0);
	if (i_fs_attr.uu == 0)
	    ASSERT((i_fs_attr.raw & (BITS_11_08)) == 0);
	if (i_gs_attr.uu == 0)
	    //	    ASSERT((i_gs_attr.raw & (BITS_11_08)) == 0);
	    if(	(i_gs_attr.raw & (BITS_11_08)) != 0) {
		printf("GS raw: %x\n", i_gs_attr.raw);
		ASSERT(0);
	    }

	// 14 d/b.
	if (ia32e && (i_cs_attr.l == 1))
	    ASSERT (i_cs_attr.db == 0);

	// G.
	if (i_cs_lim & ((BITS_11_00) == 0)) {
	    ASSERT(i_cs_attr.g == 0);
	}
	if ((i_cs_lim & (BITS_31_20)) != 0) {
	    ASSERT(i_cs_attr.g == 1);
	}
	ASSERT((i_cs_attr.raw & BITS_31_17) == 0);
	// reserved, G, for SS
	if (i_ss_attr.uu == 0) {
	    if ((i_ss_lim & (BITS_11_00)) == 0)
		ASSERT(i_ss_attr.g == 0);
	    if ((i_ss_lim & (BITS_31_20)) != 0)
		ASSERT(i_ss_attr.g == 1);
	    ASSERT((i_ss_attr.raw & BITS_31_17) == 0);
	}
	// reserved, G, for DS
	if (i_ds_attr.uu == 0) {
	    if ((i_ds_lim & (BITS_11_00)) == 0)
		ASSERT(i_ds_attr.g == 0);
	    if ((i_ds_lim & (BITS_31_20)) != 0)
		ASSERT(i_ds_attr.g == 1);
	    ASSERT((i_ds_attr.raw & BITS_31_17) == 0);
	}
	// reserved, G, for ES
	if (i_es_attr.uu == 0) {
	    if ((i_es_lim & (BITS_11_00)) == 0)
		ASSERT(i_es_attr.g == 0);
	    if ((i_es_lim & (BITS_31_20)) != 0)
		ASSERT(i_es_attr.g == 1);
	    ASSERT((i_es_attr.raw & BITS_31_17) == 0);
	}
	// reserved, G, for FS
	if (i_fs_attr.uu == 0) {
	    if ((i_fs_lim & (BITS_11_00)) == 0)
		ASSERT(i_fs_attr.g == 0);
	    if ((i_fs_lim & (BITS_31_20)) != 0)
		ASSERT(i_fs_attr.g == 1);
	    ASSERT((i_fs_attr.raw & BITS_31_17) == 0);
	}
	// reserved, G, for GS
	if (i_gs_attr.uu == 0) {
	    if ((i_gs_lim & (BITS_11_00)) == 0)
		ASSERT(i_gs_attr.g == 0);
	    if ((i_gs_lim & (BITS_31_20)) != 0)
		ASSERT(i_gs_attr.g == 1);
	    ASSERT((i_gs_attr.raw & BITS_31_17) == 0);
	}
    } // Access Rights (!virt8086)

    // TR.
    if (!virt8086)
    {
	ASSERT ((i_tr_attr.type == 3) ||
		(i_tr_attr.type == 11));
    }
    if (ia32e)
	ASSERT (i_tr_attr.type == 11);
    ASSERT (i_tr_attr.s == 0);
    ASSERT (i_tr_attr.p == 1);
    if ((i_tr_lim & (BITS_11_00)) == 0) {
	ASSERT (i_tr_attr.g == 0);
    }
    if ((i_tr_lim & (BITS_31_20)) != 0) {
	ASSERT (i_tr_attr.g == 1);
    }
    ASSERT (i_tr_attr.uu == 0);
    ASSERT ((i_tr_attr.raw & (BITS_31_17)) == 0);

    if (i_ldtr_attr.uu == 0) {
	ASSERT (i_ldtr_attr.type == 2);
	ASSERT (i_ldtr_attr.s == 0);
	ASSERT (i_ldtr_attr.p == 1);
	ASSERT ((i_ldtr_attr.raw & (BITS_11_08)) == 0);
	if ((i_ldtr_lim & (BITS_11_00)) != (BITS_11_00))
	    ASSERT (i_ldtr_attr.g == 0);
	if ((i_ldtr_lim & (BITS_31_20)) != 0)
	    ASSERT (i_ldtr_attr.g == 1);
	ASSERT ((i_ldtr_attr.raw & (BITS_31_17)) == 0);

    }

    // Descriptor-Table Registers (22.3.1.3)
    ASSERT((i_gdtr_limit & (BITS_31_16)) == 0);
    ASSERT((i_idtr_limit & (BITS_31_16)) == 0);

    // RIP		(22.3.1.4)
    if (i_entryctr.ia32e_mode == 0)
    {
	ASSERT((i_rip & BITS_63_32) == 0);
    }
    ASSERT(is_canonical_address(i_rip));

    // RFLAGS		(22.3.1.4)
    ASSERT((i_rfl & (BITS_63_22 | (1UL<<15) | (1UL<<5) | (1UL<<3))) == 0);
    ASSERT((i_rfl & (1<<1)) != 0);
    if (i_entryctr.ia32e_mode == 1)
	ASSERT ((i_rfl & X86_FLAGS_VM) == 0);
    if ((i_iif.valid == 1) &&
	(i_iif.type == VMCS_INT_EXT_INT))
	ASSERT((i_rfl & (X86_FLAGS_IF)) != 0);

    // Activity State	(22.3.1.5)
    ASSERT(i_as.raw <= 3);
    if (i_as.state == VMCS_AS_HLT)
	ASSERT(i_ss_attr.dpl == 0);
    if ((i_ias.bl_movss == 1) || (i_ias.bl_sti == 1))
	ASSERT(i_as.state == VMCS_AS_ACTIVE);
    if (i_iif.valid == 1)
    {
	if (i_as.state == VMCS_AS_HLT)
	    enter_kdebug("checks missing hlt");
	if (i_as.state == VMCS_AS_SHUTDOWN)
	    enter_kdebug("checks missing shutdown");
	if (i_as.state == VMCS_AS_WF_IPI)
	    enter_kdebug("checks missing wait-for-ipi");
    }

    // Interuptibility State.	(22.3.1.5)
    ASSERT((i_ias.raw & BITS_31_04) == 0);
    ASSERT( !((i_ias.bl_sti == 1) && (i_ias.bl_movss == 1)));
    if ((i_rfl & X86_FLAGS_IF) == 0)
	ASSERT(i_ias.bl_sti == 0);
    if ((i_iif.valid == 1) & (i_iif.type == VMCS_INT_EXT_INT))
    {
	ASSERT(i_ias.bl_sti == 0);
	ASSERT(i_ias.bl_movss == 0);
    }
    if (i_entryctr.entry_smm == 1)
	ASSERT(i_ias.bl_smi == 1);
    else
	ASSERT(i_ias.bl_smi == 0);
    if ((i_iif.valid == 1) &&
	(i_iif.type == 2))
	ASSERT(i_ias.bl_sti == 0);

    // Pending debug exceptions.
    i_pend_dbg_except = vmcs_gs_get_pend_dbg_except (self);
    ASSERT((i_pend_dbg_except.raw & (BITS_11_04 | (1UL<<13) | BITS_63_15)) == 0);
    if ((i_ias.bl_sti == 1) ||
	(i_ias.bl_movss == 1) ||
	(i_as.state == VMCS_AS_HLT))
    {
	// To tight checks.
	u64_t debugctl = vmcs_gs_get_dbg_ctl (self);

	if (((i_rfl & X86_FLAGS_TF) != 0) && ((debugctl & 1) == 0))
	    ASSERT(i_pend_dbg_except.bs == 1);

	if (((i_rfl & X86_FLAGS_TF) == 0) || ((debugctl & 1) == 1))
	    ASSERT(i_pend_dbg_except.bs == 0);
    }

    // VMCS Link Pointer.		(22.3.1.5)
    ASSERT(vmcs_gs_get_linkptr (self) == (-1ULL));
}


void vmcs_hsarea_do_vmentry_checks (vmcs_t *self, vmcs_entryctr_t i_entryctr, vmcs_exitctr_t i_exitctr)
{
    // CRs		(22.2.2)
    word_t i_cr0, i_cr4;
    i_cr0 = vmcs_hs_get_cr0 (self);
    i_cr4 = vmcs_hs_get_cr4 (self);

    ASSERT(x86_x32_vmx_check_fixed_bits_cr0 (i_cr0));
    ASSERT(x86_x32_vmx_check_fixed_bits_cr4 (i_cr4));

    if (i_exitctr.host_as_sz == 1)
	ASSERT((i_cr4 & X86_CR4_PAE) == 1);

    // Checks Related to Address Space Size.
    if (i_exitctr.host_as_sz == 0)
    {
	u64_t i_rip;

	ASSERT(i_entryctr.ia32e_mode == 0);

	i_rip = vmcs_hs_get_rip (self);
	ASSERT((i_rip & (BITS_63_32)) == 0);
    }
}

#endif /* !defined(CONFIG_DEBUG) */
