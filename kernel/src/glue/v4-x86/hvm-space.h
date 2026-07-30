/*********************************************************************
 *
 * Copyright (C) 2007,  Karlsruhe University
 *
 * File path:     glue/v4-ia32/hvm-space.h
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
#ifndef __GLUE__V4_X86__HVM_SPACE_H__
#define __GLUE__V4_X86__HVM_SPACE_H__


struct tcb_t;
typedef struct tcb_t tcb_t;
struct space_t;

/* Was class x86_hvm_space_t; the kdb_t friend declaration went with the class. */
struct x86_hvm_space_t
{
    bool         active;
    struct tcb_t *tcb_list;
};
typedef struct x86_hvm_space_t x86_hvm_space_t;

/* Activate virtualization for this space. */
INLINE bool x86_hvm_space_is_active (x86_hvm_space_t *self) { return self->active; }
bool x86_hvm_space_activate (x86_hvm_space_t *self, struct space_t *space);

/* Remember attached TCBs. */
void x86_hvm_space_enqueue_tcb (x86_hvm_space_t *self, struct tcb_t *tcb, struct space_t *space);
void x86_hvm_space_dequeue_tcb (x86_hvm_space_t *self, struct tcb_t *tcb, struct space_t *space);

/* Handle unmapping on all attached VCPUs. */
void x86_hvm_space_handle_gphys_unmap (x86_hvm_space_t *self, addr_t g_paddr, word_t log2size);

/* Lookup a mapping in a VTLB. */
bool x86_hvm_space_lookup_gphys_addr (x86_hvm_space_t *self, addr_t gvaddr, addr_t *gpaddr);

#if defined(CONFIG_DEBUG)
INLINE struct tcb_t * x86_hvm_space_get_tcb_list (x86_hvm_space_t *self) { return self->tcb_list; }
#endif


#endif /* !__GLUE__V4_X86__HVM_SPACE_H__ */
