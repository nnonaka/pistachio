/****************************************************************************
 *
 * Copyright (C) 2002-2003, Karlsruhe University
 *
 * File path:	platform/ofppc/1275tree.h
 * Description:	Macros and data types for enabling easy access to the 
 *		position-independent Open Firmware device tree.
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
 * $Id: 1275tree.h,v 1.5 2003/09/24 19:04:58 skoglund Exp $
 *
 ***************************************************************************/

#ifndef __PLATFORM__OFPPC__1275TREE_H__
#define __PLATFORM__OFPPC__1275TREE_H__

#define OF1275_KIP_TYPE		0xe
#define OF1275_KIP_SUBTYPE	0xf

INLINE word_t of1275_align( word_t val )
{
    word_t size = sizeof(word_t);

    if( val % size )
	val = (val + size) & ~(size-1);
    return val;
}


/* `char data[]' / `char name[]' are flexible array members, which C requires
   to follow at least one named member -- they do here, so both structs port
   unchanged. */
struct of1275_item_t
{
    word_t len;
    char data[];
};
typedef struct of1275_item_t of1275_item_t;

INLINE of1275_item_t * of1275_item_next (of1275_item_t *self)
{
    return (of1275_item_t *)of1275_align( (word_t)self->data + self->len );
}


struct of1275_device_t
{
    word_t handle;
    word_t prop_count;
    word_t prop_size;
    word_t len;
    char name[];
};
typedef struct of1275_device_t of1275_device_t;

INLINE of1275_item_t * of1275_device_item_first (of1275_device_t *self)
{
    return (of1275_item_t *)of1275_align( (word_t)self->name + self->len );
}

INLINE char * of1275_device_get_name (of1275_device_t *self)
{ return self->name; }
INLINE word_t of1275_device_get_handle (of1275_device_t *self)
{ return self->handle; }
INLINE word_t of1275_device_get_prop_count (of1275_device_t *self)
{ return self->prop_count; }

INLINE bool of1275_device_is_valid (of1275_device_t *self)
{ return self->handle != 0; }

/* get_prop was overloaded three ways upstream.  C has no overloading, and
   collapsing an overload set to its narrowest member is exactly the failure
   §140 chased, so all three keep distinct names rather than one of them
   winning: by property name, by property index, and the word-sized
   convenience wrapper over the first. */
bool of1275_device_get_prop (of1275_device_t *self, const char *name,
			     char **data, word_t *data_len);
bool of1275_device_get_prop_index (of1275_device_t *self, word_t index,
				   char **name, char **data, word_t *data_len);
int  of1275_device_get_depth (of1275_device_t *self);

INLINE bool of1275_device_get_prop_word (of1275_device_t *self,
					 const char *name, word_t *data)
{
    word_t *ptr, len;
    if( !of1275_device_get_prop (self, name, (char **)&ptr, &len) )
	return false;
    if( len != sizeof(*data) )
	return false;
    *data = *ptr;
    return true;
}

INLINE of1275_device_t * of1275_device_next (of1275_device_t *self)
{
    return (of1275_device_t *)
	of1275_align( (word_t)self->name + self->len + self->prop_size );
}


struct of1275_tree_t
{
    of1275_device_t *head;
};
typedef struct of1275_tree_t of1275_tree_t;

INLINE void of1275_tree_init (of1275_tree_t *self, char *spill)
{
    self->head = (of1275_device_t *)of1275_align( (word_t)spill );
}

INLINE of1275_device_t * of1275_tree_first (of1275_tree_t *self)
{
    return self->head;
}

of1275_device_t * of1275_tree_find (of1275_tree_t *self, const char *name);
of1275_device_t * of1275_tree_find_handle (of1275_tree_t *self, word_t handle);
of1275_device_t * of1275_tree_find_device_type (of1275_tree_t *self,
						const char *device_type);
of1275_device_t * of1275_tree_get_parent (of1275_tree_t *self,
					  of1275_device_t *dev);

/* Ten 32-bit files call get_of1275_tree -- platform/ofppc, ofpower3, ofg5,
   their kdb halves and glue/v4-powerpc/init -- but upstream declares it only
   in arch/powerpc64/1275tree.h, which INC_ARCH cannot reach from a 32-bit
   build.  None of them has ever compiled.  The object it returns is defined at
   file scope in 1275tree.c, so the accessor admits no behavioural choice; it is
   the powerpc64 one, verbatim.  Notes §144. */
INLINE of1275_tree_t * get_of1275_tree (void)
{
    extern of1275_tree_t of1275_tree;
    return &of1275_tree;
}


#endif	/* __PLATFORM__OFPPC__1275TREE_H__ */
