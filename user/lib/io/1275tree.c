/****************************************************************************
 *
 * Copyright (C) 2002, Karlsruhe University
 *
 * File path:	lib/io/1275tree.c
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
 * $Id: 1275tree.cc,v 1.1 2004/01/16 11:23:56 joshua Exp $
 *
 ***************************************************************************/

#include <config.h>

#if defined(CONFIG_COMPORT)
#include "1275tree.h"
#include "lib.h"

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

bool of1275_device_get_prop (of1275_device_t *self, const char *prop_name,
			     char **data, L4_Word_t *data_len)
{
    of1275_item_t *item_name, *item_data;
    L4_Word_t i;

    item_name = of1275_device_item_first (self);
    item_data = of1275_item_next (item_name);

    for( i = 0; i < of1275_device_get_prop_count (self); i++ )
    {
	if( !strcmp(item_name->data, prop_name) )
	{
	    *data = item_data->data;
	    *data_len = item_data->len;
	    return true;
	}
	item_name = of1275_item_next (item_data);
	item_data = of1275_item_next (item_name);
    }

    return false;
}

bool of1275_device_get_prop_index (of1275_device_t *self, L4_Word_t index,
	char **prop_name, char **data, L4_Word_t *data_len )
{
    of1275_item_t *item_name, *item_data;
    L4_Word_t i;

    if( index >= of1275_device_get_prop_count (self) )
	return false;

    item_name = of1275_device_item_first (self);
    item_data = of1275_item_next (item_name);

    for( i = 0; i < index; i++ )
    {
	item_name = of1275_item_next (item_data);
	item_data = of1275_item_next (item_name);
    }

    *prop_name = item_name->data;
    *data = item_data->data;
    *data_len = item_data->len;
    return true;
}

of1275_device_t * of1275_tree_find (of1275_tree_t *self, const char *name)
{
    of1275_device_t *dev = of1275_tree_first (self);
    if( !dev )
	return 0;

    while( of1275_device_is_valid (dev) )
    {
	if( !strcmp(of1275_device_get_name (dev), name) )
	    return dev;
	dev = of1275_device_next (dev);
    }

    return 0;
}

of1275_device_t * of1275_tree_find_handle (of1275_tree_t *self, L4_Word_t handle)
{
    of1275_device_t *dev = of1275_tree_first (self);
    if( !dev )
	return 0;

    while( of1275_device_is_valid (dev) )
    {
	if( of1275_device_get_handle (dev) == handle )
	    return dev;
	dev = of1275_device_next (dev);
    }

    return 0;
}

of1275_device_t * of1275_tree_get_parent (of1275_tree_t *self, of1275_device_t *dev)
{
    char *slash = 0;
    char *c;
    int cnt, depth;
    of1275_device_t *parent;

    if( !dev || !of1275_tree_first (self) )
	return 0;

    // Do we have any parents?
    depth = of1275_device_get_depth (dev);
    if( depth <= 1 )
	return 0;

    // Locate the last slash in the name.
    for( c = of1275_device_get_name (dev); *c; c++ )
	if( *c == '/' )
	    slash = c;
    if( slash == 0 )
	return 0;

    // Count the offset of the last slash.
    cnt = 0;
    for( c = of1275_device_get_name (dev); c != slash; c++ )
	cnt++;

    // Search for the parent node.
    parent = of1275_tree_first (self);
    while( of1275_device_is_valid (parent) )
    {
	if( !strncmp(of1275_device_get_name (parent), of1275_device_get_name (dev), cnt) )
	    if( of1275_device_get_depth (parent) == (depth-1) )
		return parent;
	parent = of1275_device_next (parent);
    }

    return 0;
}

of1275_device_t * of1275_tree_find_device_type (of1275_tree_t *self, const char *device_type)
{
    of1275_device_t *dev;
    L4_Word_t len;
    char *type;

    dev = of1275_tree_first (self);
    if( !dev )
	return 0;

    while( of1275_device_is_valid (dev) )
    {
	if( of1275_device_get_prop (dev, "device_type", &type, &len) )
	    if( !strcmp(type, device_type) )
		return dev;
	dev = of1275_device_next (dev);
    }

    return 0;
}

#endif	/* !CONFIG_COMPORT */
