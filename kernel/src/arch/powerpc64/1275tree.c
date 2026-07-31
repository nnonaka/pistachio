/****************************************************************************
 *
 * Copyright (C) 2002-2003, Karlsruhe University
 *
 * File path:	arch/powerpc64/1275tree.c
 * Description:	Functions which enable easy access to the position-independent
 * 		Open Firmware device tree.
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
 * $Id: 1275tree.c,v 1.3 2005/01/18 13:32:35 cvansch Exp $
 *
 ***************************************************************************/

#include INC_ARCH(string.h)
#include INC_ARCH(1275tree.h)

of1275_tree_t of1275_tree;

int of1275_device_get_depth (of1275_device_t *self)
{
    int depth = 0;
    char *c = self->name;

    while( *c )
    {
	if( *c == '/' )
	    depth++;
	c++;
    }
    return depth;
}

#include <debug.h>
bool of1275_device_get_prop (of1275_device_t *self, const char *name,
		char **data, u32_t *data_len)
{
    of1275_item_t *item_name, *item_data;

    item_name = of1275_device_item_first (self);
    item_data = of1275_item_next (item_name);

    {
    word_t i;
    for( i = 0; i < of1275_device_get_prop_count (self); i++ )
    {
	if( !strcmp(item_name->data, name) )
	{
	    *data = item_data->data;
	    *data_len = item_data->len;
	    return true;
	}
	item_name = of1275_item_next (item_data);
	item_data = of1275_item_next (item_name);
    }
    }

    return false;
}

bool of1275_device_get_prop_index (of1275_device_t *self, word_t index,
	char **name, char **data, u32_t *data_len)
{
    of1275_item_t *item_name, *item_data;

    if( index >= of1275_device_get_prop_count (self) )
	return false;

    item_name = of1275_device_item_first (self);
    item_data = of1275_item_next (item_name);

    {
    word_t i;
    for( i = 0; i < index; i++ )
    {
	item_name = of1275_item_next (item_data);
	item_data = of1275_item_next (item_name);
    }
    }

    *name = item_name->data;
    *data = item_data->data;
    *data_len = item_data->len;
    return true;
}

of1275_device_t * of1275_tree_find (of1275_tree_t *self, const char *name)
{
    of1275_device_t *dev = of1275_tree_first (self);
    if( !dev )
	return NULL;

    while( of1275_device_is_valid (dev) )
    {
	if( !strcmp_of(of1275_device_get_name (dev), name) )
	    return dev;
	dev = of1275_device_next (dev);
    }

    return NULL;
}

of1275_device_t * of1275_tree_find_handle (of1275_tree_t *self, word_t handle)
{
    of1275_device_t *dev = of1275_tree_first (self);
    if( !dev )
	return NULL;

    while( of1275_device_is_valid (dev) )
    {
	if( of1275_device_get_handle (dev) == handle )
	    return dev;
	dev = of1275_device_next (dev);
    }

    return NULL;
}

of1275_device_t * of1275_tree_get_parent (of1275_tree_t *self, of1275_device_t *dev)
{
    char *slash = NULL;
    int cnt, depth;

    if( !dev || !of1275_tree_first (self) )
	return NULL;

    // Do we have any parents?
    depth = of1275_device_get_depth (dev);
    if( depth <= 1 )
	return NULL;

    // Locate the last slash in the name.
    {
    char *c;
    for( c = of1275_device_get_name (dev); *c; c++ )
	if( *c == '/' )
	    slash = c;
    }
    if( slash == NULL )
	return NULL;

    // Count the offset of the last slash.
    cnt = 0;
    {
    char *c;
    for( c = of1275_device_get_name (dev); c != slash; c++ )
	cnt++;
    }

    // Search for the parent node.
    {
    of1275_device_t *parent = of1275_tree_first (self);
    while( of1275_device_is_valid (parent) )
    {
	if( !strncmp(of1275_device_get_name (parent), of1275_device_get_name (dev), cnt) )
	    if( of1275_device_get_depth (parent) == (depth-1) )
		return parent;
	parent = of1275_device_next (parent);
    }
    }

    return NULL;
}

of1275_device_t * of1275_tree_find_device_type (of1275_tree_t *self, const char *device_type)
{
    of1275_device_t *dev;
    u32_t len;
    char *type;

    dev = of1275_tree_first (self);
    if( !dev )
	return NULL;

    while( of1275_device_is_valid (dev) )
    {
	if( of1275_device_get_prop (dev, "device_type", &type, &len) )
	    if( !strcmp(type, device_type) )
		return dev;
	dev = of1275_device_next (dev);
    }

    return NULL;
}

of1275_device_t * of1275_device_next_by_type (of1275_device_t *self, const char *device_type)
{
    of1275_device_t *dev = self;
    u32_t len;
    char *type;

    if( !dev )
	return NULL;

    while( of1275_device_is_valid (dev) )
    {
	if( of1275_device_get_prop (dev, "device_type", &type, &len) )
	    if( !strcmp(type, device_type) )
		return dev;
	dev = of1275_device_next (dev);
    }
    return NULL;
}
