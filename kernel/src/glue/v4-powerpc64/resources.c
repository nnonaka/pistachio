/*********************************************************************
 *                
 * Copyright (C) 2003-2004,  National ICT Australia (NICTA)
 *                
 * File path:     glue/v4-powerpc64/resources.c
 * Description:   thread resource management
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
 * $Id: resources.cc,v 1.5 2004/06/04 06:38:41 cvansch Exp $
 *                
 ********************************************************************/
#include INC_API(tcb.h)
#include INC_ARCH(msr.h)

#include <kdb/tracepoints.h>

/* Processor specific resources that are handled lazily
 * such as the floating point registers. This keeps a copy
 * of the resource owner for lazy replacement
 */
processor_resources_t processor_resources UNIT("cpulocal");

DECLARE_TRACEPOINT (DISABLED_FPU);

void tcb_resources_save (thread_resources_t *self, tcb_t * tcb)
{
}

void tcb_resources_load (thread_resources_t *self, tcb_t * tcb)
{
}

void tcb_resources_purge (thread_resources_t *self, tcb_t * tcb)
{
}

void tcb_resources_free (thread_resources_t *self, tcb_t * tcb)
{
}

void tcb_resources_init (thread_resources_t *self, tcb_t * tcb)
{
    processor_resources_clear_fp_lazy_tcb (get_resources());
    self->fpu_fpscr = 0;

    tcb->resource_bits = 0;
}

/* Were private members; nothing outside this file used them. */
INLINE void tcb_resources_deactivate_fpu( thread_resources_t *self, tcb_t *tcb )
{
    powerpc64_irq_context_t * context = tcb_irq_context (tcb);

    processor_resources_clear_fp_lazy_tcb (get_resources());

    context->srr1 &= ~MSR_FP;	    /* Disable floating point in user */
}

INLINE void tcb_resources_activate_fpu( thread_resources_t *self, tcb_t *tcb )
{
    powerpc64_irq_context_t * context = tcb_irq_context (tcb);

    processor_resources_set_fp_lazy_tcb (get_resources(), tcb);

    context->srr1 |= MSR_FP;	    /* Enable floating point in user */
}

static void tcb_resources_spill_fpu( thread_resources_t *self, tcb_t *tcb )
{
    /* Spill the registers. */
    u64_t *start = self->fpu_gprs;
    asm volatile (
	    "stfd  %%f0,    0(%0) ;"
	    "stfd  %%f1,    8(%0) ;"
	    "stfd  %%f2,   16(%0) ;"
	    "stfd  %%f3,   24(%0) ;"
	    "stfd  %%f4,   32(%0) ;"
	    "stfd  %%f5,   40(%0) ;"
	    "stfd  %%f6,   48(%0) ;"
	    "stfd  %%f7,   56(%0) ;"
	    "stfd  %%f8,   64(%0) ;"
	    "stfd  %%f9,   72(%0) ;"
	    "stfd  %%f10,  80(%0) ;"
	    "stfd  %%f11,  88(%0) ;"
	    "stfd  %%f12,  96(%0) ;"
	    "stfd  %%f13, 104(%0) ;"
	    "stfd  %%f14, 112(%0) ;"
	    "stfd  %%f15, 120(%0) ;"
	    "stfd  %%f16, 128(%0) ;"
	    "stfd  %%f17, 136(%0) ;"
	    "stfd  %%f18, 144(%0) ;"
	    "stfd  %%f19, 152(%0) ;"
	    "stfd  %%f20, 160(%0) ;"
	    "stfd  %%f21, 168(%0) ;"
	    "stfd  %%f22, 176(%0) ;"
	    "stfd  %%f23, 184(%0) ;"
	    "stfd  %%f24, 192(%0) ;"
	    "stfd  %%f25, 200(%0) ;"
	    "stfd  %%f26, 208(%0) ;"
	    "stfd  %%f27, 216(%0) ;"
	    "stfd  %%f28, 224(%0) ;"
	    "stfd  %%f29, 232(%0) ;"
	    "stfd  %%f30, 240(%0) ;"
	    "stfd  %%f31, 248(%0) ;"
	    : /* outputs */
	    : /* inputs */
	      "b" (start)
	);

    /* Spill the fpscr.  */
    asm volatile (
	    "mffs %%f0 ;"
	    "stfd %%f0, 0(%0) ;"
	    : /* ouputs */
	    : /* inputs */
	      "b" (&self->fpu_fpscr)
	);

    tcb_resources_deactivate_fpu( self, tcb );
}

