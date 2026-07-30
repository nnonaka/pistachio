/****************************************************************************
 *
 * Copyright (C) 2002-2003, Karlsruhe University
 *
 * File path:	kdb/platform/ofppc/of1275.cc
 * Description:	Routines for handling the Open Firmware client interface.
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
 * $Id: of1275.cc,v 1.4 2003/09/24 19:05:20 skoglund Exp $
 *
 ***************************************************************************/

#if defined(CONFIG_KDB_CONS_OF1275)

#include <debug.h>
#include <kdb/kdb.h>

#include INC_ARCH(string.h)

#include "ofppc.h"
#include "of1275.h"

DECLARE_CMD( cmd_dump_ci, platform, 'c', "of1275", "Open Firmware client interface" );

CMD(cmd_dump_ci, cg)
{
    printf( "stdout phandle %x\n", get_of1275_ci()->get_stdout() );
    printf( "stdin  phandle %x\n", get_of1275_ci()->get_stdin() );

    return CMD_NOQUIT;
}   


of1275_client_interface_t of1275_ci;

/* `ci' was a protected member called before its own definition; C needs the
   prototype, and the name is taken by the object above, hence _call. */
static word_t of1275_ci_call (of1275_client_interface_t *self, void *params);

void of1275_ci_init (of1275_client_interface_t *self, word_t entry)
{
    self->entry = (of1275_ci_entry_t)entry;
    spinlock_init (&self->ci_lock, 0);

    self->stdout = OF1275_INVALID_PHANDLE;
    self->stdin = OF1275_INVALID_PHANDLE;

    of1275_phandle_t chosen = of1275_ci_find_device (self, "/chosen" );
    if( chosen == OF1275_INVALID_PHANDLE )
	return;

    of1275_ci_get_prop (self, chosen, "stdout", &self->stdout, sizeof(of1275_phandle_t) );
    of1275_ci_get_prop (self, chosen, "stdin", &self->stdin, sizeof(of1275_phandle_t) );
}

static word_t of1275_ci_call (of1275_client_interface_t *self, void *params)
{
    if( self->entry == NULL )
	return (word_t)-1;

    return get_of1275_space()->execute_of1275( self->entry, params );
}

of1275_phandle_t of1275_ci_find_device (of1275_client_interface_t *self, const char *name)
{
    int namelen = strlen(name) + 1;
    
    // Is the request too large?
    if( (sizeof(self->args.find_device) + namelen) > sizeof(self->args.shared) )
	return OF1275_INVALID_PHANDLE;

    spinlock_lock (&self->ci_lock);

    // Install all parameters in the shared data area.
    self->args.find_device.service = "finddevice";
    self->args.find_device.nargs = 1;
    self->args.find_device.nret = 1;
    self->args.find_device.name = self->args.shared + sizeof(self->args.find_device);
    self->args.find_device.phandle = OF1275_INVALID_PHANDLE;
    sstrncpy( self->args.find_device.name, name, namelen );

    // Invoke OF.
    of1275_ci_call (self, &self->args.find_device );

    of1275_phandle_t ret = self->args.find_device.phandle;

    spinlock_unlock (&self->ci_lock);
    return ret;
}

int of1275_ci_get_prop (of1275_client_interface_t *self, of1275_phandle_t phandle,
	const char *name, void *buf, int buflen)
{
    int ret = -1;

    spinlock_lock (&self->ci_lock);

    // Initialize the argument structure, fitting all data within our
    // shared memory region.
    int namelen = strlen(name) + 1;
    self->args.get_prop.service = "getprop";
    self->args.get_prop.nargs = 4;
    self->args.get_prop.nret = 1;
    self->args.get_prop.phandle = phandle;
    self->args.get_prop.name = self->args.shared + sizeof(self->args.get_prop);
    self->args.get_prop.buf = addr_align_up(self->args.get_prop.name + namelen, sizeof(word_t) );
    self->args.get_prop.buflen = buflen;
    self->args.get_prop.size = ret;

    // If the data fits, then invoke Open Firmware.
    word_t tot = (word_t)self->args.get_prop.buf - (word_t)&self->args.shared + 
	buflen;
    if( tot <= sizeof(self->args.shared) )
    {
	// Copy the name into the shared buffer.
	sstrncpy( self->args.get_prop.name, name, namelen );

	of1275_ci_call (self, &self->args.get_prop ); // Call OF.

	if( (self->args.get_prop.size > -1) && 
		(self->args.get_prop.size <= buflen) )
	{
	    // Copy the data into the outgoing buffer.
	    memcpy( buf, self->args.get_prop.buf, self->args.get_prop.size );
	    ret = self->args.get_prop.size;
	}
    }

    spinlock_unlock (&self->ci_lock);
    return ret;
}

