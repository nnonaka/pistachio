/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2006-2008, 2010,  Karlsruhe University
 *                
 * File path:     arch/powerpc/tracebuffer.h
 * Description:   PPC specific tracebuffer
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
#pragma once
#include <tcb_layout.h>
#include INC_ARCH(ppc_registers.h)

#define TRACEBUFFER_SIZE        ( 1024 * 1024)
INLINE void tracerecord_store_arch (tracerecord_t *self, const traceconfig_t config)
{
    /* This port records only the timebase; config is unused, as it was in the
       C++ original. */
    (void) config;
    self->tsc = ppc_get_timebase();
}

INLINE void tracebuffer_initialize (tracebuffer_t *self)
{
    self->magic = TRACEBUFFER_MAGIC;
    /* was `current = 0' -- current is an atomic_t, so this went through
       atomic_t::operator=; x86's C form spells it the same way. */
    atomic_set (&self->current, 0);
    self->mask = TBUF_DEFAULT_MASK;
    self->max = (TRACEBUFFER_SIZE/sizeof(tracerecord_t))-1;
    self->config.raw = 0;
#if defined(CONFIG_SMP)
    self->config.smp = 1;
#endif
#if defined(CONFIG_TBUF_PERFMON)
    self->config.pmon = 1;
#endif
}
