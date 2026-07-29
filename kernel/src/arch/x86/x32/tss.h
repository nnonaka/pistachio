/*********************************************************************
 *                
 * Copyright (C) 2002, 2005, 2007-2008,  Karlsruhe University
 *                
 * File path:     arch/x86/x32/tss.h
 * Description:   IA32 Task State Segment
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
 * $Id: tss.h,v 1.4 2005/05/18 15:08:29 stoess Exp $
 *                
 ********************************************************************/
#ifndef __ARCH__X86__X32__TSS_H__
#define __ARCH__X86__X32__TSS_H__


#if defined(CONFIG_X86_IO_FLEXPAGES)
#define X86_X32_IOPERMBITMAP_BITS          (1 << 16)
#define X86_X32_IOPERMBITMAP_ALIGNMENT     __attribute__((aligned(4096)));
#else
#define X86_X32_IOPERMBITMAP_BITS 0
#define X86_X32_IOPERMBITMAP_ALIGNMENT
#endif

#define IOPERMBITMAP_SIZE		(X86_X32_IOPERMBITMAP_BITS / 8)


struct x86_x32_tss_t
{
    u32_t	link;
    u32_t	esp0, ss0;
    u32_t	esp1, ss1;
    u32_t	esp2, ss2;
    u32_t	cr3;
    u32_t	eip, eflags;
    u32_t	eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u32_t	es, cs, ss, ds, fs, gs;
    u32_t	ldt;
    u16_t	trace;
    u16_t	iopbm_offset;
    u8_t	io_bitmap[IOPERMBITMAP_SIZE] X86_X32_IOPERMBITMAP_ALIGNMENT;
    u8_t	stopper;
};
typedef struct x86_x32_tss_t x86_x32_tss_t;

/* C forms of the x86_x32_tss_t methods; the fields are C-visible.  Named
   x86_tss_* like the x64 pair, so the shared glue calls one spelling. */
INLINE void x86_tss_set_esp0 (x86_x32_tss_t *self, u32_t esp)
{
    self->esp0 = esp;
}

INLINE u32_t x86_tss_get_esp0 (x86_x32_tss_t *self)
{
    return self->esp0;
}

INLINE void x86_tss_setup (x86_x32_tss_t *self, u16_t ss0)
{
    self->ss0 = ss0;
    self->iopbm_offset = (u16_t)((u32_t) self->io_bitmap - (u32_t) self);
    self->stopper = 0xff;
}

INLINE addr_t x86_tss_get_io_bitmap (x86_x32_tss_t *self)
{
    return (addr_t) self->io_bitmap;
}

extern x86_x32_tss_t tss;


#if defined(CONFIG_IS_32BIT)
typedef x86_x32_tss_t x86_tss_t;
#endif

#endif /* !__ARCH__X86__X32__TSS_H__ */
