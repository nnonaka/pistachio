/*********************************************************************
 *                
 * Copyright (C) 2005-2006,  Karlsruhe University
 *                
 * File path:     platform/pc99/io_fpage.h
 * Description:   IO fpage declaration
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
 * $Id: io_fpage.h,v 1.5 2006/02/21 08:43:57 stoess Exp $
 *                
 ********************************************************************/
#ifndef __PLATFORM__PC99__IO_FPAGE_H__
#define __PLATFORM__PC99__IO_FPAGE_H__


#if !defined(CONFIG_X86_IO_FLEXPAGES)

#include INC_API(generic-archfpage.h)

#else

#include INC_API(config.h)

struct space_t; typedef struct space_t space_t;
struct fpage_t;
struct tcb_t;

/**
 * Flexpages are size-aligned memory objects and can cover
 * multiple hardware pages.  arch_fpage_t implements the IO-port flexpage
 * type.  Access rights are implicit: an IO fpage is always rwx.
 */
struct arch_fpage_t
{
    union {
	struct {
	    BITFIELD5(word_t,
		      reserved          : 4,
		      two               : 6,
		      size              : 6,
		      base              :16,
		      : BITS_WORD - 32
		);
	} io __attribute__((packed));
	word_t raw;
    } x;
};
typedef struct arch_fpage_t arch_fpage_t;


/**
 * sets the flexpage
 */
INLINE void arch_fpage_set (arch_fpage_t *self, word_t base, word_t log2size,
			    bool read, bool write, bool exec)
{
    (void) read; (void) write; (void) exec;
    self->x.raw = 0;
    self->x.io.two = 2;
    /* I/O ports are 16 bit wide, so cutting the base down to the
       16 bit base field is the intended encoding */
    self->x.io.base = (u16_t) (base & (~0UL << log2size));
    self->x.io.size = log2size & 0x3f;
}

/** @return true if the flexpage is a nil fpage */
INLINE bool arch_fpage_is_valid_page (arch_fpage_t *self)
{ return self->x.io.two == 2; }

/** @return true if flexpage covers the whole I/O address space */
INLINE bool arch_fpage_is_complete_page (arch_fpage_t *self)
{ return (self->x.io.size == 16 && self->x.io.base == 0); }

/** @return port of the IO fpage */
INLINE u16_t arch_fpage_get_port (arch_fpage_t *self)
{ return (u16_t) self->x.io.base; }

/** @return base address of the fpage (not size-aligned) */
INLINE addr_t arch_fpage_get_base (arch_fpage_t *self)
{ return (addr_t) (word_t) (self->x.io.base); }

/** @return size aligned address of the fpage */
INLINE addr_t arch_fpage_get_address (arch_fpage_t *self)
{ return (addr_t) (word_t) (self->x.io.base & (~0UL << self->x.io.size)); }

/** @return size of the flexpage */
INLINE word_t arch_fpage_get_size (arch_fpage_t *self)
{ return (1UL << self->x.io.size); }

/** @return log2 size of the fpage */
INLINE word_t arch_fpage_get_size_log2 (arch_fpage_t *self)
{ return self->x.io.size; }

/*
 * An IO fpage carries no permission bits -- the C++ methods returned true
 * unconditionally and the setters were empty.  Transcribed as they were.
 */
INLINE bool arch_fpage_is_read (arch_fpage_t *self)	{ (void) self; return true; }
INLINE bool arch_fpage_is_write (arch_fpage_t *self)	{ (void) self; return true; }
INLINE bool arch_fpage_is_execute (arch_fpage_t *self)	{ (void) self; return true; }
INLINE bool arch_fpage_is_rwx (arch_fpage_t *self)	{ (void) self; return true; }
INLINE void arch_fpage_set_rwx_all (arch_fpage_t *self)	{ (void) self; }
INLINE void arch_fpage_set_rwx (arch_fpage_t *self, word_t rwx)
{ (void) self; (void) rwx; }
/* NB: get_rwx returned `true', i.e. 1, not a full rwx mask.  Kept. */
INLINE word_t arch_fpage_get_rwx (arch_fpage_t *self)	{ (void) self; return true; }

/** @return an fpage covering the complete IO address space */
INLINE arch_fpage_t arch_fpage_complete (void)
{
    arch_fpage_t ret;
    ret.x.raw = 0;
    ret.x.io.two = 2;
    ret.x.io.size = 16;
    return ret;
}

#endif /* !defined(CONFIG_X86_IO_FLEXPAGES) */

#endif /* !__PLATFORM__PC99__IO_FPAGE_H__ */
