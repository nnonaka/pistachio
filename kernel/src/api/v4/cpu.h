/*********************************************************************
 *                
 * Copyright (C) 2003, 2007, 2010,  Karlsruhe University
 *                
 * File path:     api/v4/cpu.h
 * Description:   processor management
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
 * $Id: processor.h,v 1.2 2003/09/24 19:04:24 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __API__V4__CPU_H__
#define __API__V4__CPU_H__

typedef u16_t cpuid_t;
BEGIN_DECLS
void init_cpu(cpuid_t processor, word_t external_freq, word_t internal_freq);
END_DECLS

struct cpu_t {
    word_t id;
#if defined(__cplusplus)
    cpu_t()
	{ id = ~0UL; }

    bool is_valid()
	{ return this->id < ~0UL; }

    void set_id(word_t id)
	{ this->id = id; }

    word_t get_id()
	{ return id; }

    static cpu_t * get(cpuid_t cpuid);
    static bool add_cpu(word_t id);
#endif /* __cplusplus */
};
typedef struct cpu_t cpu_t;

/* The former cpu_t static data members, now plain globals so C can define and
   use them (defined in cpu.c). cpu_descriptors is initialised to invalid ids,
   matching the C++ cpu_t() constructor. */
extern cpu_t  cpu_descriptors[CONFIG_SMP_MAX_CPUS];
extern word_t cpu_count;

/* C free-function accessors; the C++ methods above stay for C++ callers. */
INLINE cpu_t * cpu_get (cpuid_t cpuid)		{ return &cpu_descriptors[cpuid]; }
INLINE word_t  cpu_get_id (cpu_t *self)		{ return self->id; }
INLINE bool    cpu_add_cpu (word_t id)
{
    if (cpu_count >= CONFIG_SMP_MAX_CPUS)
	return false;
    cpu_descriptors[cpu_count++].id = id;
    return true;
}
INLINE void    cpu_set_id (cpu_t *self, word_t id) { self->id = id; }
INLINE bool    cpu_is_valid (cpu_t *self)	{ return self->id < ~0UL; }

#if defined(__cplusplus)
INLINE cpu_t * cpu_t::get (cpuid_t cpuid)	{ return cpu_get(cpuid); }
INLINE bool cpu_t::add_cpu (word_t id)
{
    if (cpu_count >= CONFIG_SMP_MAX_CPUS)
	return false;
    cpu_descriptors[cpu_count++].id = id;
    return true;
}
#endif /* __cplusplus */

INLINE cpuid_t get_current_cpu()
{
    extern cpuid_t current_cpu;
    return current_cpu;
}


#endif /* !__API__V4__CPU_H__ */
