/*********************************************************************
 *                
 * Copyright (C) 2002-2003, 2006-2008,  Karlsruhe University
 *                
 * File path:     arch/x86/x32/ptab.h
 * Description:   pagetable management
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
 * $Id: ptab.h,v 1.11 2006/11/17 17:22:30 skoglund Exp $
 *                
 ********************************************************************/


#ifndef __ARCH__X86__X32__PTAB_H__
#define __ARCH__X86__X32__PTAB_H__

#include INC_ARCH(x86.h)

#if !defined(ASSEMBLY)

#define HW_PGSHIFTS		{ 12, 22, 32 }
#define HW_VALID_PGSIZES	((1 << 12) | (1 << 22))

#define MDB_BUFLIST_SIZES	{ {12}, {8}, {4096}, {0} }
#define MDB_PGSHIFTS		{ 12, 22, 32 }
#define MDB_NUM_PGSIZES		(2)

/* Page sizes as macros so C can name them; the C++ pagesize_e enums that used
   to carry these values alias them through X86_PGSIZES. */
#define X86_PGSIZE_4K		0
#define X86_PGSIZE_4M		1
#define X86_PGSIZE_4G		2
#define X86_PGSIZE_SYNC		X86_PGSIZE_4M
#define X86_PGSIZE_SUPERPAGE	X86_PGSIZE_4M
#define X86_PGSIZE_MAX		X86_PGSIZE_4M
#define PGENT_SIZE_MAX		X86_PGSIZE_MAX

#define X86_PGSIZES		{  size_4k = X86_PGSIZE_4K, size_4m = X86_PGSIZE_4M,	\
				   size_4g = X86_PGSIZE_4G,				\
				   size_sync = X86_PGSIZE_SYNC,				\
				   size_superpage = X86_PGSIZE_SUPERPAGE,		\
				   size_max = X86_PGSIZE_MAX }

#include <debug.h>

struct x86_pgent_t 
{
    union {
	struct {
	    unsigned present		:1;
	    unsigned rw			:1;
	    unsigned privilege		:1;
	    unsigned write_through	:1;

	    unsigned cache_disabled	:1;
	    unsigned accessed		:1;
	    unsigned dirty		:1;
	    unsigned size		:1;

	    unsigned global		:1;
	    unsigned cpulocal		:1;
	    unsigned avail		:2;

	    unsigned base		:20;
	} pg;

	struct {
	    unsigned present		:1;
	    unsigned rw			:1;
	    unsigned privilege		:1;
	    unsigned write_through	:1;

	    unsigned cache_disabled	:1;
	    unsigned accessed		:1;
	    unsigned dirty		:1;
	    unsigned size		:1;

	    unsigned global		:1;
	    unsigned cpulocal		:1;
	    unsigned avail		:2;

	    unsigned pat		:1;
	    unsigned reserved		:9;
	    unsigned base		:10;
	} pg4m;

	u32_t raw;
    };
};
typedef struct x86_pgent_t x86_pgent_t;

/*
 * C forms of the x86_pgent_t methods.  Same names as the x64 set
 * (arch/x86/x64/ptab.h), so generic/linear_ptab_walker.c and the glue see one
 * spelling on both subarchitectures.  `size' arguments are word_t holding an
 * X86_PGSIZE_* value.
 */
INLINE bool x86_pgent_is_valid (x86_pgent_t *self)		{ return self->pg.present == 1; }
INLINE bool x86_pgent_is_writable (x86_pgent_t *self)		{ return self->pg.rw == 1; }
INLINE bool x86_pgent_is_executable (x86_pgent_t *self)		{ return self->pg.present == 1; }
INLINE bool x86_pgent_is_accessed (x86_pgent_t *self)		{ return self->pg.accessed == 1; }
INLINE bool x86_pgent_is_dirty (x86_pgent_t *self)		{ return self->pg.dirty == 1; }
INLINE bool x86_pgent_is_superpage (x86_pgent_t *self)		{ return self->pg.size == 1; }
INLINE bool x86_pgent_is_kernel (x86_pgent_t *self)		{ return self->pg.privilege == 0; }
INLINE bool x86_pgent_is_write_through (x86_pgent_t *self)	{ return self->pg.write_through == 1; }
INLINE bool x86_pgent_is_cache_disabled (x86_pgent_t *self)	{ return self->pg.cache_disabled == 1; }
INLINE bool x86_pgent_is_global (x86_pgent_t *self)		{ return self->pg.global == 1; }
INLINE bool x86_pgent_is_cpulocal (x86_pgent_t *self)		{ return self->pg.cpulocal == 1; }