static void tcb_resources_restore_fpu( thread_resources_t *self, tcb_t *tcb )
{
    tcb_resources_activate_fpu( self, tcb );

    /* Restore the fpscr.  */
    asm volatile (
	    "lfd  %%f0, 0(%0) ;"
	    "mtfsf 0xff, %%f0 ;"
	    : /* ouputs */
	    : /* inputs */
	      "b" (&self->fpu_fpscr)
	);

    /* Restore the registers. */
    u64_t *start = self->fpu_gprs;
    asm volatile (
	    "lfd  %%f0,    0(%0) ;"
	    "lfd  %%f1,    8(%0) ;"
	    "lfd  %%f2,   16(%0) ;"
	    "lfd  %%f3,   24(%0) ;"
	    "lfd  %%f4,   32(%0) ;"
	    "lfd  %%f5,   40(%0) ;"
	    "lfd  %%f6,   48(%0) ;"
	    "lfd  %%f7,   56(%0) ;"
	    "lfd  %%f8,   64(%0) ;"
	    "lfd  %%f9,   72(%0) ;"
	    "lfd  %%f10,  80(%0) ;"
	    "lfd  %%f11,  88(%0) ;"
	    "lfd  %%f12,  96(%0) ;"
	    "lfd  %%f13, 104(%0) ;"
	    "lfd  %%f14, 112(%0) ;"
	    "lfd  %%f15, 120(%0) ;"
	    "lfd  %%f16, 128(%0) ;"
	    "lfd  %%f17, 136(%0) ;"
	    "lfd  %%f18, 144(%0) ;"
	    "lfd  %%f19, 152(%0) ;"
	    "lfd  %%f20, 160(%0) ;"
	    "lfd  %%f21, 168(%0) ;"
	    "lfd  %%f22, 176(%0) ;"
	    "lfd  %%f23, 184(%0) ;"
	    "lfd  %%f24, 192(%0) ;"
	    "lfd  %%f25, 200(%0) ;"
	    "lfd  %%f26, 208(%0) ;"
	    "lfd  %%f27, 216(%0) ;"
	    "lfd  %%f28, 224(%0) ;"
	    "lfd  %%f29, 232(%0) ;"
	    "lfd  %%f30, 240(%0) ;"
	    "lfd  %%f31, 248(%0) ;"
	    : /* outputs */
	    : /* inputs */
	      "b" (start)
	);

}

void tcb_resources_powerpc64_fpu_unavail_exception( thread_resources_t *self, tcb_t *tcb )
{
    tcb_t * fp_tcb = processor_resources_get_fp_lazy_tcb (get_resources());

    /* See the note in space.c: the printf() was the macro's `str' argument. */
    TRACEPOINT (DISABLED_FPU, "FPU disabled exception:  cur=%p  owner=%p\n",
		tcb, fp_tcb);

    /* In our lazy floating point model, we should never see a floating point
     * exception if the current tcb already owns the floating point register
     * file.
     */
    ASSERT( fp_tcb != tcb );

    ppc64_enable_fpu();

    if( fp_tcb )
	tcb_resources_spill_fpu( &fp_tcb->resources, fp_tcb );

    tcb_resources_restore_fpu( self, tcb );
}

void tcb_resources_powerpc64_fpu_spill( thread_resources_t *self, tcb_t *tcb )
{
    tcb_t * fp_tcb = processor_resources_get_fp_lazy_tcb (get_resources());

    ASSERT( tcb );

    if (tcb == fp_tcb) {
	ppc64_enable_fpu();
	tcb_resources_spill_fpu( &fp_tcb->resources, fp_tcb );
    }
}
