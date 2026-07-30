/*********************************************************************
 *
 * Copyright (C) 2006-2007,  Karlsruhe University
 *
 * File path:     glue/v4-ia32/hvm/vtlb.h
 * Description:   Full Virtualization Extensions - Generic VTLB
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
 * $Id$
 *
 ********************************************************************/
#ifndef __GLUE__V4_X86__HVM_VTLB_H__
#define __GLUE__V4_X86__HVM_VTLB_H__

#include INC_ARCH_SA(ptab.h)

struct arch_hvm_ktcb_t;
typedef struct arch_hvm_ktcb_t arch_hvm_ktcb_t;
struct space_t;

/*
 * Was class x86_hvm_vtlb_t.  The methods become x86_hvm_vtlb_* functions taking
 * the receiver first; the two overload pairs -- flush_gphys / flush_gvirt /
 * flush_hpdir with and without an address -- keep the no-argument name and gain
 * an _addr suffix for the one that takes one.
 */
struct x86_hvm_vtlb_t
{
    /* Unpaged host pdir */
    pgent_t *hpdir_nonpaged;
    pgent_t *hpdir_paged;

    /* Current host pdir (shadow page table) */
    pgent_t *hpdir;

    /* Guest pdir ptr */
    pgent_t *gpdir;
    /* Guest physical space */
    struct space_t *space;

    /* Virtual register change flags */
    struct {
	bool wp;
	bool pg;
	bool pe;
    } flags;
};
typedef struct x86_hvm_vtlb_t x86_hvm_vtlb_t;

bool x86_hvm_vtlb_alloc (x86_hvm_vtlb_t *self, struct space_t *space);
void x86_hvm_vtlb_free (x86_hvm_vtlb_t *self);

/* Flush the VTLB or the entry related to an address.  Were private. */
void x86_hvm_vtlb_flush_hpdir (x86_hvm_vtlb_t *self, pgent_t *pdir);
void x86_hvm_vtlb_flush_hpdir_addr (x86_hvm_vtlb_t *self, pgent_t *pdir, addr_t gvaddr);

/* Flush a gphys mapping from all VTLBs. */
INLINE void x86_hvm_vtlb_flush_gphys (x86_hvm_vtlb_t *self)
{ x86_hvm_vtlb_flush_hpdir (self, self->hpdir_paged); x86_hvm_vtlb_flush_hpdir (self, self->hpdir_nonpaged); }
INLINE void x86_hvm_vtlb_flush_gphys_addr (x86_hvm_vtlb_t *self, addr_t gvaddr)
{ x86_hvm_vtlb_flush_hpdir_addr (self, self->hpdir_paged, gvaddr); x86_hvm_vtlb_flush_hpdir_addr (self, self->hpdir_nonpaged, gvaddr); }

/* Flush a gvirt mapping from the current VTLB. */
INLINE void x86_hvm_vtlb_flush_gvirt (x86_hvm_vtlb_t *self)
{ x86_hvm_vtlb_flush_hpdir (self, self->hpdir); }
INLINE void x86_hvm_vtlb_flush_gvirt_addr (x86_hvm_vtlb_t *self, addr_t gvaddr)
{ x86_hvm_vtlb_flush_hpdir_addr (self, self->hpdir, gvaddr); }


INLINE word_t x86_hvm_vtlb_get_active_top_pdir (x86_hvm_vtlb_t *self)
{ return virt_to_phys ((word_t) self->hpdir); }

INLINE void x86_hvm_vtlb_set_guest_top_pdir (x86_hvm_vtlb_t *self, pgent_t *pdir)
{ self->gpdir = pdir; }

INLINE pgent_t * x86_hvm_vtlb_get_guest_top_pdir (x86_hvm_vtlb_t *self)
{ return self->gpdir; }

INLINE void x86_hvm_vtlb_set_pe (x86_hvm_vtlb_t *self, bool pe)
{ self->flags.pe = pe; self->hpdir = pe ? self->hpdir_paged : self->hpdir_nonpaged; }
INLINE void x86_hvm_vtlb_set_wp (x86_hvm_vtlb_t *self, bool wp) { self->flags.wp = wp; }
INLINE void x86_hvm_vtlb_set_pg (x86_hvm_vtlb_t *self, bool pg) { self->flags.pg = pg; }

/* Called on a VTLB miss. */
bool x86_hvm_vtlb_handle_vtlb_miss (x86_hvm_vtlb_t *self, addr_t gvaddr, word_t access);

/* Lookup guest-virtual memory. */
bool x86_hvm_vtlb_lookup_gphys_addr (x86_hvm_vtlb_t *self, addr_t gvaddr, addr_t *gpaddr);

/* Lookup guest-virtual memory and dump corresponding ptab entry */
bool x86_hvm_vtlb_dump_ptab_entry (x86_hvm_vtlb_t *self, addr_t gvaddr);


#endif /* !__GLUE__V4_X86__HVM_VTLB_H__ */