INLINE word_t x86_pgent_is_pat (x86_pgent_t *self, word_t size)
{
#if defined(CONFIG_X86_PAT)
    return (size == X86_PGSIZE_4K ? self->pg.size : self->pg4m.pat);
#else
    (void) self; (void) size;
    return 0;
#endif
}

INLINE addr_t x86_pgent_get_address (x86_pgent_t *self, word_t size)
{
    return (addr_t) (self->raw & (size == X86_PGSIZE_4K ? X86_PAGE_MASK : X86_SUPERPAGE_MASK));
}

INLINE x86_pgent_t * x86_pgent_get_ptab (x86_pgent_t *self)	{ return (x86_pgent_t *) (self->raw & X86_PAGE_MASK); }
INLINE u32_t x86_pgent_get_raw (x86_pgent_t *self)		{ return self->raw; }
INLINE void x86_pgent_clear (x86_pgent_t *self)			{ self->raw = 0; }

INLINE void x86_pgent_set_entry (x86_pgent_t *self, addr_t addr, word_t size, u32_t attrib)
{
    if (size == X86_PGSIZE_4K)
	self->raw = ((u32_t)(addr) & X86_PAGE_MASK) | (attrib & X86_PAGE_FLAGS_MASK);
    else
	self->raw = ((u32_t)(addr) & X86_SUPERPAGE_MASK) | X86_PAGE_SUPER |
	    (attrib & X86_SUPERPAGE_FLAGS_MASK);
}

INLINE void x86_pgent_set_ptab_entry (x86_pgent_t *self, addr_t addr, u32_t attrib)
{
    self->raw = ((u32_t)(addr) & X86_PAGE_MASK) | X86_PAGE_VALID |
	(attrib & X86_X32_PTAB_FLAGS_MASK);
}

INLINE void x86_pgent_set_cacheability (x86_pgent_t *self, bool cacheable, word_t size)
{
    self->pg.cache_disabled = !cacheable;
#if defined(CONFIG_X86_PAT)
    if (size == X86_PGSIZE_4K)
	self->pg.size = 0;
    else
	self->pg4m.pat = 0;
#else
    (void) size;
#endif
}

INLINE void x86_pgent_set_pat (x86_pgent_t *self, word_t pat, word_t size)
{
    self->pg.write_through  = (pat & 1) ? 1 : 0;
    self->pg.cache_disabled = (pat & 2) ? 1 : 0;
#if defined(CONFIG_X86_PAT)
    if (size == X86_PGSIZE_4K)
	self->pg.size  = (pat & 4) ? 1 : 0;
    else
	self->pg4m.pat = (pat & 4) ? 1 : 0;
#else
    (void) size;
#endif
}

INLINE void x86_pgent_set_global (x86_pgent_t *self, bool global)	{ self->pg.global = global; }
INLINE void x86_pgent_set_cpulocal (x86_pgent_t *self, bool local)	{ self->pg.cpulocal = local; }
INLINE void x86_pgent_set_accessed (x86_pgent_t *self, bool accessed)	{ self->pg.accessed = accessed; }
INLINE void x86_pgent_set_dirty (x86_pgent_t *self, bool dirty)		{ self->pg.dirty = dirty; }

#endif /* !ASSEMBLY */


#endif /* !__ARCH__X86__X32__PTAB_H__ */