int of1275_ci_write (of1275_client_interface_t *self, of1275_phandle_t phandle,
	const void *buf, int len)
{
    int ret = -1;

    // Adjust the amount of data to write as necessary.
    if( (len + sizeof(self->args.write)) > sizeof(self->args.shared) )
	len = sizeof(self->args.shared) - sizeof(self->args.write);

    spinlock_lock (&self->ci_lock);

    // Initialize the argument structure, fitting all data within our
    // shared data region.
    self->args.write.service = "write";
    self->args.write.nargs = 3;
    self->args.write.nret = 1;
    self->args.write.phandle = phandle;
    self->args.write.buf = self->args.shared + sizeof(self->args.write);
    self->args.write.len = len;
    self->args.write.actual = -1;
    memcpy( self->args.write.buf, buf, len );

    // Invoke OF.
    of1275_ci_call (self, &self->args.write );
    ret = self->args.write.actual;

    spinlock_unlock (&self->ci_lock);
    return ret;
}

int of1275_ci_read (of1275_client_interface_t *self, of1275_phandle_t phandle,
	void *buf, int len)
{
    int ret = -1;

    spinlock_lock (&self->ci_lock);

    // Adjust the size of the requested data to fit our shared buffer size.
    if( (len + sizeof(self->args.read)) > sizeof(self->args.shared) )
	len = sizeof(self->args.shared) - sizeof(self->args.read);

    // Initialize the argument structure, fitting all data within our
    // shared data region.
    self->args.read.service = "read";
    self->args.read.nargs = 3;
    self->args.read.nret = 1;
    self->args.read.phandle = phandle;
    self->args.read.buf = self->args.shared + sizeof(self->args.read);
    self->args.read.len = len;
    self->args.read.actual = -1;

    // Call OF.
    of1275_ci_call (self, &self->args.read );

    // If possible, copy the input data to the outgoing buffer.
    ret = self->args.read.actual;
    if( (ret >= 0) && (ret <= len) )
	memcpy( buf, self->args.read.buf, len );
    else
	ret = -1;

    spinlock_unlock (&self->ci_lock);
    return ret;
}

void of1275_ci_exit (of1275_client_interface_t *self)
{
    spinlock_lock (&self->ci_lock);

    // Pack the arguments into our shared data region.
    self->args.simple.service = "exit";
    self->args.simple.nargs = 0;
    self->args.simple.nret = 0;

    // Invoke OF.
    of1275_ci_call (self, &self->args.simple );

    // Hopefully the Open Firmware will never return to us ...
    spinlock_unlock (&self->ci_lock);
}

void of1275_ci_enter (of1275_client_interface_t *self)
{
    spinlock_lock (&self->ci_lock);

    // Pack the arguments into our shared data region.
    self->args.simple.service = "enter";
    self->args.simple.nargs = 0;
    self->args.simple.nret = 0;

    // Invoke OF.
    of1275_ci_call (self, &self->args.simple );

    spinlock_unlock (&self->ci_lock);
}

int of1275_ci_interpret (of1275_client_interface_t *self, const char *forth)
{
    int ret = -1;
    int forth_len = strlen(forth) + 1;

    if( (forth_len + sizeof(self->args.interpret)) > sizeof(self->args.shared))
	return ret;

    spinlock_lock (&self->ci_lock);

    // Pack the arguments into our shared data region.
    self->args.interpret.service = "interpret";
    self->args.interpret.nargs = 1;
    self->args.interpret.nret = 1;
    self->args.interpret.forth = self->args.shared + sizeof(self->args.interpret);
    self->args.interpret.result = -1;
    sstrncpy( self->args.interpret.forth, forth, forth_len );

    // Invoke OF
    of1275_ci_call (self, &self->args.interpret );
    ret = self->args.interpret.result;

    spinlock_unlock (&self->ci_lock);
    return ret;
}

void of1275_ci_quiesce (of1275_client_interface_t *self)
{
    spinlock_lock (&self->ci_lock);

    // Pack the arguments into our shared data region.
    self->args.simple.service = "quiesce";
    self->args.simple.nargs = 0;
    self->args.simple.nret = 0;

    // Invoke OF.
    of1275_ci_call (self, &self->args.simple );

    // Prevent any further invocations of Open Firmware.
    self->entry = NULL;

    spinlock_unlock (&self->ci_lock);
}

#endif	/* CONFIG_KDB_CONS_OF1275 */

