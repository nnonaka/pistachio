/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2007,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/timer.h
 * Description:   Simple RTC timer
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
#ifndef __GLUE__V4_X86__TIMER_H__
#define __GLUE__V4_X86__TIMER_H__

#include <timer.h>
#include INC_API(cpu.h)

#if !defined(CONFIG_X86_TSC)
// In absence of a processor cycle counter we count timer ticks
extern u64_t ticks;
#endif

/* generic_periodic_timer_t is an empty base class, so the layout is just the
   two frequency words. */
struct timer_t {
    word_t bus_freq;
    word_t proc_freq;
};
typedef struct timer_t timer_t;

/* C implementations of the former timer_t methods (defined in timer-apic.c). */
BEGIN_DECLS
void timer_init_global(void);
void timer_init_cpu(timer_t *self, cpuid_t cpu);
END_DECLS

INLINE timer_t * get_timer (void)
{
    extern timer_t timer;
    return &timer;
}
INLINE word_t timer_get_bus_freq (timer_t *self)	{ return self->bus_freq; }
INLINE word_t timer_get_proc_freq (timer_t *self)	{ return self->proc_freq; }

#endif /* !__GLUE__V4_X86__TIMER_H__ */
