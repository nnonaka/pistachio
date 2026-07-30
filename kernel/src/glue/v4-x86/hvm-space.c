/*********************************************************************
 *
 * Copyright (C) 2007,  Karlsruhe University
 *
 * File path:     glue/v4-ia32/hvm/ctrl.cc
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

#include INC_API(tcb.h)
#include INC_API(space.h)
#include INC_API(queueing.h)
#include INC_GLUE(hvm-space.h)
#include INC_GLUE(hvm.h)

/*
 * Was the x86_hvm_space_t methods.  tcb_t::get_arch() returned &arch, the
 * arch_ktcb_t; the HVM state is its `hvm' member, so the receiver of the
 * arch_hvm_ktcb_* calls is &tcb->arch.hvm.
 */

INLINE void x86_hvm_space_set_hvm_mode (x86_hvm_space_t *self, tcb_t *tcb, space_t *space)
{
    (void) self;
    if (space_is_hvm_space (space))
    {
	if (!resource_bits_have_resource (&tcb->resource_bits, HVM))
	{
	    if (arch_hvm_ktcb_enable_hvm (&tcb->arch.hvm))
		resource_bits_add (&tcb->resource_bits, HVM);
	}
    }
    else
    {
	if (resource_bits_have_resource (&tcb->resource_bits, HVM))
	{
	    resource_bits_remove (&tcb->resource_bits, HVM);
	    arch_hvm_ktcb_disable_hvm (&tcb->arch.hvm);
	}
    }
}


bool x86_hvm_space_activate (x86_hvm_space_t *self, space_t *space)
{
    tcb_t *last_tcb;
    tcb_t *tcb;

    if (self->active)
	return true;
#if defined(CONFIG_IO_FLEXPAGES)
    /*
     * If we use the IOPBM we allocate it early to avoid synchronization
     * of all VMCS on a lazy allocation of the IOPBM.
     *
     * NEVER COMPILED: the option is spelled CONFIG_X86_IO_FLEXPAGES everywhere
     * else in the tree, so this branch is unreachable, and create_io_bitmap has
     * no definition anywhere -- upstream included.  Translated by inspection;
     * see doc/notes/cpp-to-c-migration.md §134.
     */
    if (!space_create_io_bitmap (space))
	return false;
#endif
    self->active = true;

    /* Loop through existing TCBs pointing to space, and activate HVM */
    last_tcb = NULL;
    for (tcb = self->tcb_list; tcb != last_tcb; tcb = tcb->arch.hvm.space_list.prev)
    {
	ASSERT (tcb_exists (tcb));
	ASSERT (tcb_get_space (tcb) == space);
	x86_hvm_space_set_hvm_mode (self, tcb, tcb_get_space (tcb));
	last_tcb = tcb;
    }
    return true;
}


void x86_hvm_space_handle_gphys_unmap (x86_hvm_space_t *self, addr_t g_paddr, word_t log2size)
{
    tcb_t *last_tcb;
    tcb_t *tcb;

    (void) g_paddr; (void) log2size;

    if (!self->active)
	return;
    /*
     * Loop through all TCBs (VCPUs) using the space,
     * and remove the entries from their VTLBs.
     */
    last_tcb = NULL;
    for (tcb = self->tcb_list; tcb != last_tcb; tcb = tcb->arch.hvm.space_list.prev)
    {
	ASSERT (tcb_exists (tcb));
        ASSERT (arch_hvm_ktcb_is_hvm_enabled (&tcb->arch.hvm));
        // Currently, just flush the VTLB completely.
	last_tcb = tcb;
    }
}


bool x86_hvm_space_lookup_gphys_addr (x86_hvm_space_t *self, addr_t gvaddr, addr_t *gpaddr)
{
    tcb_t *tcb;

    if (!self->active)
	return false;

    tcb = self->tcb_list;

    if (!tcb || !tcb_exists (tcb) || !arch_hvm_ktcb_is_hvm_enabled (&tcb->arch.hvm))
	return false;

    return x86_hvm_vtlb_lookup_gphys_addr (&tcb->arch.hvm.vtlb, gvaddr, gpaddr);
}


void x86_hvm_space_enqueue_tcb (x86_hvm_space_t *self, tcb_t *tcb, space_t *space)
{
    ENQUEUE_LIST_HEAD (self->tcb_list, tcb, arch.hvm.space_list);
    x86_hvm_space_set_hvm_mode (self, tcb, space);
}

void x86_hvm_space_dequeue_tcb (x86_hvm_space_t *self, tcb_t *tcb, space_t *space)
{
    (void) space;
    DEQUEUE_LIST (self->tcb_list, tcb, arch.hvm.space_list);
}
