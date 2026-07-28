/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     glue/v4-powerpc/resource_functions.h
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
 * $Id$
 *                
 ********************************************************************/
#ifndef __GLUE__V4_POWERPC__RESOURCE_FUNCTIONS_H__
#define __GLUE__V4_POWERPC__RESOURCE_FUNCTIONS_H__
#include INC_API(resources.h)

INLINE void tcb_resources_fpu_unavail_exception (thread_resources_t *self, tcb_t *tcb)
{
    tcb_t *fp_tcb = get_fp_lazy_tcb();

    /* In our lazy floating point model, we should never see a floating point
     * exception if the current tcb already owns the floating point register
     * file.
     */
    ASSERT( fp_tcb != tcb );

    if( fp_tcb )
	tcb_resources_spill_fpu (&fp_tcb->resources, fp_tcb);
    tcb_resources_restore_fpu (self, tcb);
}

INLINE addr_t tcb_resources_copy_area_real_address (thread_resources_t *self, tcb_t *src, addr_t addr)
{
    return addr_offset(addr, self->copy_area_offset - COPY_AREA_START);
}

INLINE void tcb_resources_setup_copy_area (thread_resources_t *self, tcb_t *src, addr_t *saddr,
					   tcb_t *dst, addr_t *daddr)
{
    self->copy_area_offset = (word_t)*daddr & ~(COPY_AREA_SIZE - 1);
    *daddr = addr_offset((addr_t)COPY_AREA_START, (word_t)*daddr & (COPY_AREA_SIZE - 1));
    //TRACEF("copy area: offset: %08x, dst: %p\n", self->copy_area_offset, *daddr);
    resource_bits_add (&src->resource_bits, COPY_AREA);
}

#ifdef CONFIG_PPC_MMU_SEGMENTS
INLINE void tcb_resources_enable_copy_area (thread_resources_t *self, tcb_t *src)
{
    ppc_resource_bits_t *bits = (ppc_resource_bits_t *)&src->resource_bits;

    threadid_t partner_tid = tcb_get_partner (src);
    tcb_t *partner = tcb_get_tcb (partner_tid);
    ppc_segment_t partner_seg = space_get_segment_id (partner->space);

    // Change the copy area segment register to point into the target space.
#warning VU: copy area code is inorrect for tunnelled PFs
    isync();
    ppc_set_sr( COPY_AREA_SEGMENT, 
		partner_seg.raw | bits->get_copy_area_dst_seg() );
    isync();
}
INLINE void tcb_resources_flush_copy_area (thread_resources_t *self, tcb_t *tcb) { }

#elif defined(CONFIG_PPC_MMU_TLB)
INLINE void tcb_resources_enable_copy_area (thread_resources_t *self, tcb_t *src) { }
INLINE void tcb_resources_flush_copy_area (thread_resources_t *self, tcb_t *tcb)
{
    /* tcb_get_space is defined in api/v4/tcb.h after this header is
       reached, so use the member directly. */
    space_t *space = tcb->space;
    space_flush_tlb_range (space, space, (addr_t)COPY_AREA_START, (addr_t)COPY_AREA_END);
}
#endif

INLINE void tcb_resources_disable_copy_area (thread_resources_t *self, tcb_t *tcb)
{
    if (resource_bits_have_resource (&tcb->resource_bits, COPY_AREA))
    {
	//TRACEF("disable copy area\n");
	tcb_resources_flush_copy_area (self, tcb);
	resource_bits_remove (&tcb->resource_bits, COPY_AREA);
    }
}

INLINE void tcb_resources_set_kernel_ipc (thread_resources_t *self, tcb_t *tcb)
{
    resource_bits_add (&tcb->resource_bits, KERNEL_IPC);
}

INLINE void tcb_resources_clr_kernel_ipc (thread_resources_t *self, tcb_t *tcb)
{
    resource_bits_remove (&tcb->resource_bits, KERNEL_IPC);
}

INLINE void tcb_resources_set_kernel_thread (thread_resources_t *self, tcb_t *tcb)
{
    resource_bits_add (&tcb->resource_bits, KERNEL_THREAD);
}


#endif /* !__GLUE__V4_POWERPC__RESOURCE_FUNCTIONS_H__ */
