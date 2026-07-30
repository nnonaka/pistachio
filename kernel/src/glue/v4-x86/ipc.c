/*********************************************************************
 *
 * Copyright (C) 2006-2007,  Karlsruhe University
 *
 * File path:     glue/v4-ia32/x32/hvm.cc
 * Description:   Full Virtualization Extensions
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
#include INC_ARCH(traps.h)

#include INC_API(tcb.h)
#include INC_API(ipc.h)

#if defined(CONFIG_X_X86_HVM)
#include INC_GLUE(hvm.h)

static msg_tag_t x86_hvm_fault_tag(word_t fault_id, bool internal)
{
    msg_tag_t tag;
    msg_tag_set (&tag, 0, 3, -((9UL + fault_id) << 4) |
		 (internal  ? 1 << 3 : 0));
    return tag;
}


#endif

#include <debug.h>
#include <kdb/tracepoints.h>


/**********************************************************************
 *
 *                        ctrlxfer arrays
 *
 * Were the static members ctrlxfer_item_t::num_hwregs and
 * ctrlxfer_item_t::hwregs; api/v4/ipc.h declares them as the file-scope arrays
 * ctrlxfer_num_hwregs / ctrlxfer_hwregs, as on powerpc.
 *
 **********************************************************************/
const word_t ctrlxfer_num_hwregs[id_max] = 
{
    10 /* GPRegs */, 
    0, /* FPURegs */
#if defined(CONFIG_X_X86_HVM)
    X86_HVM_NUM_CREGS, 
    X86_HVM_NUM_DREGS, 
    X86_HVM_NUM_CSREGS,
    X86_HVM_NUM_SSREGS,
    X86_HVM_NUM_DSREGS,
    X86_HVM_NUM_ESREGS,
    X86_HVM_NUM_FSREGS,
    X86_HVM_NUM_GSREGS,
    X86_HVM_NUM_TRREGS,
    X86_HVM_NUM_LDTRREGS,
    X86_HVM_NUM_IDTRREGS,
    X86_HVM_NUM_GDTRREGS,
    X86_HVM_NUM_NONREGEXC, 
    X86_HVM_NUM_EXECCTRL, 
    X86_HVM_NUM_OTHERREGS, 
#endif
};

static const word_t  x86_gpregs[]       =  {  X86_EXC_IPREG  /* EIP */, X86_EXC_FREG   /* EFL */,
                                              X86_EXC_DIREG  /* EDI */, X86_EXC_SIREG  /* ESI */,  
                                              X86_EXC_BPREG  /* EBP */, X86_EXC_SPREG  /* ESP */,  
                                              X86_EXC_BREG   /* EBX */, X86_EXC_DREG   /* EDX */, 
                                              X86_EXC_CREG   /* ECX */, X86_EXC_AREG   /* EAX */ };
