/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     platform/ppc44x/fdt.h
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
#ifndef __PLATFORM__PPC44X__FDT_H__
#define __PLATFORM__PPC44X__FDT_H__

#include INC_ARCH(string.h)

struct fdt_reserve_entry_t
{
    u64_t address;
    u64_t size;
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
    u32_t tag;
};
typedef struct fdt_node_t fdt_node_t;

INLINE bool fdt_node_is_begin_node (fdt_node_t *self)	 { return self->tag == fdt_begin_node; }
INLINE bool fdt_node_is_end_node (fdt_node_t *self)	 { return self->tag == fdt_end_node; }
INLINE bool fdt_node_is_property_node (fdt_node_t *self){ return self->tag == fdt_property_node; }

/* fdt_header_t and fdt_property_t derived from fdt_node_t; in C the base is
   the first member, which is layout-identical and keeps the casts valid. */
struct fdt_header_t
{
    fdt_node_t base;
    char name[0];
};

/* The parentheses are written out, not added: `+' binds tighter than `&', so
   the whole sum was already what got masked.  Same value either way -- every
   member of fdt_header_t is a u32_t, so sizeof is a multiple of 4.  Notes
   §140. */
INLINE int fdt_header_get_size (fdt_header_t *self)
{ return (sizeof(fdt_header_t) + (strlen(self->name) + 4)) & ~3; }

struct fdt_t
{
    u32_t magic;
    u32_t size;
    u32_t offset_dt_struct;	/* offset to structure */
    u32_t offset_dt_strings;	/* offset to strings */
    u32_t offset_mem_reserve_map; /* offset to memory map */
    u32_t version;
    u32_t last_compatible_version;
    u32_t boot_cpuid_phys;
    u32_t dt_string_size;
    u32_t dt_struct_size;
};

struct fdt_property_t
{
    fdt_node_t base;
    u32_t len;
    u32_t offset_name;
    u32_t data[0];
};

INLINE int fdt_property_get_size (fdt_property_t *self)
{ return (sizeof(fdt_property_t) + (self->len - 1 + 4)) & ~3; }

INLINE char * fdt_property_get_name (fdt_property_t *self, fdt_t *fdt)
{ return ((char*)fdt) + fdt->offset_dt_strings + self->offset_name; }

INLINE u32_t  fdt_property_get_len (fdt_property_t *self)	{ return self->len; }
INLINE word_t fdt_property_get_word (fdt_property_t *self, int index) { return self->data[index]; }
INLINE u64_t  fdt_property_get_u64 (fdt_property_t *self, int index)
{ return ((u64_t)self->data[index]) << 32 | ((u64_t)self->data[index + 1]); }
INLINE char * fdt_property_get_string (fdt_property_t *self)	{ return (char*)self->data; }

INLINE bool   fdt_is_valid (fdt_t *self)	{ return self->magic == 0xd00dfeed; }
INLINE word_t fdt_get_size (fdt_t *self)	{ return self->size; }

INLINE fdt_node_t * fdt_get_root_node (fdt_t *self)
{ return (fdt_node_t*)((word_t)self + self->offset_dt_struct); }

/* was the template get_next_node<T>(T*), instantiated on the two node kinds. */
INLINE fdt_node_t * fdt_next_after_header (fdt_header_t *p)
{ return (fdt_node_t*)((word_t)p + fdt_header_get_size (p)); }
INLINE fdt_node_t * fdt_next_after_property (fdt_property_t *p)
{ return (fdt_node_t*)((word_t)p + fdt_property_get_size (p)); }

fdt_property_t * fdt_find_property_node_in (fdt_t *self, fdt_node_t *node, char *name);
fdt_header_t *   fdt_find_subtree_node (fdt_t *self, fdt_node_t *node, char *name);
fdt_header_t *   fdt_next_subtree_node (fdt_t *self, fdt_node_t *node, bool cont);
fdt_property_t * fdt_find_property_node (fdt_t *self, char *path);
fdt_header_t *   fdt_find_subtree (fdt_t *self, char *path);
void             fdt_dump (fdt_t *self);

/*
 * fdt_header_t and fdt_property_t derived from fdt_node_t, so C++ converted
 * either to the base implicitly -- including the null pointer, which the
 * standard requires to convert to null.  In C that conversion has to be
 * written; this is it, null check included, and it folds away because the base
 * is at offset zero.  Notes §140.
 */
INLINE fdt_node_t * fdt_header_node (fdt_header_t *self)
{ return self ? &self->base : NULL; }

INLINE fdt_header_t * fdt_find_first_subtree_node (fdt_t *self, fdt_node_t *node)
{ return fdt_next_subtree_node (self, node, false); }
INLINE fdt_header_t * fdt_find_next_subtree_node (fdt_t *self, fdt_header_t *curr)
{ return fdt_next_subtree_node (self, fdt_header_node (curr), true); }

typedef fdt_t dtree_t;
dtree_t *get_dtree();

#endif /* !__PLATFORM__PPC44X__FDT_H__ */
