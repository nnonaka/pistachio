/*********************************************************************
 *                
 * Copyright (C) 2007-2008, 2010,  Karlsruhe University
 *                
 * File path:     kdb/tracebuffer.h
 * Description:   Kernel tracebuffer facility
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
 * $Id: tracebuffer.h,v 1.3 2007/02/05 14:44:05 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __KDB__TRACEBUFFER_H__
#define __KDB__TRACEBUFFER_H__


#if defined(CONFIG_TRACEBUFFER)
#include INC_API(cpu.h)
#include INC_ARCH(atomic.h)

typedef u16_t cpuid_t;

#define TB_DEFAULT				(1 << 0)
#define TB_USERID_START				(100)

#if defined(CONFIG_IS_32BIT)
#define TRACEBUFFER_MAGIC          0x1464b123
#else
#define TRACEBUFFER_MAGIC          0x1464b123acebf
#endif

union traceconfig_t
{
    struct {
        word_t              smp      : 1; // SMP 
        word_t              pmon     : 1; // Enable perf monitoring
        word_t              pmon_cpu : 2; // CPU: x86: 00=P2/P3/K8, 01=P4
        word_t              pmon_e   : 2; // Enable energy monitoring
        word_t                       : BITS_WORD-6;
    };
    word_t raw;
};
typedef union traceconfig_t traceconfig_t;


/**
 * A tracebuffer record indicates the type of event, the time of the
 * event, the current thread, a number of event specific parameters,
 * and potentially the current performance counters.
 */
#define TRACERECORD_NUM_ARGS 9

/* The operations on a record are free functions below; tracerecord_store_arch
   is arch-specific and comes with the arch header included at the bottom of
   this file. */
struct tracerecord_t
{
    struct {
        word_t          utype   : 16;
        word_t          ktype   : 16;
        word_t                  : BITS_WORD-32;
        word_t          cpu     : 16;
        word_t          id      : 16;
        word_t                  : BITS_WORD-32;
    };
    word_t              tsc;
    word_t              thread;
    word_t              pmc0;
    word_t              pmc1;
    const char *        str;
    word_t              arg[TRACERECORD_NUM_ARGS];
};
typedef struct tracerecord_t tracerecord_t;

INLINE bool tracerecord_is_kernel_event (tracerecord_t *self)
{ return self->ktype && ! self->utype; }
INLINE word_t tracerecord_get_type (tracerecord_t *self)
{ return (self->utype == 0) ? self->ktype : self->utype; }


/**
 * The tracebuffer is a region in memory accessible by kernel and user
 * through the FS segment.  The first words of the region indicate the
 * current tracebuffer record and which events should be recorded.
 * The remaining part of the region hold the counters and the
 * tracebuffer records.
 */
/* tracebuffer_next_record and tracebuffer_initialize need store_arch, so they
   live after the arch include at the bottom of this file. */
struct tracebuffer_t
{
    word_t        magic;
    atomic_t      current;
    word_t        mask;
    word_t        max;
    traceconfig_t config;
    word_t        __pad[3];
    word_t        counters[8];
    tracerecord_t tracerecords[];
};
typedef struct tracebuffer_t tracebuffer_t;

#define TRACEBUFFER_OFS_COUNTERS  (sizeof (word_t) * 8)

INLINE bool tracebuffer_is_valid (tracebuffer_t *self)
{ return self->magic == TRACEBUFFER_MAGIC; }

INLINE void tracebuffer_increase_counter (tracebuffer_t *self, word_t ctr)
{ self->counters[ctr & 0x7]++; }

INLINE void tracebuffer_store_string (tracebuffer_t *self, const char *str)
{ self->tracerecords[atomic_read (&self->current)].str = str; }

INLINE void tracebuffer_store_data (tracebuffer_t *self, word_t offset, word_t item)
{ self->tracerecords[atomic_read (&self->current)].arg[offset] = item; }

INLINE tracebuffer_t * get_tracebuffer (void)
{
    extern tracebuffer_t * tracebuffer;
    return tracebuffer;
}

/*
 * Wrap tracepoint events with event type arguments
 */

extern void tbuf_dump (word_t count, word_t usec, word_t tp_id, word_t cpumask);

#define DEBUG_KERNEL_DETAILS
#if defined(DEBUG_KERNEL_DETAILS)
#define TBUF_DEFAULT_MASK			(0xFFFFFFFF)
#else
#define TBUF_DEFAULT_MASK			(0xFFFFFFFF & ~(TP_DETAIL << 16))
#endif

#include INC_ARCH(tracebuffer.h)

/* These need tracerecord_store_arch, defined by the arch header above. */
INLINE void tracerecord_store_record (tracerecord_t *self, traceconfig_t config,
				      word_t type, word_t id)
{
    /* Store type, cpu, id, thread, counters */
    self->ktype = type & 0xffff;
    self->utype = 0;
    self->id = id & 0xffff;
    self->cpu = get_current_cpu();
    self->thread = (word_t) __builtin_frame_address(0);
    tracerecord_store_arch (self, config);
}

INLINE bool tracebuffer_next_record (tracebuffer_t *self, word_t type, word_t id)
{
    /* Check wheter to filter the event */
    if ((self->mask & ((type & 0xffff) << 16)) == 0)
	return false;

    atomic_inc (&self->current);
    if (atomic_read (&self->current) == self->max)
	atomic_set (&self->current, 0);

    /* Store type, cpu, id, thread, counters */
    tracerecord_store_record (&self->tracerecords[atomic_read (&self->current)],
			      self->config, type, id);

    return true;
}

#include <stdarg.h>	/* for va_list, ... comes with gcc */

#define tbuf_record_event(type, tpid, str, args...)			\
    __tbuf_record_event(type, tpid, str, ##args, TRACEBUFFER_MAGIC);

INLINE void __tbuf_record_event(word_t type, word_t tpid, const char *str, ...)
{

    va_list args;
    word_t arg;
    
    /* The buffer is only allocated once setup_tracebuffer() has run.  This
       used to be checked as "!this" inside next_record(), which is undefined
       behaviour -- the compiler is free to assume "this" is never null. */
    tracebuffer_t *tbuf = get_tracebuffer();
    if(!tbuf || !tracebuffer_next_record (tbuf, type, tpid))
        return;
                   
    tracebuffer_store_string (tbuf, str);

    va_start(args, str);
    
    word_t i;
    for (i=0; i < TRACERECORD_NUM_ARGS; i++)
    {
	arg = va_arg(args, word_t);
	if (arg == TRACEBUFFER_MAGIC)
	    break;
	
	tracebuffer_store_data (tbuf, i, arg);
	
    }
    
    va_end(args);
}

void setup_tracebuffer (void);

#else /* !CONFIG_TRACEBUFFER */
#define tbuf_inc_counter(counter)
#define tbuf_record_event(args...)
#endif

# define TBUF_REC_TRACEPOINT(tptype, tpid, str, args...)	\
    tbuf_record_event (tptype, tpid, str, ##args)

#endif /* !__KDB__TRACEBUFFER_H__ */
