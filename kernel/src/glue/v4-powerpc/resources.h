/*********************************************************************
 *                
 * Copyright (C) 1999-2011,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     glue/v4-powerpc/resources.h
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
#ifndef __GLUE__V4_POWERPC__RESOURCES_H__
#define __GLUE__V4_POWERPC__RESOURCES_H__

#define HAVE_RESOURCE_TYPE_E
enum resource_type_e {
    KERNEL_THREAD	= 0, /* disables fast path */
    KERNEL_IPC		= 1, /* disables fast path */
    FPU			= 2,
    COPY_AREA		= 3,
    SOFTHVM		= 4,
};


#define FPU_REGS	32

/* support for extended floating point state like AltiVec or Double Hummer */
#ifdef CONFIG_SUBPLAT_440_BGP
#define FPU_EXTRA_REGS	32
#else
#define FPU_EXTRA_REGS	0
#endif

/* Was a class deriving from generic_thread_resources_t, which is empty and no
   longer defined anywhere, so as on x86 the base is simply not embedded. */
struct thread_resources_t {
    word_t copy_area_offset;
    word_t fpscr;
    u64_t  fpu_state[FPU_REGS + FPU_EXTRA_REGS] __attribute__((aligned(16)));
};
typedef struct thread_resources_t thread_resources_t;

#ifdef CONFIG_X_PPC_SOFTHVM
/* was the static member thread_resources_t::last_hvm_tcb */
extern tcb_t *thread_resources_last_hvm_tcb;
#endif

BEGIN_DECLS
/* Defined in resources.c. */
void   tcb_resources_dump (thread_resources_t *self, tcb_t *tcb);
void   tcb_resources_save (thread_resources_t *self, tcb_t *tcb);
void   tcb_resources_load (thread_resources_t *self, tcb_t *tcb);
void   tcb_resources_purge (thread_resources_t *self, tcb_t *tcb);
void   tcb_resources_init (thread_resources_t *self, tcb_t *tcb);
void   tcb_resources_free (thread_resources_t *self, tcb_t *tcb);
void   tcb_resources_spill_fpu (thread_resources_t *self, tcb_t *tcb);
void   tcb_resources_restore_fpu (thread_resources_t *self, tcb_t *tcb);
void   tcb_resources_reown_fpu (thread_resources_t *self, tcb_t *tcb, tcb_t *new_owner);
void   tcb_resources_deactivate_fpu (thread_resources_t *self, tcb_t *tcb);
void   tcb_resources_activate_fpu (thread_resources_t *self, tcb_t *tcb);
#ifdef CONFIG_X_PPC_SOFTHVM
void   tcb_resources_enable_hvm_mode (thread_resources_t *self, tcb_t *tcb);
void   tcb_resources_disable_hvm_mode (thread_resources_t *self, tcb_t *tcb);
#endif
END_DECLS

/* was the private change_segment helper */
INLINE addr_t tcb_resources_change_segment (addr_t addr, word_t segment)
{
    word_t tmp = (word_t)addr;
    tmp &= 0x0fffffff;
    tmp |= segment << 28;
    return (addr_t)tmp;
}


INLINE tcb_t *get_fp_lazy_tcb()
{
    extern tcb_t *_fp_lazy_tcb;
    return _fp_lazy_tcb;
}

INLINE void set_fp_lazy_tcb( tcb_t *tcb )
{
    extern tcb_t *_fp_lazy_tcb;
    _fp_lazy_tcb = tcb;
}


#endif /* !__GLUE__V4_POWERPC__RESOURCES_H__ */
