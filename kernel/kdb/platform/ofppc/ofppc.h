/****************************************************************************
 *
 * Copyright (C) 2002-2003, Karlsruhe University
 *
 * File path:	kdb/platform/ofppc/ofppc.h
 * Description:	OpenFirmware PPC support declarations.
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
 * $Id: ofppc.h,v 1.3 2003/09/24 19:05:20 skoglund Exp $
 *
 ***************************************************************************/

#ifndef __KDB__PLATFORM__OFPPC__OFPPC_H__
#define __KDB__PLATFORM__OFPPC__OFPPC_H__

#if defined(CONFIG_KDB_CONS_OF1275)

#include <sync.h>

#include INC_ARCH(ppc_registers.h)
#include INC_ARCH(pghash.h)

struct of1275_space_t
{
    spinlock_t lock;

    word_t of1275_ptab_loc;
    word_t of1275_segments[16];
    word_t of1275_stack_top;
    word_t of1275_stack_bottom;

    word_t current_ptab_loc;
    word_t current_segments[16];
};
typedef struct of1275_space_t of1275_space_t;

INLINE of1275_space_t *get_of1275_space (void)
{
    extern of1275_space_t of1275_space;
    return &of1275_space;
}

INLINE word_t of1275_space_get_ptab_loc (of1275_space_t *self)
{ return ppc_get_sdr1(); }

INLINE void of1275_space_get_segments (of1275_space_t *self, word_t segments[16])
{
    int i;
    for( i = 0; i < 16; i++ )
	segments[i] = ppc_get_sr(i);
}

/* `stack' is initialised from its own address, which is how it gets a stack
   address to compare -- upstream's, and deliberate. */
INLINE bool of1275_space_using_of1275_stack (of1275_space_t *self)
{
    word_t stack = (word_t)&stack;
    return (stack >= self->of1275_stack_bottom) &&
	   (stack < self->of1275_stack_top);
}

void of1275_space_init (of1275_space_t *self, word_t stack_top, word_t stack_bottom);

word_t of1275_space_execute_of1275 (of1275_space_t *self, word_t (*func)(void *), void *param);

#endif	/* CONFIG_KDB_CONS_OF1275 */

#endif	/* __KDB__PLATFORM__OFPPC__OFPPC_H__ */
