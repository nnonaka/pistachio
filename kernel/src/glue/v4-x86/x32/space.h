/*********************************************************************
 *                
 * Copyright (C) 2002, 2004-2008, 2010,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/x32/space.h
 * Description:   ia32-specific space implementation
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
 * $Id: space.h,v 1.55 2007/01/08 14:15:59 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __GLUE_V4_X86__X32__SPACE_H__
#define __GLUE_V4_X86__X32__SPACE_H__


#include <debug.h>
#include INC_API(types.h)
#include INC_API(fpage.h)
#include INC_API(thread.h)
#include INC_ARCH(atomic.h)
#include INC_ARCH(pgent.h)
#include INC_GLUE(config.h)


#if defined(CONFIG_X86_SMALL_SPACES)
#include INC_ARCH(segdesc.h)
#include INC_GLUE_SA(smallspaces.h)
#endif

#if defined(CONFIG_X86_IO_FLEXPAGES)
#include INC_GLUE(io_space.h)
#endif

#if defined(CONFIG_X_X86_HVM)
#include INC_GLUE(hvm-space.h)
#endif

// Even if new MDB is not used we need the mdb_t::ctrl_t
#include <mdb.h>

//translation table (actual declaration in space.cc)
#define TRANSLATION_TABLE_ENTRIES 32
extern struct transTable_t {
	word_t s0addr;
	paddr_t physaddr;
	word_t size;
} transTable[TRANSLATION_TABLE_ENTRIES];

#define PGSIZE_KTCB	X86_PGSIZE_4K
#define PGSIZE_UTCB	X86_PGSIZE_4K
#define PGSIZE_KERNEL	((KERNEL_PAGE_SIZE == X86_SUPERPAGE_SIZE) ? X86_PGSIZE_4M : X86_PGSIZE_4K)
#define PGSIZE_KIP	X86_PGSIZE_4K
#define PGSIZE_SIGMA    PGSIZE_KERNEL

/* forward declarations - space_t depends on tcb_t and utcb_t */
struct tcb_t;
typedef struct tcb_t tcb_t;
struct utcb_t;
typedef struct utcb_t utcb_t;

struct space_t;
typedef struct space_t space_t;

/* Hoisted so C can name the access kinds; access_e was a signed enum
   (readwrite = -1).  Mirrors the x64 header. */
#define SPACE_ACCESS_READ	0
#define SPACE_ACCESS_WRITE	2
#define SPACE_ACCESS_READWRITE	(-1)
#define SPACE_ACCESS_EXECUTE	0

/*
 * top_pdir_t was nested in x86_space_t.  Hoisted to a top-level struct so
 * x86_space_t's data (which holds a top_pdir_t*) is C-visible, exactly as
 * x64/space.h does with its own.
 */
struct x86_top_pdir_t {
    union {
	pgent_t pgent[1024];
	struct {
	    x86_pgent_t user[USER_AREA_END >> X86_X32_PDIR_BITS];
	    x86_pgent_t small[SMALLSPACE_AREA_SIZE >> X86_X32_PDIR_BITS];
	    x86_pgent_t copy_area[COPY_AREA_COUNT][COPY_AREA_SIZE >> X86_X32_PDIR_BITS];
	    x86_pgent_t readmem_area[MEMREAD_AREA_SIZE >> X86_X32_PDIR_BITS];
	    space_t * space; /* back link ptr, "automagically" invalid */
	    /* the rest, e.g., TSS, APIC_MAPPINGS, ... */
	};
    };
};
typedef struct x86_top_pdir_t x86_top_pdir_t;

/**
 * The address space representation
 */
struct x86_space_t {
    /* Shadow pagetable */
    pgent_t user_pgent[USER_AREA_END >> X86_X32_PDIR_BITS];
    struct {
	/* CPU-specific ptabs */
	struct {
	    x86_top_pdir_t* top_pdir;
	    atomic_t thread_count;
	} cpu_ptab [CONFIG_SMP_MAX_CPUS];
	cpuid_t reference_ptab;
	/* Administrative data */
	fpage_t kip_area;
	fpage_t utcb_area;
	atomic_t thread_count;
#if defined(CONFIG_X86_IO_FLEXPAGES)
	io_space_t *	io_space;
#endif
#if defined(CONFIG_X86_SMALL_SPACES)
	smallspace_id_t smallid;
	x86_segdesc_t segdesc;
	struct x86_space_t *prev;
	struct x86_space_t *next;
#endif
#if defined(CONFIG_X_X86_HVM)
	x86_hvm_space_t hvm_space;
#endif
    } data;
};
typedef struct x86_space_t x86_space_t;

#endif /* !__GLUE_V4_X86__X32__SPACE_H__ */
