/*********************************************************************
 *
 * Copyright (C) 2026,  Karlsruhe Institute of Technology
 *
 * File path:     platform/ofppc/platform.h
 * Description:   Platform-neutral CPU queries for the OFPPC platform.
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
 ********************************************************************/
#ifndef __PLATFORM__OFPPC__PLATFORM_H__
#define __PLATFORM__OFPPC__PLATFORM_H__

/* glue/v4-powerpc/init.c includes INC_PLAT(platform.h) unconditionally, but
   d52a5e2 -- the commit that added ppc44x -- created one only for that
   platform.  The same commit rewrote init to call the platform-neutral
   get_cpu_speed/get_cpu_count in place of ofppc_get_cpu_speed and
   ofppc_get_cpu_count, leaving those two defined in ofppc.c with no caller and
   this platform with no header to include.  This file is the missing
   adaptation: the names it defines are the ones init.c asks for, and the
   bodies are the calls d52a5e2 deleted.  Notes §144. */

#include INC_PLAT(ofppc.h)

/* ofppc_get_cpu_speed reports the primary CPU, which is what init.c asked it
   for before the rewrite; the cpu argument the neutral signature carries is
   accordingly unused here. */
INLINE bool get_cpu_speed( word_t cpu, word_t *cpu_hz, word_t *bus_hz )
{
    return ofppc_get_cpu_speed( cpu_hz, bus_hz );
}

INLINE int get_cpu_count( void )
{
    return ofppc_get_cpu_count();
}

#endif /* !__PLATFORM__OFPPC__PLATFORM_H__ */
