/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2007-2008,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/resources.h
 * Description:   ia32 specific resources
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
 ********************************************************************/
#ifndef __GLUE__V4_X86__RESOURCES_H__
#define __GLUE__V4_X86__RESOURCES_H__

#include INC_API(resources.h)

#define HAVE_RESOURCE_TYPE_E
enum resource_type_e {
    FPU			= 0,
    COPY_AREA		= 1,
#if defined(CONFIG_X86_SMALL_SPACES)
    IPC_PAGE_TABLE	= 2,
#endif
#if defined(CONFIG_SMP)
    SMP_PAGE_TABLE	= 3,
#endif
#if defined(CONFIG_IS_64BIT)
    COMPATIBILITY_MODE	= 4,
#endif
    HVM		= 5,
};


struct thread_resources_t {
    /* generic_thread_resources_t base is empty (EBO) -> not embedded in C */
    addr_t fpu_state;
    word_t last_copy_area;
    word_t pdir_idx[COPY_AREA_COUNT][COPY_AREA_PDIRS];
};
typedef struct thread_resources_t thread_resources_t;

/* C prototypes for the resources entry points defined in resources.c, so
   glue/v4-x86/thread.c (tcb_switch_to / tcb_release_copy_area) can call them.
   The __asm__ labels on the C++ methods above make both languages agree. */
BEGIN_DECLS
void tcb_resources_save (thread_resources_t *self, tcb_t *tcb);
void tcb_resources_load (thread_resources_t *self, tcb_t *tcb);
void tcb_resources_release_copy_area (thread_resources_t *self, tcb_t *tcb, bool disable_copyarea);
void tcb_resources_dump (thread_resources_t *self, tcb_t *tcb);
END_DECLS


#endif /* !__GLUE__V4_X86__RESOURCES_H__ */
