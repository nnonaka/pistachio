/*********************************************************************
 *                
 * Copyright (C) 1999-2011,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     glue/v4-powerpc/resources.cc
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

#include INC_API(tcb.h)
#include INC_GLUE(memcfg.h)
#include INC_ARCH(ppc44x.h)

// TODO: ensure that this is initialized to NULL for each cpu, perhaps via 
// ctors.
tcb_t *_fp_lazy_tcb UNIT("cpulocal");

void tcb_resources_deactivate_fpu (thread_resources_t *self, tcb_t *tcb)
{
    set_fp_lazy_tcb( NULL );
    // Disable fpu access for the tcb.
    except_regs_t *regs = get_user_except_regs(tcb);
    regs->srr1_flags = MSR_CLR( regs->srr1_flags, MSR_FP );
}

void tcb_resources_activate_fpu (thread_resources_t *self, tcb_t *tcb)
{
    set_fp_lazy_tcb( tcb );
    // Enable fpu access for the tcb.
    except_regs_t *regs = get_user_except_regs(tcb);
    regs->srr1_flags = MSR_SET( regs->srr1_flags, MSR_FP );
}

void tcb_resources_reown_fpu (thread_resources_t *self, tcb_t *tcb, tcb_t *new_owner)
{
    tcb_resources_deactivate_fpu (self, tcb);
    tcb_resources_activate_fpu (&new_owner->resources, new_owner);
}

#ifdef CONFIG_X_PPC_SOFTHVM
#include INC_ARCH(softhvm.h)
tcb_t *thread_resources_last_hvm_tcb;

void tcb_resources_enable_hvm_mode (thread_resources_t *self, tcb_t *tcb)
{
    //TRACEF("Enable HVM mode (%p)\n", tcb);
    ppc_set_spr(SPR_IVPR, ((word_t)memcfg_start_except() & 0xffff0000) + EXCEPT_HVM_OFFSET);
    ppc_set_pid (ppc_softhvm_get_pid_for_msr ((&tcb->arch)->vm));
    ppc_softhvm_load_guest_sprs ((&tcb->arch)->vm);
}

void tcb_resources_disable_hvm_mode (thread_resources_t *self, tcb_t *tcb)
{
    //TRACEF("Disable HVM mode (%p)\n", get_current_tcb());
    ppc_set_spr(SPR_IVPR, (word_t)memcfg_start_except() & 0xffff0000);
}
#endif

void tcb_resources_dump (thread_resources_t *self, tcb_t * tcb)
{
    if (resource_bits_have_resource (&tcb->resource_bits, COPY_AREA))
	printf("copy ");
    if (resource_bits_have_resource (&tcb->resource_bits, KERNEL_IPC))
	printf("kipc ");
    if (resource_bits_have_resource (&tcb->resource_bits, KERNEL_THREAD))
	printf("kthread ");
    if (resource_bits_have_resource (&tcb->resource_bits, FPU))
	printf("fpu ");
    if (resource_bits_have_resource (&tcb->resource_bits, SOFTHVM))
	printf("hvm ");
}

void tcb_resources_save (thread_resources_t *self, tcb_t *tcb)
{
    if (resource_bits_have_resource (&tcb->resource_bits, COPY_AREA))
	tcb_resources_flush_copy_area (self, tcb);
#ifdef CONFIG_X_PPC_SOFTHVM
    if (resource_bits_have_resource (&tcb->resource_bits, SOFTHVM)) {
	tcb_resources_disable_hvm_mode (self, tcb);
        thread_resources_last_hvm_tcb = tcb;
    }
#endif
}

void tcb_resources_load (thread_resources_t *self, tcb_t *tcb)
{
    if (resource_bits_have_resource (&tcb->resource_bits, COPY_AREA))
	tcb_resources_enable_copy_area (self, tcb);
#ifdef CONFIG_X_PPC_SOFTHVM
    if (resource_bits_have_resource (&tcb->resource_bits, SOFTHVM)) {
	tcb_resources_enable_hvm_mode (self, tcb);
        space_t *space = tcb_get_space (tcb);
        if (thread_resources_last_hvm_tcb && space != tcb_get_space (thread_resources_last_hvm_tcb))
        {
            //flush hvm tlb entries
            //printf("hvm space switch %t %p -> %t %p, flushing tlb\n",
            //     thread_resources_last_hvm_tcb, tcb_get_space (thread_resources_last_hvm_tcb), tcb, space);
            space_flush_tlb_hvm (space, space, 0, ~0U);
        }
    }
#endif
}

void tcb_resources_purge (thread_resources_t *self, tcb_t *tcb)
{
    if( get_fp_lazy_tcb() == tcb )
	tcb_resources_spill_fpu (self,  tcb );
}

void tcb_resources_init (thread_resources_t *self, tcb_t *tcb)
{
    resource_bits_init (&tcb->resource_bits);
    self->fpscr = 0;	// TODO: seed with an appropriate value!
}

void tcb_resources_free (thread_resources_t *self, tcb_t *tcb)
{
    if( get_fp_lazy_tcb() == tcb )
	tcb_resources_deactivate_fpu (self,  tcb );
}

void tcb_resources_spill_fpu (thread_resources_t *self, tcb_t *tcb)
{
    // Spill the registers.
    u64_t *start = self->fpu_state;
#ifdef CONFIG_SUBPLAT_440_BGP
    ASSERT(((word_t)(start) & 0xf) == 0);
    asm volatile (
	"stfpdx	  0, 0, %[dest]\n"
	"stfpdux  1, %[dest], %[offset]\n"
	"stfpdux  2, %[dest], %[offset]\n"
	"stfpdux  3, %[dest], %[offset]\n"
	"stfpdux  4, %[dest], %[offset]\n"
	"stfpdux  5, %[dest], %[offset]\n"
	"stfpdux  6, %[dest], %[offset]\n"
	"stfpdux  7, %[dest], %[offset]\n"
	"stfpdux  8, %[dest], %[offset]\n"
	"stfpdux  9, %[dest], %[offset]\n"
	"stfpdux 10, %[dest], %[offset]\n"
	"stfpdux 11, %[dest], %[offset]\n"
	"stfpdux 12, %[dest], %[offset]\n"
	"stfpdux 13, %[dest], %[offset]\n"
	"stfpdux 14, %[dest], %[offset]\n"
	"stfpdux 15, %[dest], %[offset]\n"
	"stfpdux 16, %[dest], %[offset]\n"
	"stfpdux 17, %[dest], %[offset]\n"
	"stfpdux 18, %[dest], %[offset]\n"
	"stfpdux 19, %[dest], %[offset]\n"
	"stfpdux 20, %[dest], %[offset]\n"
	"stfpdux 21, %[dest], %[offset]\n"
	"stfpdux 22, %[dest], %[offset]\n"
	"stfpdux 23, %[dest], %[offset]\n"
	"stfpdux 24, %[dest], %[offset]\n"
	"stfpdux 25, %[dest], %[offset]\n"
	"stfpdux 26, %[dest], %[offset]\n"
	"stfpdux 27, %[dest], %[offset]\n"
	"stfpdux 28, %[dest], %[offset]\n"
	"stfpdux 29, %[dest], %[offset]\n"
	"stfpdux 30, %[dest], %[offset]\n"
	"stfpdux 31, %[dest], %[offset]\n"
	: [dest] "+b" (start)
	: [offset] "b"(16)
	);
#else
    asm volatile (
	    "stfd %%f0,    0(%0) ;"
	    "stfd %%f1,    8(%0) ;"
	    "stfd %%f2,   16(%0) ;"
	    "stfd %%f3,   24(%0) ;"
	    "stfd %%f4,   32(%0) ;"
	    "stfd %%f5,   40(%0) ;"
	    "stfd %%f6,   48(%0) ;"
	    "stfd %%f7,   56(%0) ;"
	    "stfd %%f8,   64(%0) ;"
	    "stfd %%f9,   72(%0) ;"
	    "stfd %%f10,  80(%0) ;"
	    "stfd %%f11,  88(%0) ;"
	    "stfd %%f12,  96(%0) ;"
	    "stfd %%f13, 104(%0) ;"
	    "stfd %%f14, 112(%0) ;"
	    "stfd %%f15, 120(%0) ;"
	    "stfd %%f16, 128(%0) ;"
	    "stfd %%f17, 136(%0) ;"
	    "stfd %%f18, 144(%0) ;"
	    "stfd %%f19, 152(%0) ;"
	    "stfd %%f20, 160(%0) ;"
	    "stfd %%f21, 168(%0) ;"
	    "stfd %%f22, 176(%0) ;"
	    "stfd %%f23, 184(%0) ;"
	    "stfd %%f24, 192(%0) ;"
	    "stfd %%f25, 200(%0) ;"
	    "stfd %%f26, 208(%0) ;"
	    "stfd %%f27, 216(%0) ;"
	    "stfd %%f28, 224(%0) ;"
	    "stfd %%f29, 232(%0) ;"
	    "stfd %%f30, 240(%0) ;"
	    "stfd %%f31, 248(%0) ;"
	    : /* ouputs */
	    : /* inputs */
	      "b" (start)
	    );
