/*********************************************************************
 *                
 * Copyright (C) 2002-2006,  Karlsruhe University
 *                
 * File path:     api/v4/fpage.h
 * Description:   V4 flexpages
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
 * $Id: fpage.h,v 1.26 2006/11/14 18:46:31 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __API__V4__FPAGE_H__
#define __API__V4__FPAGE_H__

#include INC_API(config.h)
#include INC_GLUE(fpage.h)

struct mempage_t
{
    union {
	struct {
	    BITFIELD7(word_t,
		      execute		: 1,
		      write		: 1,
		      read		: 1,
		      reserved		: 1,
		      size		: 6,
		      base		: L4_FPAGE_BASE_BITS,
		      : BITS_WORD - L4_FPAGE_BASE_BITS - 10
		);
	} x __attribute__((packed));
	word_t raw;
    };
};
typedef struct mempage_t mempage_t;

/**
 * Flexpages are size-aligned memory objects and can cover 
 * multiple hardware pages. fpage_t implements the V4 specific
 * flexpage type, having read, write and execute bits.
 */
struct fpage_t
{
    /* data members */
    union {
	mempage_t mem;
	arch_fpage_t arch;
	word_t raw;
    };
    /* member functions */

};
typedef struct fpage_t fpage_t;


/*
 * Helper functions used in conjunction with mapping.
 */


/* C wrappers for the fpage_t methods and the base_mask/address helpers
   (defined in glue/v4-x86/space.cc) for generic/linear_ptab_walker.c. */
BEGIN_DECLS
bool   fpage_is_nil_fpage (fpage_t *self);
bool   fpage_is_complete_fpage (fpage_t *self);
word_t fpage_get_size_log2 (fpage_t *self);
bool   fpage_is_range_overlapping (fpage_t *self, addr_t start, addr_t end);
addr_t fpage_get_base (fpage_t *self);
addr_t fpage_get_address (fpage_t *self);
word_t fpage_get_rwx (fpage_t *self);
void   fpage_set_rwx (fpage_t *self, word_t rwx);
bool   fpage_is_read (fpage_t *self);
bool   fpage_is_write (fpage_t *self);
bool   fpage_is_execute (fpage_t *self);
void   fpage_set (fpage_t *self, word_t base, word_t size, bool read, bool write, bool exec);
word_t fpage_base_mask (fpage_t fp, word_t size);
addr_t fpage_address (fpage_t fp, word_t size);
bool   fpage_is_rwx (fpage_t *self);
bool   fpage_is_mempage (fpage_t *self);
bool   fpage_is_archpage (fpage_t *self);
bool   fpage_is_overlapping (fpage_t *self, fpage_t other);
word_t fpage_get_size (fpage_t *self);
fpage_t fpage_complete_mem (void);
void   arch_unmap_fpage_c (struct tcb_t *from, fpage_t fpage, bool flush);
void   arch_map_fpage_c (struct tcb_t *src, fpage_t snd_fpage, word_t snd_base, struct tcb_t *dst, fpage_t rcv_fpage, bool grant);
void   fpage_set_rwx_all (fpage_t *self);	/* the no-arg set_rwx() */
fpage_t fpage_nilpage (void);
END_DECLS


#endif /* !__API__V4__FPAGE_H__ */
