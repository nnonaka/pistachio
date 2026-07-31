/****************************************************************************
 *
 * Copyright (C) 2002-2003,  Karlsruhe University
 *
 * File path:	arch/powerpc64/1275tree.h
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
 * $Id: 1275tree.h,v 1.4 2005/01/18 13:21:46 cvansch Exp $
 *
 ***************************************************************************/

#ifndef __ARCH__POWERPC64__1275TREE_H__
#define __ARCH__POWERPC64__1275TREE_H__


INLINE word_t of1275_align( word_t val )
{
    word_t size = sizeof(word_t);

    if( val % size )
	val = (val + size) & ~(size-1);
    return val;
}


struct of1275_item_t
{
    u32_t len;
    char data[];
};
typedef struct of1275_item_t of1275_item_t;

INLINE of1275_item_t * of1275_item_next (of1275_item_t *self)
{
    return (of1275_item_t *)of1275_align( (word_t)self->data + self->len );
}


struct of1275_device_t
{
    u32_t handle;
    u32_t prop_count;
    u32_t prop_size;
    u32_t len;
    char name[];
};
typedef struct of1275_device_t of1275_device_t;

INLINE of1275_item_t * of1275_device_item_first (of1275_device_t *self)
{
    return (of1275_item_t *)of1275_align( (word_t)self->name + self->len );
}

INLINE char * of1275_device_get_name (of1275_device_t *self)	{ return self->name; }
INLINE u32_t of1275_device_get_handle (of1275_device_t *self)	{ return self->handle; }
INLINE u32_t of1275_device_get_prop_count (of1275_device_t *self) { return self->prop_count; }

INLINE bool of1275_device_is_valid (of1275_device_t *self)	{ return self->handle != 0; }

/* Out of line in arch/powerpc64/1275tree.c.  get_prop was overloaded three
   ways -- by name, by index, and a word-sized convenience wrapper -- which C
   cannot carry, so each keeps the name of what it looks up. */
BEGIN_DECLS
bool of1275_device_get_prop (of1275_device_t *self, const char *name,
			     char **data, u32_t *data_len);
bool of1275_device_get_prop_index (of1275_device_t *self, word_t index,
				   char **name, char **data, u32_t *data_len);
int  of1275_device_get_depth (of1275_device_t *self);
of1275_device_t * of1275_device_next_by_type (of1275_device_t *self,
					      const char *device_type);
END_DECLS

INLINE bool of1275_device_get_prop_word (of1275_device_t *self,
					 const char *name, word_t *data)
{
    u32_t *ptr, len;
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

BEGIN_DECLS
of1275_device_t * of1275_tree_find (of1275_tree_t *self, const char *name);
of1275_device_t * of1275_tree_find_handle (of1275_tree_t *self, word_t handle);
of1275_device_t * of1275_tree_find_device_type (of1275_tree_t *self,
						const char *device_type);
of1275_device_t * of1275_tree_get_parent (of1275_tree_t *self,
					  of1275_device_t *dev);
END_DECLS


INLINE of1275_tree_t *get_of1275_tree (void)
{
    extern of1275_tree_t of1275_tree;
    return &of1275_tree;
}

struct of1275_pci_address
{
    u32_t a_hi;
    u32_t a_mid;
    u32_t a_lo;
} __attribute__((packed));
typedef struct of1275_pci_address of1275_pci_address;


struct of1275_pci_ranges
{
    union {
	struct {
	    of1275_pci_address addr;
	    u32_t phys;
	    u32_t size_hi;
	} pci32;
	struct {
	    of1275_pci_address addr;
	    u32_t phys_hi;
	    u32_t phys_lo;
	    u32_t size_hi;
	    u32_t size_lo;
	} pci64;
    };
} __attribute__((packed));
typedef struct of1275_pci_ranges of1275_pci_ranges;


struct of1275_isa_reg_property
{
    u32_t space;
    u32_t address;
    u32_t size;
} __attribute__((packed));
typedef struct of1275_isa_reg_property of1275_isa_reg_property;


struct of1275_pci_assigned_addresses
{
    struct {
	of1275_pci_address addr;
	u32_t size_hi;
	u32_t size_lo;
    } pci;
} __attribute__((packed));
typedef struct of1275_pci_assigned_addresses of1275_pci_assigned_addresses;


#endif	/* __ARCH__POWERPC64__1275TREE_H__ */