#endif
    /* Spill the self->fpscr.  Temporarily store it to an 8-byte location,
     * so that we can store it as a double and avoid an fp-double to
     * fp-single conversion.  Then we move it to our 4-byte tcb location.
     */
    self->fpscr = ppc_get_fpscr();
    tcb_resources_deactivate_fpu (self,  tcb );
}

void tcb_resources_restore_fpu (thread_resources_t *self, tcb_t *tcb)
{
    /* Restore the self->fpscr.  We store it in the tcb as a 4-byte quantity,
     * but we need to load it as a floating point double to prevent
     * conversion from fp-single to fp-double.  So temporarily store
     * it in a 8-byte location.
     */
    ppc_set_fpscr(self->fpscr);

    // Load the registers.
    u64_t *start = self->fpu_state;
#ifdef CONFIG_SUBPLAT_440_BGP
    ASSERT(((word_t)(start) & 0xf) == 0);
    asm volatile (
	"lfpdx	 0, 0, %[src]\n"
	"lfpdux  1, %[src], %[offset]\n"
	"lfpdux  2, %[src], %[offset]\n"
	"lfpdux  3, %[src], %[offset]\n"
	"lfpdux  4, %[src], %[offset]\n"
	"lfpdux  5, %[src], %[offset]\n"
	"lfpdux  6, %[src], %[offset]\n"
	"lfpdux  7, %[src], %[offset]\n"
	"lfpdux  8, %[src], %[offset]\n"
	"lfpdux  9, %[src], %[offset]\n"
	"lfpdux 10, %[src], %[offset]\n"
	"lfpdux 11, %[src], %[offset]\n"
	"lfpdux 12, %[src], %[offset]\n"
	"lfpdux 13, %[src], %[offset]\n"
	"lfpdux 14, %[src], %[offset]\n"
	"lfpdux 15, %[src], %[offset]\n"
	"lfpdux 16, %[src], %[offset]\n"
	"lfpdux 17, %[src], %[offset]\n"
	"lfpdux 18, %[src], %[offset]\n"
	"lfpdux 19, %[src], %[offset]\n"
	"lfpdux 20, %[src], %[offset]\n"
	"lfpdux 21, %[src], %[offset]\n"
	"lfpdux 22, %[src], %[offset]\n"
	"lfpdux 23, %[src], %[offset]\n"
	"lfpdux 24, %[src], %[offset]\n"
	"lfpdux 25, %[src], %[offset]\n"
	"lfpdux 26, %[src], %[offset]\n"
	"lfpdux 27, %[src], %[offset]\n"
	"lfpdux 28, %[src], %[offset]\n"
	"lfpdux 29, %[src], %[offset]\n"
	"lfpdux 30, %[src], %[offset]\n"
	"lfpdux 31, %[src], %[offset]\n"
	: [src] "+b" (start)
	: [offset] "b"(16)
	);
#else
    asm volatile (
	    "lfd %%f0,    0(%0) ;"
	    "lfd %%f1,    8(%0) ;"
	    "lfd %%f2,   16(%0) ;"
	    "lfd %%f3,   24(%0) ;"
	    "lfd %%f4,   32(%0) ;"
	    "lfd %%f5,   40(%0) ;"
	    "lfd %%f6,   48(%0) ;"
	    "lfd %%f7,   56(%0) ;"
	    "lfd %%f8,   64(%0) ;"
	    "lfd %%f9,   72(%0) ;"
	    "lfd %%f10,  80(%0) ;"
	    "lfd %%f11,  88(%0) ;"
	    "lfd %%f12,  96(%0) ;"
	    "lfd %%f13, 104(%0) ;"
	    "lfd %%f14, 112(%0) ;"
	    "lfd %%f15, 120(%0) ;"
	    "lfd %%f16, 128(%0) ;"
	    "lfd %%f17, 136(%0) ;"
	    "lfd %%f18, 144(%0) ;"
	    "lfd %%f19, 152(%0) ;"
	    "lfd %%f20, 160(%0) ;"
	    "lfd %%f21, 168(%0) ;"
	    "lfd %%f22, 176(%0) ;"
	    "lfd %%f23, 184(%0) ;"
	    "lfd %%f24, 192(%0) ;"
	    "lfd %%f25, 200(%0) ;"
	    "lfd %%f26, 208(%0) ;"
	    "lfd %%f27, 216(%0) ;"
	    "lfd %%f28, 224(%0) ;"
	    "lfd %%f29, 232(%0) ;"
	    "lfd %%f30, 240(%0) ;"
	    "lfd %%f31, 248(%0) ;"
	    : /* ouputs */
	    : /* inputs */
	      "b" (start)
	    );
#endif
    tcb_resources_activate_fpu (self,  tcb );
}

