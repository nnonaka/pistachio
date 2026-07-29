/*********************************************************************
 *                
 * Copyright (C) 2003, 2007,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/x32/smallspaces.h
 * Description:   Small space id handling
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
 * $Id: smallspaces.h,v 1.4 2003/09/24 19:04:36 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __GLUE_V4_X86__X32__SMALLSPACES_H__
#define __GLUE_V4_X86__X32__SMALLSPACES_H__

#include INC_GLUE(config.h)

struct space_t;
typedef struct space_t space_t;

struct smallspace_id_t
{
    union {
	u32_t	raw;
	u8_t	id;
    };
};
typedef struct smallspace_id_t smallspace_id_t;

/**
 * Check whether small space id indicates a small space or not.
 * @return true if space id indicates small space, false otherwise
 */
INLINE bool smallspace_id_is_small (smallspace_id_t *self)
{
    return self->id != 0;
}

/**
 * Set small space id to indicate large address space.
 */
INLINE void smallspace_id_set_large (smallspace_id_t *self)
{
    self->raw = 0;
}

/**
 * Set small space id to indicate small address space.
 * @param idx	index into small space area (4MB stepping)
 * @param size	size of small space in megabytes
 */
INLINE void smallspace_id_set_small (smallspace_id_t *self, word_t idx, word_t size)
{
    self->id = (u8_t) (((idx & ~(size - 1)) >> 1) | (size >> 2));
}

/**
 * Get size of small space (in bytes).
 * @return size of small space (in bytes)
 */
INLINE word_t smallspace_id_size (smallspace_id_t *self)
{
    word_t size;
    word_t mask;

    if (self->id == 0)
	return 0;

    size = (1UL << 22);
    for (mask = 1; (self->id & mask) == 0; mask <<= 1, size <<= 1)
	;
    return size;
}

/**
 * Get offset of small space within small space area (in bytes).
 * @return offset of small space (in bytes)
 */
INLINE word_t smallspace_id_offset (smallspace_id_t *self)
{
    word_t mask;

    if (self->id == 0)
	return 0;

    mask = 1;
    for (; (self->id & mask) == 0; mask <<= 1)
	;
    return (word_t) (self->id & ~mask) << 21;
}

INLINE void   smallspace_id_set_raw (smallspace_id_t *self, word_t r) { self->raw = (u32_t) r; }
INLINE word_t smallspace_id_get_raw (smallspace_id_t *self) { return self->raw; }


bool is_smallspace(space_t *s);


#endif /* !__GLUE_V4_X86__X32__SMALLSPACES_H__ */
