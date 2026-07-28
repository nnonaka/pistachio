/*********************************************************************
 *                
 * Copyright (C) 2002-2008, 2010,  Karlsruhe University
 *                
 * File path:     arch/x86/x64/ptab.h
 * Description:   X86-64 pagetable management (4KByte long mode)
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
 * $Id: ptab.h,v 1.9 2006/11/18 10:15:04 stoess Exp $
 *                
 ********************************************************************/


#ifndef __ARCH__X86__X64__PTAB_H__
#define __ARCH__X86__X64__PTAB_H__

#include INC_ARCH(x86.h)

#if !defined(ASSEMBLY)

#define HW_PGSHIFTS             { 12, 21, 30, 39, 48, 64 }
#define HW_VALID_PGSIZES        ((1 << 12) | (1 << 21))

#define MDB_PGSHIFTS            { 12, 21, 30, 39, 48 }
#define MDB_NUM_PGSIZES         (4)

/* Page-size indices hoisted to macros so C (linear_ptab_walker.c and the
   pgent_t C API) can name them; the pgsize_e enum below aliases these. */
#define X86_PGSIZE_4K		0
#define X86_PGSIZE_2M		1
#define X86_PGSIZE_1G		2
#define X86_PGSIZE_512G		3
#define X86_PGSIZE_SYNC		X86_PGSIZE_1G
#define X86_PGSIZE_SUPERPAGE	X86_PGSIZE_2M
#define X86_PGSIZE_MAX		X86_PGSIZE_512G

#define X86_PGSIZES		{ size_4k = X86_PGSIZE_4K, size_2m = X86_PGSIZE_2M,	\
				  size_1g = X86_PGSIZE_1G, size_512g = X86_PGSIZE_512G,	\
				  size_sync = X86_PGSIZE_SYNC, size_superpage = X86_PGSIZE_SUPERPAGE, \
				  size_max = X86_PGSIZE_MAX }

struct x86_pgent_t
{
    union {
	struct {
	    u64_t present		:1;
	    u64_t rw			:1;
	    u64_t privilege		:1;
	    u64_t write_through		:1;

	    u64_t cache_disabled	:1;
	    u64_t accessed		:1;
	    u64_t dirty			:1;
	    u64_t pat			:1;

	    u64_t global		:1;
	    u64_t cpulocal		:1;
	    u64_t avl			:2;
	    
	    u64_t base			:40;
	    u64_t available		:11;
	    u64_t nx		        :1;

	} pg4k;
	struct {
	    u64_t present		:1;
	    u64_t rw			:1;
	    u64_t privilege		:1;
	    u64_t write_through		:1;

	    u64_t cache_disabled	:1;
	    u64_t accessed		:1;
	    u64_t dirty			:1;
	    u64_t super			:1;
	    
	    u64_t global		:1;
	    u64_t cpulocal		:1;
	    u64_t avl			:2;
	    
	    u64_t pat			:1;  
	    u64_t mbz			:8;
	    u64_t base			:31;
	    u64_t available		:11;
	    u64_t nx		        :1;

	} pg2m;
	u64_t raw;
    };

};
typedef struct x86_pgent_t x86_pgent_t;

/* C forms of the x86_pgent_t bit-twiddling methods (the pg4k/pg2m/raw union is
   C-visible above); used by the pgent_t C API in glue/v4-x86/space.c.  size is
   X86_PGSIZE_4K/2M. */
INLINE bool x86_pgent_is_valid (x86_pgent_t *self)		{ return self->pg4k.present == 1; }
INLINE bool x86_pgent_is_writable (x86_pgent_t *self)		{ return self->pg4k.rw == 1; }
INLINE bool x86_pgent_is_executable (x86_pgent_t *self)		{ return self->pg4k.nx == 0; }
INLINE bool x86_pgent_is_accessed (x86_pgent_t *self)		{ return self->pg4k.accessed == 1; }
INLINE bool x86_pgent_is_dirty (x86_pgent_t *self)		{ return self->pg4k.dirty == 1; }
INLINE bool x86_pgent_is_global (x86_pgent_t *self)		{ return self->pg4k.global == 1; }
INLINE bool x86_pgent_is_cpulocal (x86_pgent_t *self)		{ return self->pg4k.cpulocal == 1; }
INLINE bool x86_pgent_is_superpage (x86_pgent_t *self)		{ return self->pg2m.super == 1; }
INLINE bool x86_pgent_is_kernel (x86_pgent_t *self)		{ return self->pg4k.privilege == 0; }
INLINE bool x86_pgent_is_write_through (x86_pgent_t *self)	{ return self->pg4k.write_through == 1; }
INLINE bool x86_pgent_is_cache_disabled (x86_pgent_t *self)	{ return self->pg4k.cache_disabled == 1; }
INLINE word_t x86_pgent_is_pat (x86_pgent_t *self, word_t size)	{ return (size == X86_PGSIZE_4K ? self->pg4k.pat : self->pg2m.pat); }
INLINE addr_t x86_pgent_get_address (x86_pgent_t *self, word_t size)
{ return (addr_t) (size == X86_PGSIZE_4K ? (self->raw & X86_PAGE_MASK) : (self->raw & X86_SUPERPAGE_MASK)); }
INLINE x86_pgent_t * x86_pgent_get_ptab (x86_pgent_t *self)	{ return (x86_pgent_t *) (self->raw & X86_X64_PTE_MASK); }
INLINE u64_t x86_pgent_get_raw (x86_pgent_t *self)		{ return self->raw; }
INLINE void x86_pgent_clear (x86_pgent_t *self)			{ self->raw = 0; }
INLINE void x86_pgent_set_entry (x86_pgent_t *self, addr_t addr, word_t size, u64_t attrib)
{
    if (size == X86_PGSIZE_4K)
	self->raw = (((u64_t) addr & X86_PAGE_MASK) | (attrib & X86_PAGE_FLAGS_MASK));
    else
	self->raw = (((u64_t) addr & X86_SUPERPAGE_MASK) | X86_PAGE_SUPER | (attrib & X86_SUPERPAGE_FLAGS_MASK));
}
INLINE void x86_pgent_set_cacheability (x86_pgent_t *self, bool cacheable, word_t size)
{
    self->pg4k.cache_disabled = !cacheable;
    if (size == X86_PGSIZE_4K) self->pg4k.pat = 0; else self->pg2m.pat = 0;
}
INLINE void x86_pgent_set_pat (x86_pgent_t *self, word_t pat, word_t size)
{
    self->pg4k.write_through  = (pat & 1) ? 1 : 0;
    self->pg4k.cache_disabled = (pat & 2) ? 1 : 0;
    if (size == X86_PGSIZE_4K) self->pg4k.pat = (pat & 4) ? 1 : 0; else self->pg2m.pat = (pat & 4) ? 1 : 0;
}
INLINE void x86_pgent_set_global (x86_pgent_t *self, bool global)	{ self->pg4k.global = global; }
INLINE void x86_pgent_set_cpulocal (x86_pgent_t *self, bool local)	{ self->pg4k.cpulocal = local; }
INLINE void x86_pgent_set_ptab_entry (x86_pgent_t *self, addr_t addr, u32_t attrib)
{ self->raw = ((u64_t) addr & X86_X64_PTE_MASK) | X86_PAGE_VALID | (attrib & X86_X64_PTE_FLAGS_MASK); }

#endif /* !ASSEMBLY */


#endif /* !__ARCH__X86__X64__PTAB_H__ */