#if defined(CONFIG_X_X86_HVM)
static const word_t  x86_hvm_dregs[]	 = { 0 /* DR0 */, 1 /* DR1 */, 2 /* DR3 */, 3 /* DR3 */, 6 /* DR6 */, 7 /* DR7 */ };
static const word_t  x86_hvm_csregs[]	 = { VMCS_IDX_G_CS_SEL, VMCS_IDX_G_CS_BASE, VMCS_IDX_G_CS_LIMIT, VMCS_IDX_G_CS_ATTR };
static const word_t  x86_hvm_ssregs[]	 = { VMCS_IDX_G_SS_SEL, VMCS_IDX_G_SS_BASE, VMCS_IDX_G_SS_LIMIT, VMCS_IDX_G_SS_ATTR };
static const word_t  x86_hvm_dsregs[]	 = { VMCS_IDX_G_DS_SEL, VMCS_IDX_G_DS_BASE, VMCS_IDX_G_DS_LIMIT, VMCS_IDX_G_DS_ATTR };
static const word_t  x86_hvm_esregs[] 	 = { VMCS_IDX_G_ES_SEL, VMCS_IDX_G_ES_BASE, VMCS_IDX_G_ES_LIMIT, VMCS_IDX_G_ES_ATTR };
static const word_t  x86_hvm_fsregs[]	 = { VMCS_IDX_G_FS_SEL, VMCS_IDX_G_FS_BASE, VMCS_IDX_G_FS_LIMIT, VMCS_IDX_G_FS_ATTR };
static const word_t  x86_hvm_gsregs[]	 = { VMCS_IDX_G_GS_SEL, VMCS_IDX_G_GS_BASE, VMCS_IDX_G_GS_LIMIT, VMCS_IDX_G_GS_ATTR };
static const word_t  arch_hvm_ktcb_trregs[]	 = { VMCS_IDX_G_TR_SEL, VMCS_IDX_G_TR_BASE, VMCS_IDX_G_TR_LIMIT, VMCS_IDX_G_TR_ATTR };
static const word_t  x86_hvm_ldtrregs[]	 = { VMCS_IDX_G_LDTR_SEL, VMCS_IDX_G_LDTR_BASE, VMCS_IDX_G_LDTR_LIMIT, VMCS_IDX_G_LDTR_ATTR };
static const word_t  x86_hvm_idtrregs[]	 = { VMCS_IDX_G_IDTR_BASE, VMCS_IDX_G_IDTR_LIMIT };
static const word_t  x86_hvm_gdtrregs[]	 = { VMCS_IDX_G_GDTR_BASE, VMCS_IDX_G_GDTR_LIMIT };
#endif

const word_t *const ctrlxfer_hwregs[id_max] = 
{
    x86_gpregs,
    NULL, /* fpu regs software-emulated */ 
#if defined(CONFIG_X_X86_HVM)
    NULL, /* cr regs software-emulated */ 
    x86_hvm_dregs,
    x86_hvm_csregs,
    x86_hvm_ssregs,
    x86_hvm_dsregs,
    x86_hvm_esregs,
    x86_hvm_fsregs,
    x86_hvm_gsregs,
    arch_hvm_ktcb_trregs,
    x86_hvm_ldtrregs,
    x86_hvm_idtrregs,
    x86_hvm_gdtrregs,
    NULL, /* nonreg/exc state software-emulated */ 
    NULL, /* execctrl   state software-emulated */ 
    NULL /* otherregs        software-emulated */ 
#endif 
};



/**********************************************************************
 *
 *                        hvm-specific ipc handling
 *
 **********************************************************************/

#if defined(CONFIG_X_X86_HVM)
EXTERN_TRACEPOINT(X86_HVM_EXIT);

void arch_hvm_ktcb_send_hvm_fault (arch_hvm_ktcb_t *self, word_t fault_id, word_t qual,
				   word_t ilen, word_t ia_info, bool internal)
{
    tcb_t *tcb;
    msg_tag_t tag;
    threadid_t to, from;
    acceptor_t acceptor;
    
    TRACEPOINT (X86_HVM_EXIT, "x86-hvm: hvm fault id %d qual %d ilen %d",  fault_id, qual, ilen);

    tcb = addr_to_tcb (self);
    tag = x86_hvm_fault_tag(fault_id, internal);
    to   = tcb_get_pager (tcb);
    from = to;

    tag.x.typed = tcb_append_ctrlxfer_item (tcb, tag, 4) & 0x3f;
    tcb_set_tag (tcb, tag);
    tcb_set_mr (tcb, 1, qual);
    tcb_set_mr (tcb, 2, ilen);
    tcb_set_mr (tcb, 3, ia_info);

    acceptor.raw = 0;
    acceptor.x.ctrlxfer = 1;
    tcb_set_br (tcb, 0, acceptor.raw);

    // Send the virtualization message to the pager
    // and do a closed wait for the reply.
    tag = tcb_do_ipc (tcb, to, from, timeout_never ());

    if (msg_tag_is_error (&tag))
    {
	printf("result tag = %p, qual = %p, errcode = %p\n", 
	       tag.raw, qual, tcb_get_error_code (tcb));
	enter_kdebug("hvm IPC error");
	tcb_set_tag (tcb, tag);
    }
}

#endif /* defined(CONFIG_X_X86_HVM) */

