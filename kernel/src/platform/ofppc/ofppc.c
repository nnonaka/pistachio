/****************************************************************************
 *
 * Copyright (C) 2002-2003, Karlsruhe University
 *
 * File path:	platform/ofppc/ofppc.c
 * Description:	Open Firmware PowerPC support
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
 * $Id: ofppc.c,v 1.4 2003/10/31 16:04:41 joshua Exp $
 *
 ***************************************************************************/

#include <debug.h>

#include INC_ARCH(string.h)
#include INC_PLAT(1275tree.h)

SECTION(".init") bool ofppc_get_cpu_speed( word_t *cpu_hz, word_t *bus_hz )
    /* Return the clock speed and bus speed of the primary CPU.
     */
{
    of1275_device_t *dev;

    // First look for the device pointed to by the /chosen/cpu property.
    dev = of1275_tree_find (get_of1275_tree(), "/chosen");
    if( dev )
    {
	word_t handle;
	if( of1275_device_get_prop_word (dev, "cpu", &handle) )
	    dev = of1275_tree_find_handle (get_of1275_tree(), handle);
	else
	    dev = NULL;
    }

    if( dev == NULL )
    	dev = of1275_tree_find (get_of1275_tree(), "/cpus/cpu@0"); // Try a fallback.
    if( dev == NULL )
	return false;

    if( !of1275_device_get_prop_word (dev, "clock-frequency", cpu_hz) )
	return false;
    if( !of1275_device_get_prop_word (dev, "bus-frequency", bus_hz) )
	return false;

    return true;
}

SECTION(".init") int ofppc_get_cpu_count( void )
{
    of1275_device_t *dev;
    int cnt = 0;
    char token[] = "/cpus/";

    dev = of1275_tree_first (get_of1275_tree());
    if( dev == NULL )
	return 1;

    // Search through every device, looking for those which are in the /cpus
    // tree.  Count those which are immediate children of /cpus.
    while( of1275_device_is_valid (dev) )
    {
	if( !strncmp(of1275_device_get_name (dev), token, sizeof(token)-1) )
	    if( of1275_device_get_depth (dev) == 2 )
	    {
		TRACE_INIT( "Found cpu: %s\n", of1275_device_get_name (dev) );
		cnt++;
	    }
	dev = of1275_device_next (dev);
    }

    if( !cnt )
	cnt = 1;
    return cnt;
}
