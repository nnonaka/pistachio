/*********************************************************************
 *                
 * Copyright (C) 2003, 2006-2008,  Karlsruhe University
 *                
 * File path:     arch/x86/x64/segdesc.h
 * Description:   paste ia32/segdesc.h, s/ia32/amd64
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
 * $Id: segdesc.h,v 1.5 2006/10/20 17:17:58 reichelt Exp $
 *                
 ********************************************************************/
#ifndef __ARCH__X86__X64__SEGDESC_H__
#define __ARCH__X86__X64__SEGDESC_H__

#include INC_ARCH(cpu.h)

/**
 * Code or Data Segment Descriptor for long/compatibility mode
 */
struct x86_segdesc_t
{
    union {
	u64_t raw;
	struct {
	    u64_t limit_low	: 16;
	    u64_t base_low	: 24;
	    u64_t type		:  4;
	    u64_t s		:  1;
	    u64_t dpl		:  2;
	    u64_t p		:  1;
	    u64_t limit_high	:  4;
	    u64_t avl		:  1;
	    u64_t l		:  1;
	    u64_t d		:  1;
	    u64_t g		:  1;
	    u64_t base_high	:  8;
	} d;
    } x;
};
typedef struct x86_segdesc_t x86_segdesc_t;

/* segtype_e / mode_e / msr_e values as macros so C can reference them. */
#define X86_SEGDESC_INV		0x0
#define X86_SEGDESC_CODE	0xb
#define X86_SEGDESC_DATA	0x3
#define X86_SEGDESC_M_LONG	1
#define X86_SEGDESC_M_COMP	0
#define X86_SEGDESC_MSR_NONE	0
#define X86_SEGDESC_MSR_FS	1
#define X86_SEGDESC_MSR_GS	2

/* C form of the 5-arg x86_segdesc_t::set_seg (the union is C-visible). */
INLINE void x86_segdesc_set_seg (x86_segdesc_t *self, u64_t base, int type, int dpl, int mode, int msr)
{
    if (msr != X86_SEGDESC_MSR_NONE && (base >> 32))
    {
	u32_t reg = (msr == X86_SEGDESC_MSR_FS) ? X86_X64_MSR_FS : X86_X64_MSR_GS;
	x86_wrmsr (reg, base);
    }

    self->x.d.base_low   = base & 0xFFFFFF;
    self->x.d.base_high  = (base >> 24) & 0xFF;

    self->x.d.limit_low  = 0xFFFF;
    self->x.d.limit_high = 0xF;

    self->x.d.g = 1;

    self->x.d.type = type & 0xF;
    self->x.d.l    = mode & 0x1;
    self->x.d.dpl  = dpl & 0x3;

    if (mode == X86_SEGDESC_M_LONG && type == X86_SEGDESC_CODE)
	self->x.d.d = 0;
    else
	self->x.d.d = 1;

    self->x.d.p = 1;
    self->x.d.s = 1;
    self->x.d.avl = 0;
}

/* 
 * Limits are ignored for code/data segments in 64bit mode, 
 * addresses are ignored unless segment is selected by FS or GS
 */

#if !defined(X64_32BIT_CODE)

/**
 * TSS Descriptor for long/compatibility mode
 */
struct x86_tssdesc_t
{
    union {
	u64_t raw[2];
	struct {
	    u64_t limit_low	: 16;
	    u64_t base_low	: 24;
	    u64_t type		:  4;
	    u64_t s		:  1;
	    u64_t dpl		:  2;
	    u64_t p		:  1;
	    u64_t limit_high	:  4;
	    u64_t avl		:  1;
	    u64_t res0		:  2;
	    u64_t g		:  1;
	    u64_t base_med	:  8;
	    u64_t base_high     : 32;
	    u64_t res1		:  8;
	    u64_t mbz		:  5;
	    u64_t res2		: 19;
	    
	} d;
    } x;
};
typedef struct x86_tssdesc_t x86_tssdesc_t;

/* 
 * Limits are checked 64bit mode, 
 * Addresses are ignored unless segment is for selected 
 * by FS or GS
 */   
INLINE void x86_tssdesc_set_seg (x86_tssdesc_t *self, u64_t base, u32_t limit)
{
    self->x.d.base_low  = base & 0xFFFFFF;
    self->x.d.base_med  = (base >> 24) & 0xFF;
    self->x.d.base_high = (u32_t) ((base >> 32) & 0xFFFFFFFF);

    if (limit >= (1 << 20))
    {
	self->x.d.limit_low  = (limit >> 12) & 0xFFFF;
	self->x.d.limit_high = (u8_t) (limit >> 28) & 0xF;
	self->x.d.g = 1;      /* 4K granularity       */
    }
    else
    {
	self->x.d.limit_low  =  limit        & 0xFFFF;
	self->x.d.limit_high = (limit >> 16) & 0xF;
	self->x.d.g = 0;      /* 1B granularity       */
    }

    self->x.d.type = 0x9;	/* 64bit TSS type	*/
    self->x.d.s = 0;		/* system segment	*/
    self->x.d.dpl =  0;		/* Privilege Level 0	*/
    self->x.d.p = 1;		/* present		*/
    self->x.d.avl = 0;
    self->x.d.mbz = 0;
    self->x.d.res0 = 0;
}


/**
 * IDT Descriptor for long mode
 * Note: Would look different for compatibility mode
 */

struct x86_idtdesc_t
{
    union {
	u64_t raw[2];
	struct {
	    u64_t offset_low	: 16;
	    u64_t selector	: 16; 
	    u64_t ist		:  3;
	    u64_t res0		:  5;
	    u64_t type		:  4;
	    u64_t s		:  1;
	    u64_t dpl		:  2;
	    u64_t p		:  1;
	    u64_t offset_high	: 48 __attribute__((packed)); 
	    u64_t res1		: 32;
	} d;
    } x;
};
typedef struct x86_idtdesc_t x86_idtdesc_t;

/* C free-function API for x86_idtdesc_t (the C++ set() method above is used by
   code still compiled as C++; segtype values mirror segtype_e). */
#define X86_IDTDESC_INTERRUPT	0xe
#define X86_IDTDESC_TRAP	0xf

static inline void x86_idtdesc_set(x86_idtdesc_t *self, u16_t selector,
				   void (*address)(void), int type, int dpl, int ist)
{
    /* offset_high holds bits 16..63 of address, i.e. exactly 48 bits */
    u64_t offset_high = (u64_t) address >> 16;

    self->x.d.offset_low = ( (u64_t) address & 0xFFFF );
    self->x.d.offset_high = offset_high & 0xFFFFFFFFFFFF;
    self->x.d.selector   = selector;
    self->x.d.ist = ((u64_t) ist) & 0x7;
    self->x.d.type = ((u64_t) type) & 0xF;
    self->x.d.dpl = ((u64_t) dpl) & 0x3;

    self->x.d.p = 1;		/* present */
    self->x.d.s = 0;		/* system segment */

    self->x.d.res0 = 0;
    self->x.d.res1 = 0;
}


#endif /* !X64_32BIT_CODE */

#endif /* !__ARCH__X86__X64__SEGDESC_H__ */
