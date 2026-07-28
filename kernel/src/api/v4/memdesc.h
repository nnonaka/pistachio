/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2008, 2010,  Karlsruhe University
 *                
 * File path:     api/v4/memdesc.h
 * Description:   Memory descriptors for kernel interface page
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
 * $Id: memdesc.h,v 1.8 2004/03/15 21:33:59 ud3 Exp $
 *                
 ********************************************************************/
#ifndef __API__V4__MEMDESC_H__
#define __API__V4__MEMDESC_H__


/**
 * Descriptor for a memory region described in the kernel interface
 * page.  Regions have a type, an upper, a lower limit (multiple of
 * 1K), and are valid for either virtual memory or physical memory.
 */
struct memdesc_t
{
    BITFIELD5(word_t,
	_type	: 4,
	_t	: 4,
		: 1,
	_v	: 1,
	_low	: BITS_WORD - 10
	);
    BITFIELD2(word_t,
		: 10,
	_high	: BITS_WORD - 10
	);

};
typedef struct memdesc_t memdesc_t;

/* Memory-descriptor type values, named as macros so C can reference them; the
   C++ type_e enum above aliases these. */
#define MEMDESC_UNDEFINED	0x0
#define MEMDESC_CONVENTIONAL	0x1
#define MEMDESC_RESERVED	0x2
#define MEMDESC_DEDICATED	0x3
#define MEMDESC_SHARED		0x4
#define MEMDESC_BOOT_SPECIFIC	0xe
#define MEMDESC_ARCH_SPECIFIC	0xf
#define MEMDESC_MAX_TYPE	0x10

/* C forms of the memdesc_t methods (the bitfields are C-visible).
   NOTE the explicit (word_t) casts before the shifts: _low/_high are 54-bit
   bitfields, and C gives such an expression a 54-bit type (so << 10 discards
   the top bits) where C++ uses the declared word_t.  Without the cast these
   silently truncate every address at or above 2^54. */
INLINE word_t memdesc_type (const memdesc_t *self)	{ return self->_type; }
INLINE word_t memdesc_subtype (const memdesc_t *self)	{ return self->_t; }
INLINE bool   memdesc_is_virtual (const memdesc_t *self)	{ return self->_v; }
INLINE addr_t memdesc_low (const memdesc_t *self)	{ return (addr_t) ((word_t) self->_low << 10); }
INLINE addr_t memdesc_high (const memdesc_t *self)	{ return (addr_t) (((word_t) self->_high << 10) + 0x3ff); }
INLINE word_t memdesc_size (const memdesc_t *self)	{ return ((word_t) (self->_high - self->_low + 1) << 10); }



#endif /* !__API__V4__MEMDESC_H__ */
