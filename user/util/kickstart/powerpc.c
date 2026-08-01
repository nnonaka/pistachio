/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     powerpc.cc
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
#include <config.h>
#include <l4io.h>
#include <l4/arch.h>

#include "kickstart.h"
#include "fdt.h"

static inline void dcbi(void* ptr)
{
    __asm__ __volatile__ ("dcbi  0,%0" : : "r" (ptr) : "memory");
}


static inline void mtdcrx(unsigned int dcrn, unsigned int value)
{
    __asm__ __volatile__("mtdcrx %0,%1": :"r" (dcrn), "r" (value) : "memory");
}



struct bgp_mailbox_t
{
    volatile unsigned short command;	// comand; upper bit=ack
    unsigned short len;			// length (does not include header)
    unsigned short result;		// return code from reader
    unsigned short crc;			// 0=no CRC
    char data[0];
};
typedef struct bgp_mailbox_t bgp_mailbox_t;

struct bgp_cons_t {
    bgp_mailbox_t *mb;
    unsigned size;
    unsigned dcr_set;
    unsigned dcr_clear;
    unsigned dcr_mask;
    bool verbose;

};
typedef struct bgp_cons_t bgp_cons_t;


/* were members of bgp_cons_t. */
static void bgp_cons_send_command (bgp_cons_t *self, int command)
{
    self->mb->command = command;
    __asm__ __volatile__("sync");
    mtdcrx(self->dcr_set, self->dcr_mask);
    
    do {
	dcbi((void*)&self->mb->command);
    } while(!(self->mb->command & 0x8000));
}

static bool bgp_cons_init (bgp_cons_t *self, fdt_t *fdt)
{
    fdt_property_t *prop;
    fdt_node_t *node = fdt_find_subtree (fdt, "/jtag/console0");

    if (! (prop = fdt_find_property_node (fdt, node, "reg")) )
	return false;

    // addr is 64 bit with upper part 0
    self->mb = (bgp_mailbox_t*)fdt_property_get_word (prop, 1);
    self->size = fdt_property_get_word (prop, 2);
    
    if (! (prop = fdt_find_property_node (fdt, node, "dcr-reg")) )
	return false;

    self->dcr_set = fdt_property_get_word (prop, 0);
    self->dcr_clear = fdt_property_get_word (prop, 1);
    
    if (! (prop = fdt_find_property_node (fdt, node, "dcr-mask")) )
	return false;
    self->dcr_mask = fdt_property_get_word (prop, 0);

    self->verbose = false;
    fdt_node_t *l4node = fdt_find_subtree (fdt, "/l4");
    if ((prop = fdt_find_property_node (fdt, l4node, "kickstart")))
    {
	if (strstr(fdt_property_get_string (prop), "self->verbose"))
	    self->verbose = true;
    }

    return true;
}

static void bgp_cons_putc (bgp_cons_t *self, int c)
{
    if (!self->mb || !self->verbose)
	return;

    self->mb->data[self->mb->len++] = c;

    if (self->mb->len >= self->size || c == '\n')
    {
	bgp_cons_send_command (self, 2);
	self->mb->len = 0;
    }
}

bgp_cons_t bgp_cons;

void putc(int c)
{
    bgp_cons_putc (&bgp_cons, c);
#if defined(CONFIG_COMPORT)
    extern void __l4_putc(int c);
    __l4_putc(c);
#endif
}


/*
 * Loader formats supported for PowerPC
 */
bool fdt_probe (void);
L4_Word_t fdt_init (void);

loader_format_t loader_formats[] = {
    { "Flattened device tree", fdt_probe, fdt_init },
    NULL_LOADER
};


void fail(int ec)
{
    printf("PANIC: FAIL in line %d\n", ec);
    while(1);
}

void flush_dcache_range(L4_Word_t start, L4_Word_t end)
{
    printf("invalidate dcache %x-%x\n", start, end);
    for (; start < end; start += 32)
	__asm__("dcbf 0, %0" : : "b"(start));
}

void flush_cache (void)
{
    /* Should we flush the cache??? */
    flush_dcache_range((L4_Word_t)get_fdt_ptr(), 
		       ((L4_Word_t)get_fdt_ptr()) + get_fdt_ptr()->size);
}

static fdt_t *fdt_ptr;
fdt_t *get_fdt_ptr (void)
{
    return fdt_ptr;
}

extern void (*entry_secondary)(void);

void launch_kernel(L4_Word_t entry)
{
    void (*kernel)(void) = (void(*)(void))entry;

    entry_secondary = kernel; /* release APs */
    __asm__("msync; dcbi 0, %0" : : "b"(&entry_secondary));
    (*kernel)();
}

void loader();
void __loader(L4_Word_t r3, L4_Word_t r4, L4_Word_t r5, 
			 L4_Word_t r6, L4_Word_t r7)
{
    fdt_ptr = (fdt_t*)r3;
#if defined(CONFIG_COMPORT)
    extern void *__l4_dtree;
    __l4_dtree = fdt_ptr;
#endif
    
    bgp_cons_init (&bgp_cons, fdt_ptr);
    loader();
}
