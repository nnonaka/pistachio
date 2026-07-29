/*********************************************************************
 *                
 * Copyright (C) 2002-2008,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/x64/space.h
 * Description:   AMD64 space_t implementation
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
 * $Id: space.h,v 1.20 2006/11/14 18:44:56 skoglund Exp $
 * 
 ********************************************************************/
#ifndef __GLUE_V4_X86__X64__SPACE_H__
#define __GLUE_V4_X86__X64__SPACE_H__

#include <debug.h>
#include INC_API(types.h)
#include INC_API(fpage.h)
#include INC_API(thread.h)
#include INC_ARCH(pgent.h)
#include INC_ARCH(atomic.h)
#include INC_GLUE(config.h)

#if defined(CONFIG_X86_IO_FLEXPAGES)
#include INC_GLUE(io_space.h)
#endif

// Even if new MDB is not used we need the mdb_t::ctrl_t
#include <mdb.h>

/* forward declarations - space_t depends on tcb_t and utcb_t */
struct tcb_t;
typedef struct tcb_t tcb_t;
struct utcb_t;
typedef struct utcb_t utcb_t;

#define PGSIZE_UTCB	X86_PGSIZE_4K
#define PGSIZE_KTCB	X86_PGSIZE_4K
#define PGSIZE_KERNEL	((KERNEL_PAGE_SIZE == X86_SUPERPAGE_SIZE) ? X86_PGSIZE_2M : X86_PGSIZE_4K)
#define PGSIZE_SIGMA    X86_PGSIZE_2M

//translation table (actual declaration in space.cc)
#define TRANSLATION_TABLE_ENTRIES 32
extern struct transTable_t {
	word_t s0addr;
	paddr_t physaddr;
	word_t size;
} transTable[TRANSLATION_TABLE_ENTRIES];
   
struct space_t;
typedef struct space_t space_t;

/*
 * kernel_pdp_t / top_pdir_t were nested in x86_space_t.  Hoisted to
 * top-level structs so x86_space_t's data (which holds a top_pdir_t*) is
 * C-visible; re-aliased with typedefs inside x86_space_t below so
 * x86_space_t::top_pdir_t / sizeof(top_pdir_t) keep working in C++.
 */
struct x86_kernel_pdp_t {
    union {
	x86_pgent_t pdpe[512];
	struct {
	    /* Copy area */
	    x86_pgent_t copy_area[COPY_AREA_COUNT][COPY_AREA_SIZE >> X86_X64_PDP_BITS];
	    /* Kernel area */
	    x86_pgent_t reserved[512 - 6 - COPY_AREA_COUNT * ((COPY_AREA_SIZE >> X86_X64_PDP_BITS))];
	    x86_pgent_t ktcb;
	    x86_pgent_t remap32[4];
	    x86_pgent_t kernel_area;
	} __attribute__((aligned(X86_PTAB_BYTES)));
    };
};
typedef struct x86_kernel_pdp_t x86_kernel_pdp_t;

struct x86_top_pdir_t {
    union {
	pgent_t pgent[512];
	struct {
	    pgent_t user_area[X86_X64_PML4_IDX(USER_AREA_END)];
	    space_t * space; /* space backlink */
	    pgent_t kernel_pdp;
	} __attribute__((aligned(X86_PTAB_BYTES)));
    };
};
typedef struct x86_top_pdir_t x86_top_pdir_t;

/* C forms of the x86_top_pdir_t kernel-pdp accessors (defined in space.cc,
   which stays C++); used by glue/v4-x86/x64/space.c's pgent_smp_sync. */
BEGIN_DECLS
pgent_t *          x86_top_pdir_get_kernel_pdp_pgent (x86_top_pdir_t *self);
x86_kernel_pdp_t * x86_top_pdir_get_kernel_pdp       (x86_top_pdir_t *self);
END_DECLS

/**
 * The address space representation
 */
struct x86_space_t {
/* Hoisted so C (api/v4/space.c) can name the access kinds; the access_e enum
   below aliases these.  access_e is a signed enum (readwrite = -1). */
#define SPACE_ACCESS_READ	0
#define SPACE_ACCESS_WRITE	2
#define SPACE_ACCESS_READWRITE	(-1)
#define SPACE_ACCESS_EXECUTE	16

    struct
    {	
	struct {
	    /* CPU-specific ptabs */
	    x86_top_pdir_t* top_pdir;
	    atomic_t thread_count;
	} cpu_ptab [CONFIG_SMP_MAX_CPUS];
	word_t reference_ptab;
	fpage_t kip_area;
	fpage_t utcb_area;
	atomic_t thread_count;
#if defined(CONFIG_X86_IO_FLEXPAGES)
	io_space_t *io_space;
#endif
#if defined(CONFIG_X86_COMPATIBILITY_MODE)
	word_t compatibility_mode;
#endif		    
    } data;


} __attribute__((aligned(X86_PTAB_BYTES)));
typedef struct x86_space_t x86_space_t;

#if defined(CONFIG_X86_COMPATIBILITY_MODE)
/* space_t derives from x86_space_t (glue/v4-x86/space.h), which is not yet
   complete here, so this takes the base and the callers pass &space->base. */
INLINE bool x86_space_is_compatibility_mode (x86_space_t *self)
{
    return self->data.compatibility_mode != 0;
}
#endif







#endif /* !__GLUE_V4_X86__X64__SPACE_H__ */
