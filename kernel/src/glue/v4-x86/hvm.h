/*********************************************************************
 *                
 * Copyright (C) 2008-2009,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/hvm.h
 * Description:   
 *                
 * @LICENSE@
 *                
 * $Id:$
 *                
 ********************************************************************/
#ifndef __GLUE__V4_X86__HVM_H__
#define __GLUE__V4_X86__HVM_H__

#include INC_GLUE_SA(hvm-vmx.h)

/*
 * Was class arch_hvm_ktcb_t : public x86_svmx_hvm_t.  The state of both is one
 * struct now, declared in the subarchitecture's hvm-vmx.h because that is where
 * the base's members live; what remains here is the arch-neutral half of the
 * interface, as free functions taking the receiver first and turning the
 * word_t& out-parameters into pointers (the get/set_ctrlxfer_regs tables in
 * api/v4/tcb.h describe exactly that signature).
 */

/* Initialize/finalize. */
INLINE bool arch_hvm_ktcb_is_hvm_enabled (arch_hvm_ktcb_t *self)
{ return self->hvm_enabled; }
bool arch_hvm_ktcb_enable_hvm (arch_hvm_ktcb_t *self);
void arch_hvm_ktcb_disable_hvm (arch_hvm_ktcb_t *self);

/* Send a virtualization IPC, wait for reply. */
void arch_hvm_ktcb_send_hvm_fault (arch_hvm_ktcb_t *self, word_t id, word_t qual,
				   word_t insn_length, word_t addr_instr_info, bool internal);

/* Manage guest-physical memory. */
INLINE void arch_hvm_ktcb_handle_gphys_unmap (arch_hvm_ktcb_t *self, addr_t addr, word_t log2size)
{
    (void) addr; (void) log2size;
    x86_hvm_vtlb_flush_gphys (&self->vtlb);
}


/* Handle initial virtualization fault reply. */
NORETURN void arch_hvm_ktcb_enter_hvm_loop (arch_hvm_ktcb_t *self);

/* Handle VM exit. */
void arch_hvm_ktcb_handle_hvm_exit (arch_hvm_ktcb_t *self);

/* Was protected. */
void arch_hvm_ktcb_resume_hvm (arch_hvm_ktcb_t *self);

/*
 * Get/set control registers.  These are the get_ctrlxfer_regs_t /
 * set_ctrlxfer_regs_t entries for the HVM item ids: the C++ tables in
 * x32/thread.cc held pointers to member of arch_ktcb_t, which is what those
 * typedefs describe, so these take the arch_ktcb_t and reach its `hvm' member.
 * The rest of the interface above takes the arch_hvm_ktcb_t directly.
 */
struct arch_ktcb_t;
typedef struct arch_ktcb_t arch_ktcb_t;

word_t arch_ktcb_get_x86_hvm_cregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr);
word_t arch_ktcb_set_x86_hvm_cregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *src, word_t *src_mr);

word_t arch_ktcb_get_x86_hvm_dregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr);
word_t arch_ktcb_set_x86_hvm_dregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *src, word_t *src_mr);

word_t arch_ktcb_get_x86_hvm_segregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr);
word_t arch_ktcb_set_x86_hvm_segregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *src, word_t *src_mr);

word_t arch_ktcb_get_x86_hvm_nonregexc (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr);
word_t arch_ktcb_set_x86_hvm_nonregexc (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *src, word_t *src_mr);

word_t arch_ktcb_get_x86_hvm_execctrl (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr);
word_t arch_ktcb_set_x86_hvm_execctrl (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *src, word_t *src_mr);

word_t arch_ktcb_get_x86_hvm_otherregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr);
word_t arch_ktcb_set_x86_hvm_otherregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *src, word_t *src_mr);


#if defined(CONFIG_DEBUG)
word_t arch_ktcb_get_x86_hvm_ctrlxfer_reg (arch_ktcb_t *self, word_t id, word_t reg);
void arch_hvm_ktcb_dump_hvm (arch_hvm_ktcb_t *self);
INLINE void arch_hvm_ktcb_dump_hvm_ptab_entry (arch_hvm_ktcb_t *self, addr_t vaddr)
{ x86_hvm_vtlb_dump_ptab_entry (&self->vtlb, vaddr); }
#endif   


#endif /* !__GLUE__V4_X86__HVM_H__ */
