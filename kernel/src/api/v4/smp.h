/*********************************************************************
 *                
 * Copyright (C) 2002-2003, 2006, 2008-2010,  Karlsruhe University
 *                
 * File path:     api/v4/smp.h
 * Description:   multiprocessor handling
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
 * $Id: smp.h,v 1.14 2006/09/27 14:14:42 stoess Exp $
 *                
 ********************************************************************/
#ifndef __API__V4__SMP_H__
#define __API__V4__SMP_H__

#include INC_API(types.h)
#include INC_API(tcb.h)
#include <generic/linear_ptab.h>

#if defined(__cplusplus)
template<typename T> INLINE T *get_on_cpu(cpuid_t cpu, T *item)
{
#if defined(CONFIG_SMP)
    pgent_t *pgent;
    pgent_t::pgsize_e pgsize;
    space_t *kspace = get_kernel_space();
    bool ret = kspace->lookup_mapping(item, &pgent, &pgsize, cpu);
    
    if (ret)
        return (T *) addr_offset(phys_to_virt((addr_t)pgent->address(kspace, pgsize)),
                                 addr_mask(item, page_mask (pgsize)));
    else 
        return NULL;
    
#else
    return item;
#endif
}
#endif /* __cplusplus */

#if defined(CONFIG_SMP)

#define ON_CONFIG_SMP(x) do { x; } while(0)

BEGIN_DECLS
/**
 * central SMP handler function; should be called in processor_sleep
 * deals with both, sync and async
 */
void process_xcpu_mailbox(void);

/**
 * Architecture specific XCPU trigger function (IPI) processing XCPU
 * mailboxes
 */
void smp_xcpu_trigger(cpuid_t cpu);
END_DECLS


/**********************************************************************
 *
 *                  Asynchronous XCPU handling
 *
 **********************************************************************/

// maximum number of outstanding XCPU requests
#define MAX_MAILBOX_ENTRIES	32

struct cpu_mb_entry_t;
typedef void (*xcpu_handler_t)(struct cpu_mb_entry_t *);

// mailbox entry
struct cpu_mb_entry_t
{
#if defined(__cplusplus)
public:
    void set(xcpu_handler_t handler, tcb_t * tcb,
	     word_t param0, word_t param1, word_t param2)
	{
	    this->handler = handler;
	    this->tcb = tcb;
	    this->param[0] = param0;
	    this->param[1] = param1;
	    this->param[2] = param2;
	}


    void set(xcpu_handler_t handler, tcb_t * tcb,
	     word_t param0, word_t param1, word_t param2, word_t param3,
	     word_t param4, word_t param5, word_t param6, word_t param7)
	{
	    set (handler, tcb, param0, param1, param2);
	    this->param[3] = param3;
	    this->param[4] = param4;
	    this->param[5] = param5;
	    this->param[6] = param6;
	    this->param[7] = param7;
	}

public:
#endif /* __cplusplus */
    xcpu_handler_t handler;
    tcb_t * tcb;
    word_t param[8];
};
typedef struct cpu_mb_entry_t cpu_mb_entry_t;

/* C free-function form of cpu_mb_entry_t::set (5-arg); the C++ method stays. */
INLINE void cpu_mb_entry_set (cpu_mb_entry_t *self, xcpu_handler_t handler,
			      tcb_t *tcb, word_t param0, word_t param1, word_t param2)
{
    self->handler = handler;
    self->tcb = tcb;
    self->param[0] = param0;
    self->param[1] = param1;
    self->param[2] = param2;
}

#if !defined(__cplusplus)
/* C form of the get_on_cpu<T> template: locate a cpu-local object's copy for
   another CPU by walking that CPU's page table. */
INLINE void * get_on_cpu_c (cpuid_t cpu, void *item)
{
#if defined(CONFIG_SMP)
    pgent_t *pgent;
    int pgsize;
    space_t *kspace = get_kernel_space_c ();

    if (space_lookup_mapping (kspace, item, &pgent, &pgsize, cpu))
	return addr_offset (phys_to_virt (pgent_address (pgent, kspace, (word_t) pgsize)),
			    addr_mask (item, page_mask ((word_t) pgsize)));
    return NULL;
#else
    (void) cpu;
    return item;
#endif
}
#endif /* !__cplusplus */

