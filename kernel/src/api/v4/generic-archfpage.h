/*********************************************************************
 *                
 * Copyright (C) 2002-2005,  Karlsruhe University
 *                
 * File path:     generic/fpage.h
 * Description:   dummy architecture specific fpage declaration, for use by 
 *		  architectures without such pages
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
 * $Id: generic-archfpage.h,v 1.1 2005/05/19 08:34:59 stoess Exp $
 *                
 ********************************************************************/
#ifndef __GENERIC__FPAGE_H__
#define __GENERIC__FPAGE_H__

#include INC_API(config.h)

struct fpage_t;
struct tcb_t;


/**
 * Flexpages are size-aligned memory objects and can cover multiple hardware
 * pages. arch_fpage_t implements the architecture-specific flexpage type,
 * having read, write and execute bits.
 */
struct arch_fpage_t
{
    /* data members */
    word_t raw;
    /* member functions */

};
typedef struct arch_fpage_t arch_fpage_t;

/*
 * C forms of the arch_fpage_t methods.  This is the architecture that has no
 * architecture-specific flexpages, so every one of them is the answer that
 * makes fpage_t's arch branch dead code: is_valid_page() is false, and
 * api/v4/accessors.c's fpage accessors therefore take the mem-page branch.
 * glue/v4-x86/io_fpage.h supplies the other set under CONFIG_X86_IO_FLEXPAGES.
 */
INLINE void   arch_fpage_set (arch_fpage_t *self, word_t base, word_t log2size,
			      bool read, bool write, bool exec)
{ (void) self; (void) base; (void) log2size; (void) read; (void) write; (void) exec; }
INLINE bool   arch_fpage_is_valid_page (arch_fpage_t *self)	{ (void) self; return false; }
INLINE bool   arch_fpage_is_complete_page (arch_fpage_t *self)	{ (void) self; return false; }
INLINE addr_t arch_fpage_get_base (arch_fpage_t *self)		{ (void) self; return NULL; }
INLINE addr_t arch_fpage_get_address (arch_fpage_t *self)	{ (void) self; return NULL; }
INLINE word_t arch_fpage_get_size (arch_fpage_t *self)		{ (void) self; return 0; }
INLINE word_t arch_fpage_get_size_log2 (arch_fpage_t *self)	{ (void) self; return 0; }
INLINE bool   arch_fpage_is_read (arch_fpage_t *self)		{ (void) self; return false; }
INLINE bool   arch_fpage_is_write (arch_fpage_t *self)		{ (void) self; return false; }
INLINE bool   arch_fpage_is_execute (arch_fpage_t *self)	{ (void) self; return false; }
INLINE bool   arch_fpage_is_rwx (arch_fpage_t *self)		{ (void) self; return true; }
INLINE void   arch_fpage_set_rwx_all (arch_fpage_t *self)	{ (void) self; }
INLINE void   arch_fpage_set_rwx (arch_fpage_t *self, word_t rwx) { (void) self; (void) rwx; }
/* NB: get_rwx() returned `false', i.e. 0, from a word_t function.  Kept. */
INLINE word_t arch_fpage_get_rwx (arch_fpage_t *self)		{ (void) self; return false; }

INLINE arch_fpage_t arch_fpage_complete (void)
{
    arch_fpage_t ret;
    ret.raw = 0;
    return ret;
}


#endif /* !__GENERIC__FPAGE_H__ */
