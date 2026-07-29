/*********************************************************************
 *                
 * Copyright (C) 2006, 2008,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/x64/x32comp/types.h
 * Description:   32-bit twins of the V4 API types
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
 * $Id: types.h,v 1.2 2006/10/20 16:18:38 reichelt Exp $
 *                
 ********************************************************************/
#ifndef __GLUE__V4_X86__X64__X32COMP__TYPES_H__
#define __GLUE__V4_X86__X64__X32COMP__TYPES_H__

#include INC_API(types.h)

#undef TIME_BITS_WORD
#define TIME_BITS_WORD 32

/*
 * The second copy of api/v4/types.h, with a 32-bit word.  x32-names.h renames
 * everything it declares out of the way of the 64-bit copy already included
 * above; see that header for how this stands in for `namespace x32'.
 */
#include INC_GLUE_SA(x32comp/x32-names.h)

typedef u32_t word_t;
typedef word_t addr_t;

#undef __API__V4__TYPES_H__
#include INC_API(types.h)

#define X32_UNRENAME
#include INC_GLUE_SA(x32comp/x32-names.h)
#undef X32_UNRENAME

INLINE x32_time_t time_32 (time_t t)
{
    x32_time_t r;
    r.raw = t.raw;
    return r;
}

INLINE time_t time_64 (x32_time_t t)
{
    time_t r;
    r.raw = t.raw;
    return r;
}

INLINE x32_timeout_t timeout_32 (timeout_t t)
{
    x32_timeout_t r;
    r.raw = (x32_word_t) t.raw;
    return r;
}

INLINE timeout_t timeout_64 (x32_timeout_t t)
{
    timeout_t r;
    r.raw = t.raw;
    return r;
}

#undef TIME_BITS_WORD


#endif /* !__GLUE__V4_X86__X64__X32COMP__TYPES_H__ */