INLINE void cpu_mb_entry_set_many (cpu_mb_entry_t *self, xcpu_handler_t handler,
				   tcb_t *tcb, word_t p0, word_t p1, word_t p2, word_t p3,
				   word_t p4, word_t p5, word_t p6, word_t p7)
{
    self->handler = handler;
    self->tcb = tcb;
    self->param[0] = p0; self->param[1] = p1; self->param[2] = p2; self->param[3] = p3;
    self->param[4] = p4; self->param[5] = p5; self->param[6] = p6; self->param[7] = p7;
}

/**
 * Asynchronous XCPU mailbox
 * currently not very efficient using a spin-lock for the mailbox
 */
struct cpu_mb_t
{
#if defined(__cplusplus)
public:
    void walk_mailbox();
    void dump_mailbox(word_t cpu);
    cpu_mb_entry_t * alloc()
	{
	    lock.lock();
	    if ( ((first_free + 1) % MAX_MAILBOX_ENTRIES) == first_alloc )
	    {
		lock.unlock();
		return NULL;
	    }
	    unsigned idx = first_free;
	    first_free = (first_free + 1) % MAX_MAILBOX_ENTRIES;
	    return &entries[idx];
	}

    void commit (cpu_mb_entry_t * entry)
	{
	    lock.unlock();
	}

    bool enter (xcpu_handler_t handler, tcb_t * tcb,
		word_t param0, word_t param1, word_t param2)
	{
	    cpu_mb_entry_t * entry = alloc();
	    if (!entry) return false;

	    entry->set(handler, tcb, param0, param1, param2);
	    commit(entry);
	    return true;
	}

    bool enter (xcpu_handler_t handler, tcb_t * tcb,
		word_t param0, word_t param1, word_t param2, word_t param3,
		word_t param4, word_t param5, word_t param6, word_t param7)
	{
	    cpu_mb_entry_t * entry = alloc();
	    if (!entry) return false;

	    entry->set (handler, tcb,
			param0, param1, param2, param3,
			param4, param5, param6, param7);
	    commit(entry);
	    return true;
	}

private:
#endif /* __cplusplus */
    unsigned first_alloc;
    unsigned first_free;
    spinlock_t lock;
    cpu_mb_entry_t entries[MAX_MAILBOX_ENTRIES]
    __attribute__((aligned (CACHE_LINE_SIZE)));
} __attribute__ ((aligned (CACHE_LINE_SIZE)));
typedef struct cpu_mb_t cpu_mb_t;

/* C free-function API for the OOL cpu_mb_t methods (defined in smp.c). */
BEGIN_DECLS
void cpu_mb_walk_mailbox(cpu_mb_t *self);
#if !defined(__cplusplus)
/* C forms of cpu_mb_t::alloc / ::commit (the data above is C-visible). */
INLINE cpu_mb_entry_t * cpu_mb_alloc (cpu_mb_t *self)
{
    spinlock_lock (&self->lock);
    if (((self->first_free + 1) % MAX_MAILBOX_ENTRIES) == self->first_alloc)
    {
	spinlock_unlock (&self->lock);
	return NULL;
    }
    unsigned idx = self->first_free;
    self->first_free = (self->first_free + 1) % MAX_MAILBOX_ENTRIES;
    return &self->entries[idx];
}
INLINE void cpu_mb_commit (cpu_mb_t *self, cpu_mb_entry_t *entry)
{ (void) entry; spinlock_unlock (&self->lock); }
#endif
void cpu_mb_dump_mailbox(cpu_mb_t *self, word_t cpu);
END_DECLS

extern cpu_mb_t cpu_mailboxes[];
INLINE cpu_mb_t * get_cpu_mailbox (cpuid_t dst)
{
    ASSERT(dst < CONFIG_SMP_MAX_CPUS);
    return &cpu_mailboxes[dst];
}

#if defined(__cplusplus)
/* C++ method forwarders to the C free functions above; the remaining inline
   helpers, sync_entry_t, and the glue include below are C++-only. */
INLINE void cpu_mb_t::walk_mailbox() { cpu_mb_walk_mailbox(this); }
INLINE void cpu_mb_t::dump_mailbox(word_t cpu) { cpu_mb_dump_mailbox(this, cpu); }

