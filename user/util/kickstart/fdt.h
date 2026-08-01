/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     fdt.h
 * Description:   
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
#ifndef __KICKSTART__FDT_H__
#define __KICKSTART__FDT_H__

#include "lib.h"

/* Was six classes, two of them deriving from fdt_node_t.  The derived pair are
   memory overlays whose base contributes exactly one word, so in C that word
   is spelled out as their first member -- the layout is what matters here, and
   an embedded struct would have forced `p->node.tag' at every use.
   See doc/notes/cpp-to-c-migration.md §172. */

struct fdt_reserve_entry_t
{
    L4_Word64_t address;
    L4_Word64_t size;
};
typedef struct fdt_reserve_entry_t fdt_reserve_entry_t;

struct fdt_t;         typedef struct fdt_t fdt_t;
struct fdt_header_t;  typedef struct fdt_header_t fdt_header_t;
struct fdt_property_t;typedef struct fdt_property_t fdt_property_t;

enum {
    fdt_begin_node = 1,
    fdt_end_node = 2,
    fdt_property_node = 3,
};

struct fdt_node_t
{
    L4_Word32_t tag;
};
typedef struct fdt_node_t fdt_node_t;

L4_INLINE bool fdt_node_is_begin_node (fdt_node_t *self)
    { return self->tag == fdt_begin_node; }
L4_INLINE bool fdt_node_is_end_node (fdt_node_t *self)
    { return self->tag == fdt_end_node; }
L4_INLINE bool fdt_node_is_property_node (fdt_node_t *self)
    { return self->tag == fdt_property_node; }

struct fdt_t
{
    L4_Word32_t magic;
    L4_Word32_t size;
    L4_Word32_t offset_dt_struct;	/* offset to structure */
    L4_Word32_t offset_dt_strings;	/* offset to strings */
    L4_Word32_t offset_mem_reserve_map; /* offset to memory map */
    L4_Word32_t version;
    L4_Word32_t last_compatible_version;
    L4_Word32_t boot_cpuid_phys;
    L4_Word32_t dt_string_size;
    L4_Word32_t dt_struct_size;
};

struct fdt_header_t
{
    L4_Word32_t tag;		/* was the fdt_node_t base */
    char name[0];
};

struct fdt_property_t
{
    L4_Word32_t tag;		/* was the fdt_node_t base */
    L4_Word32_t len;
    L4_Word32_t offset_name;
    L4_Word_t data[0];
};

L4_INLINE bool fdt_is_valid (fdt_t *self)
    { return self->magic == 0xd00dfeed; }

L4_INLINE fdt_node_t *fdt_get_root_node (fdt_t *self)
    { return (fdt_node_t*)((L4_Word_t)self + self->offset_dt_struct); }

L4_INLINE int fdt_header_get_size (fdt_header_t *self)
    { return sizeof(fdt_header_t) + (strlen(self->name) + 4) & ~3; }

L4_INLINE int fdt_property_get_size (fdt_property_t *self)
    { return sizeof(fdt_property_t) + (self->len - 1 + 4) & ~3; }

L4_INLINE char *fdt_property_get_name (fdt_property_t *self, fdt_t *fdt)
    { return ((char*)fdt) + fdt->offset_dt_strings + self->offset_name; }

L4_INLINE L4_Word_t fdt_property_get_len (fdt_property_t *self)
    { return self->len; }
L4_INLINE L4_Word_t fdt_property_get_word (fdt_property_t *self, int index)
    { return self->data[index]; }
L4_INLINE L4_Word64_t fdt_property_get_u64 (fdt_property_t *self, int index)
    { return ((L4_Word64_t)self->data[index]) << 32 |
	     ((L4_Word64_t)self->data[index + 1]); }
L4_INLINE char *fdt_property_get_string (fdt_property_t *self)
    { return (char*)self->data; }

/* fdt_t::get_next_node was a template over the two node kinds; both branches
   are `(fdt_node_t*)((L4_Word_t)p + p->get_size())'. */
L4_INLINE fdt_node_t *fdt_next_node_header (fdt_header_t *p)
    { return (fdt_node_t*)((L4_Word_t)p + fdt_header_get_size (p)); }
L4_INLINE fdt_node_t *fdt_next_node_property (fdt_property_t *p)
    { return (fdt_node_t*)((L4_Word_t)p + fdt_property_get_size (p)); }

fdt_property_t *fdt_find_property_node_path (fdt_t *self, char *path);
fdt_property_t *fdt_find_property_node (fdt_t *self, fdt_node_t *node, char *name);
fdt_header_t *fdt_find_subtree_node (fdt_t *self, fdt_node_t *node, char *name);
fdt_header_t *fdt_find_subtree (fdt_t *self, char *name);

void fdt_dump (fdt_t *self);

/* Defined per platform (util/kickstart/powerpc.c).  C++ found it by lookup
   after the point of use; C needs the declaration first. */
fdt_t *get_fdt_ptr (void);


#endif /* !__FDT_H__ */
