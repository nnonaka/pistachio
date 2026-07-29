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
#define X86_CTRLXFER_FAULT_MAX          vmcs_ei_reason_t::be_max
#else
#define X86_CTRLXFER_FLAGMASK		(word_t) (X86_USER_FLAGMASK)
#define X86_CTRLXFER_FAULT_MAX          0
#endif


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
typedef bitmask_t<u32_t> ctrlxfer_mask_t;
/*
 * NOT CONVERTED.  These were arch_ktcb_t methods and static tables; their
 * definitions are in x32/thread.c, likewise unconverted.  No configuration in
 * contrib/configs sets CONFIG_X_CTRLXFER_MSG, so none of this is compiled and
 * none of it has been compiled at any point in this migration -- converting it
 * blind is how the gate-blind deletions of §95, §116 and §123 happened.  It
 * needs a configuration that turns the option on to convert against.
 */
#error CONFIG_X_CTRLXFER_MSG: x32 ctrlxfer is not converted (see x32/ktcb.h)
#endif /* defined(CONFIG_X_CTRLXFER_MSG) */

#endif /* !__GLUE_V4_X86__X32__KTCB_H__ */