INLINE void xcpu_request(cpuid_t dstcpu, xcpu_handler_t handler,
			 tcb_t * tcb = NULL, 
			 word_t param0 = 0, word_t param1 = 0, 
			 word_t param2 = 0 )
{
    bool entered = get_cpu_mailbox(dstcpu)->enter(handler, tcb, 
						  param0, param1, param2);

    if (!entered)
    {
	printf("CPU %d tcb %t Failing XCPU requests are unimplemented cpu %d ra %x\n", 
	       get_current_cpu(), get_current_tcb(), dstcpu, __builtin_return_address(0));
	get_cpu_mailbox(dstcpu)->dump_mailbox(dstcpu);
	enter_kdebug("BUG");
	spin_forever();
    }

#ifndef CONFIG_SMP_IDLE_POLL
    /* trigger an IPI */
    smp_xcpu_trigger(dstcpu);
#endif
}

INLINE void xcpu_request(cpuid_t dstcpu, xcpu_handler_t handler, tcb_t * tcb,
			 word_t param0, word_t param1, 
			 word_t param2, word_t param3,
			 word_t param4 = 0, word_t param5 = 0,
			 word_t param6 = 0, word_t param7 = 0)
{
    bool entered =  get_cpu_mailbox(dstcpu)->enter(handler, tcb, 
						   param0, param1, param2, param3,
						   param4, param5, param6, param7);
    
    if (!entered)
    {
	printf("CPU %d Failing XCPU requests are unimplemented cpu %d ra %x\n", 
	       get_current_cpu(), dstcpu, __builtin_return_address(0));
	get_cpu_mailbox(dstcpu)->dump_mailbox(dstcpu);
	enter_kdebug("BUG");
	spin_forever();
    }
    
    
#ifndef CONFIG_SMP_IDLE_POLL
    /* trigger an IPI */
    smp_xcpu_trigger(dstcpu);
#endif
}
#endif /* __cplusplus */


/**********************************************************************
 *
 *                   Synchronous XCPU handling
 *
 **********************************************************************/
#if defined(CONFIG_SMP_SYNC_REQUEST)

/*
 * synchronous XCPU request handling, depends on the hardware
 * architecture. Needed e.g. on IA32 for TLB shoot-downs. See
 * api/v4/smp.c for a detailed description.
 *
 * sync_entry_t: single inheritance from cpu_mb_entry_t; its methods are C
 * free functions (no C++ callers) defined in glue smp.h and smp.c.
 */
#if defined(__cplusplus)
struct sync_entry_t : public cpu_mb_entry_t
{
    word_t pending_mask;
    word_t ack_mask;
};
#else
struct sync_entry_t
{
    cpu_mb_entry_t	base;	/* single inheritance -> base as first member */
    word_t pending_mask;
    word_t ack_mask;
};
#endif
typedef struct sync_entry_t sync_entry_t;

BEGIN_DECLS
void sync_entry_handle_sync_requests(sync_entry_t *self);
#if defined(__cplusplus)
void sync_xcpu_request(cpuid_t dstcpu, xcpu_handler_t handler,
		       tcb_t * tcb = NULL, word_t param0 = 0,
		       word_t param1 = 0, word_t param2 = 0);
#else
void sync_xcpu_request(cpuid_t dstcpu, xcpu_handler_t handler,
		       tcb_t * tcb, word_t param0, word_t param1, word_t param2);
#endif
/* C wrapper for xcpu_request's first overload (defined in schedule.cc). */
void xcpu_request_c (cpuid_t dstcpu, xcpu_handler_t handler, tcb_t * tcb, word_t param0);
void xcpu_request_many (cpuid_t dstcpu, xcpu_handler_t handler, tcb_t * tcb, word_t p0, word_t p1, word_t p2, word_t p3);
void xcpu_request7 (cpuid_t dstcpu, xcpu_handler_t handler, tcb_t * tcb, word_t p0, word_t p1, word_t p2, word_t p3, word_t p4, word_t p5, word_t p6);
END_DECLS

#endif /* CONFIG_SMP_SYNC_REQUEST */

#include INC_GLUE(smp.h)

#else /* ! CONFIG_SMP */

#define ON_CONFIG_SMP(x) do { } while(0)

#endif /* CONFIG_SMP */

#endif /* !__API__V4__SMP_H__ */
