/****************************************************************************
 *
 * Copyright (C) 2002, Karlsruhe University
 *
 * File path:	lib/io/1275tree.h
 * Description:	Support for the canonical representation of the
 *              Open Firmware device tree created by the boot loader.
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
 * $Id: 1275tree.h,v 1.1 2004/01/16 11:23:56 joshua Exp $
 *
 ***************************************************************************/
#ifndef __USER__LIB__IO__1275TREE_H__
#define __USER__LIB__IO__1275TREE_H__

#include <l4/types.h>
#include <l4/kip.h>
#include <l4/sigma0.h>

L4_INLINE L4_Word_t of1275_align( L4_Word_t val )
{
    L4_Word_t size = sizeof(L4_Word_t);

    if( val % size )
	val = (val + size) & ~(size-1);
    return val;
}


/* Was three classes.  The names follow the kernel's already-converted copy of
   this same structure (kernel/src/arch/powerpc64/1275tree.h) so the two trees
   agree; get_prop's three overloads split the same way there.
   See doc/notes/cpp-to-c-migration.md §172. */

struct of1275_item_t
{
    L4_Word_t len;
    char data[];
};
typedef struct of1275_item_t of1275_item_t;

L4_INLINE of1275_item_t *of1275_item_next (of1275_item_t *self)
{
    return (of1275_item_t *)
	of1275_align( (L4_Word_t)self->data + self->len );
}


struct of1275_device_t
{
    /* were protected */
    L4_Word_t handle;
    L4_Word_t prop_count;
    L4_Word_t prop_size;
    L4_Word_t len;
    char name[];
};
typedef struct of1275_device_t of1275_device_t;

/* was protected */
L4_INLINE of1275_item_t *of1275_device_item_first (of1275_device_t *self)
{
    return (of1275_item_t *)
	of1275_align( (L4_Word_t)self->name + self->len );
}

L4_INLINE char *of1275_device_get_name (of1275_device_t *self)
    { return self->name; }
L4_INLINE L4_Word_t of1275_device_get_handle (of1275_device_t *self)
    { return self->handle; }
L4_INLINE L4_Word_t of1275_device_get_prop_count (of1275_device_t *self)
    { return self->prop_count; }
L4_INLINE bool of1275_device_is_valid (of1275_device_t *self)
    { return self->handle != 0; }

bool of1275_device_get_prop (of1275_device_t *self, const char *prop_name,
			     char **data, L4_Word_t *data_len);
bool of1275_device_get_prop_index (of1275_device_t *self, L4_Word_t index,
				   char **prop_name, char **data,
				   L4_Word_t *data_len);
int  of1275_device_get_depth (of1275_device_t *self);

L4_INLINE bool of1275_device_get_prop_word (of1275_device_t *self,
					    const char *prop_name,
					    L4_Word_t *data)
{
    L4_Word_t prop_len;
    char *ptr;

    if( !of1275_device_get_prop(self, prop_name, &ptr, &prop_len) )
	return false;
    if( prop_len != sizeof(*data) )
	return false;
    *data = *ptr;
    return true;
}

L4_INLINE of1275_device_t *of1275_device_next (of1275_device_t *self)
{
    return (of1275_device_t *)
	of1275_align( (L4_Word_t)self->name + self->len + self->prop_size );
}


/* of1275_tree_t has no members: the tree *is* the first device, and first()
   was a cast of `this'.  The placeholder byte keeps sizeof() meaningful --
   an empty struct is a GNU extension of size 0. */
struct of1275_tree_t
{
    char __start;
};
typedef struct of1275_tree_t of1275_tree_t;

L4_INLINE of1275_device_t *of1275_tree_first (of1275_tree_t *self)
{
    return (of1275_device_t *)self;
}

of1275_device_t *of1275_tree_find (of1275_tree_t *self, const char *name);
of1275_device_t *of1275_tree_find_handle (of1275_tree_t *self, L4_Word_t handle);
of1275_device_t *of1275_tree_find_device_type (of1275_tree_t *self,
					       const char *device_type);
of1275_device_t *of1275_tree_get_parent (of1275_tree_t *self,
					 of1275_device_t *dev);


#endif	/* __USER__LIB__IO__1275TREE_H__ */
