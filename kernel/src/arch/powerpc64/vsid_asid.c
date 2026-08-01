/*********************************************************************
 *                
 * Copyright (C) 2003,  National ICT Australia (NICTA)
 *                
 * File path:     arch/powerpc64/vsid_asid.c
 * Description:   PowerPC64 specific reverse lookup ASID management
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
 * $Id: vsid_asid.cc,v 1.5 2004/06/04 03:40:20 cvansch Exp $
 *                
 ********************************************************************/

#include INC_ARCH(vsid_asid.h)
#include <debug.h>

vsid_asid_cache_t vsid_asid_cache;

word_t vsid_asid_cache_alloc (vsid_asid_cache_t *self, space_t *space)
{
    word_t asid = self->first_free << (VSID_REVERSE_SHIFT + 12);  /* SLB entry vsid is shifted by 12 */

    if ( self->first_free < (s64_t)ASID_MAX )
    {
	/* SLB vsid entry format of power4 */
	/* | UNIMPL  | VSID_ASID | ESID | other bits | */
	/* 63       49          40     12            0 */
	self->cache[self->first_free].asid = asid;
	self->cache[self->first_free].space = space;

	/* Find the next free ASID */
	do {
	    self->first_free++;
	} while (( self->cache[self->first_free].asid != ASID_INVALID ) &&
			(self->first_free < (s64_t)ASID_MAX));
    } else {
	/* We need to preempt ASIDs */
	UNIMPLEMENTED();
    }

    return asid;
}

void vsid_asid_cache_release (vsid_asid_cache_t *self, word_t asid)
{
    word_t entry = asid >> (VSID_REVERSE_SHIFT + 12);
    self->cache[ entry ].asid = ASID_INVALID;

    if ( (s64_t)entry < self->first_free )
	self->first_free = entry;
}

