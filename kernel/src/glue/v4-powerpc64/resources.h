/*********************************************************************
 *                
 * Copyright (C) 2003,  National ICT Australia (NICTA)
 *                
 * File path:     glue/v4-powerpc64/resources.h
 * Description:   Resource bit definitions for powerpc64
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
 * $Id: resources.h,v 1.5 2004/06/04 02:52:57 cvansch Exp $
 *                
 ********************************************************************/

#ifndef __GLUE__V4_POWERPC64__RESOURCES_H__
#define __GLUE__V4_POWERPC64__RESOURCES_H__

/* Was a class deriving from generic_thread_resources_t, which is empty and no
   longer defined anywhere, so as on powerpc and x86 the base is simply not
   embedded.  The five virtual-looking members become the tcb_resources_*
   entry points api/v4/thread.c calls; the private spill/restore/activate/
   deactivate helpers have no callers outside resources.c and are static
   there. */
struct thread_resources_t {
    u64_t fpu_gprs[32];	/* 32 FPRs */
    u64_t fpu_fpscr;	/* FPU status/condition register */
};
typedef struct thread_resources_t thread_resources_t;

/* Was inherited unchanged from the empty generic_thread_resources_t, so it has
   nothing to print; kdb/api/v4/tcb.c calls it unconditionally. */
INLINE void tcb_resources_dump (thread_resources_t *self, tcb_t *tcb) { }

BEGIN_DECLS
/* Defined in resources.c. */
void tcb_resources_save (thread_resources_t *self, tcb_t *tcb);
void tcb_resources_load (thread_resources_t *self, tcb_t *tcb);
void tcb_resources_purge (thread_resources_t *self, tcb_t *tcb);
void tcb_resources_init (thread_resources_t *self, tcb_t *tcb);
void tcb_resources_free (thread_resources_t *self, tcb_t *tcb);

/* powerpc64 specific; called from glue/v4-powerpc64/exception.c and
   kdb/arch/powerpc64/frame.c respectively. */
void tcb_resources_powerpc64_fpu_unavail_exception (thread_resources_t *self, tcb_t *tcb);
void tcb_resources_powerpc64_fpu_spill (thread_resources_t *self, tcb_t *tcb);
END_DECLS


struct processor_resources_t {
    tcb_t *fp_lazy_tcb;
};
typedef struct processor_resources_t processor_resources_t;

INLINE processor_resources_t *get_resources(void)
{
    extern processor_resources_t processor_resources;
    return &processor_resources;
}

INLINE void processor_resources_init_cpu (processor_resources_t *self)
    { self->fp_lazy_tcb = NULL; }
INLINE tcb_t * processor_resources_get_fp_lazy_tcb (processor_resources_t *self)
    { return self->fp_lazy_tcb; }
INLINE void processor_resources_set_fp_lazy_tcb (processor_resources_t *self, tcb_t *tcb)
    { self->fp_lazy_tcb = tcb; }
INLINE void processor_resources_clear_fp_lazy_tcb (processor_resources_t *self)
    { self->fp_lazy_tcb = NULL; }

#endif /* !__GLUE__V4_POWERPC64__RESOURCES_H__ */
