/*********************************************************************
 *                
 * Copyright (C) 2002, 2004, 2008-2010,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/x32/ktcb.h
 * Description:   
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
 * $Id: ktcb.h,v 1.4 2004/03/22 18:07:51 ud3 Exp $
 *                
 ********************************************************************/
#ifndef __GLUE_V4_X86__X32__KTCB_H__
#define __GLUE_V4_X86__X32__KTCB_H__

#include <tcb_layout.h>

#include INC_API(types.h)
#include INC_API(tcb.h)
#include INC_API_SCHED(ktcb.h)

#if defined(CONFIG_X_CTRLXFER_MSG)
#include INC_GLUE(ipc.h)
#endif

#if defined(CONFIG_X_X86_HVM)
#include INC_GLUE(hvm.h)
#define X86_CTRLXFER_FLAGMASK		(hvm_enabled ? (word_t) X86_HVM_EFLAGS_MASK : (word_t)  X86_USER_FLAGMASK)
#define X86_CTRLXFER_FAULT_MAX          VMCS_EI_REASON_BE_MAX
#else
#define X86_CTRLXFER_FLAGMASK		(word_t) (X86_USER_FLAGMASK)
#define X86_CTRLXFER_FAULT_MAX          0
#endif

/* api/v4/tcb.h sizes tcb_t::fault_ctrlxfer by this name; it was
   arch_ktcb_t::fault_max, a static const member the glue code read through the
   class.  Same spelling as glue/v4-powerpc/ktcb.h. */
#define ARCH_KTCB_FAULT_MAX		X86_CTRLXFER_FAULT_MAX


struct arch_ktcb_t {
    /* Like x64, x32 carries no arch-specific ktcb state in any configuration
       this tree builds -- the CONFIG_X_CTRLXFER_MSG members below were static
       and the HVM base class empty.  arch_ktcb_t is a by-value member of
       tcb_t and an empty struct is a GNU C extension of size 0, which would
       shift every field after `arch'; the explicit byte pins the size at 1,
       matching the layout tcb_layout.h is generated against. */
    char __empty;
};
typedef struct arch_ktcb_t arch_ktcb_t;

#if defined(CONFIG_X_CTRLXFER_MSG)
typedef bitmask_u32_t ctrlxfer_mask_t;

/*
 * Were arch_ktcb_t methods and two static member tables.  The methods take the
 * receiver first; the word_t& out-parameters become pointers, which is what the
 * get_ctrlxfer_regs_t / set_ctrlxfer_regs_t typedefs in api/v4/tcb.h already
 * describe.  The tables are plain file-scope arrays, as on powerpc, and are
 * indexed directly by api/v4/thread.c rather than through the class.
 */
word_t arch_ktcb_get_x86_gpregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr);
word_t arch_ktcb_set_x86_gpregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *src, word_t *src_mr);
word_t arch_ktcb_get_x86_fpuregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *dst, word_t *dst_mr);
word_t arch_ktcb_set_x86_fpuregs (arch_ktcb_t *self, word_t id, word_t mask, tcb_t *src, word_t *src_mr);

#if defined(CONFIG_DEBUG)
word_t arch_ktcb_get_ctrlxfer_reg (arch_ktcb_t *self, word_t id, word_t reg);
#endif

extern get_ctrlxfer_regs_t get_ctrlxfer_regs[id_max];
extern set_ctrlxfer_regs_t set_ctrlxfer_regs[id_max];

/* Was tcb_t::append_ctrlxfer_item, an INLINE in x32/tcb.h.  It is out of line
   in glue/v4-x86/thread.c beside tcb_set_fault_ctrlxfer_items and
   tcb_get_fault_ctrlxfer_items, which api/v4/tcb.h declares extern.  Declared
   here rather than there because powerpc keeps its own copy INLINE. */
word_t tcb_append_ctrlxfer_item (tcb_t *self, msg_tag_t tag, word_t offset);
#endif /* defined(CONFIG_X_CTRLXFER_MSG) */

#endif /* !__GLUE_V4_X86__X32__KTCB_H__ */
