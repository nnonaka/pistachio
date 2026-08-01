/****************************************************************************
 *
 * Copyright (C) 2003,  National ICT Australia (NICTA)
 *
 * File path:	arch/powerpc64/of1275.h
 * Description:	OpenFirmware Interface.
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
 * $Id: of1275.h,v 1.4 2005/01/18 13:30:11 cvansch Exp $
 *
 ***************************************************************************/

#ifndef __ARCH__POWERPC64__OF1275_H__
#define __ARCH__POWERPC64__OF1275_H__

#include <sync.h>

#define PTRRELOC(x)	((typeof(x))((word_t)(x) - KERNEL_OFFSET))
#define PTRUNRELOC(x)	((typeof(x))((word_t)(x) + KERNEL_OFFSET))
#define RELOC(x)	(*PTRRELOC(&(x)))

typedef s32_t of1275_phandle_t;
typedef s32_t of1275_ihandle_t;
typedef s32_t ptr32_t;

#define OF1275_INVALID_PHANDLE	((of1275_phandle_t)-1)
#define OF1275_INVALID_IHANDLE	((of1275_ihandle_t)-1)

/* Was a class; the protected/public split disappears, so the members below
   that were protected (entry, stdin/stdout, ci_lock, args) are now reachable
   directly -- of1275.c is still their only writer.  The `call' helper was
   private and stays file-static there. */
struct of1275_client_interface_t
{
    word_t entry;
    of1275_phandle_t stdout;
    of1275_phandle_t stdin;

    spinlock_t ci_lock;

    union 
    {
	char shared[512];
	struct {
	    ptr32_t service;
	    s32_t nargs;
	    s32_t nret;
	    of1275_phandle_t phandle;
	    ptr32_t name;
	    ptr32_t buf;
	    s32_t buflen;
	    s32_t size;
	} get_prop;
	struct {
	    ptr32_t service;
	    s32_t nargs;
	    s32_t nret;
	    ptr32_t name;
	    of1275_phandle_t phandle;
	} find_device;
	struct {
	    ptr32_t service;
	    s32_t nargs;
	    s32_t nret;
	    ptr32_t name;
	    of1275_ihandle_t ihandle;
	} open;
	struct {
	    ptr32_t service;
	    s32_t nargs;
	    s32_t nret;
	    of1275_phandle_t phandle;
	    ptr32_t buf;
	    s32_t len;
	    s32_t actual;
	} write;
	struct {
	    ptr32_t service;
	    s32_t nargs;
	    s32_t nret;
	    of1275_phandle_t phandle;
	    ptr32_t buf;
	    s32_t len;
	    s32_t actual;
	} read;
	struct {
	    ptr32_t service;
	    s32_t nargs;
	    s32_t nret;
	    ptr32_t forth;
	    s32_t result;
	} interpret;
	struct {
	    ptr32_t service;
	    s32_t nargs;
	    s32_t nret;
	    ptr32_t method;
	    of1275_phandle_t phandle;
	    s32_t args[32];
	} call;
	struct {
	    ptr32_t service;
	    s32_t nargs;
	    s32_t nret;
	} simple;
	struct {
	    ptr32_t service;
	    s32_t nargs;
	    s32_t nret;
	    s32_t val;
	    of1275_phandle_t ret;
	} simple_arg;
	struct {
	    ptr32_t service;
	    s32_t nargs;
	    s32_t nret;
	    u32_t virt;
	    u32_t size;
	    u32_t align;
	    s32_t retval;
	} claim;
    } args;

};
typedef struct of1275_client_interface_t of1275_client_interface_t;

INLINE of1275_phandle_t of1275_get_stdout (of1275_client_interface_t *self)
    { return self->stdout; }
INLINE of1275_phandle_t of1275_get_stdin (of1275_client_interface_t *self)
    { return self->stdin; }

/* Out of line in arch/powerpc64/of1275.c. */
BEGIN_DECLS
void of1275_init( of1275_client_interface_t *self, word_t entry );
of1275_phandle_t of1275_find_device( of1275_client_interface_t *self, const char *name );
s32_t of1275_get_prop( of1275_client_interface_t *self, of1275_phandle_t phandle,
		       const char *name, void *buf, s32_t buflen );
of1275_ihandle_t of1275_open( of1275_client_interface_t *self, const char *name );
s32_t of1275_write( of1275_client_interface_t *self, of1275_phandle_t phandle,
		    const void *buf, s32_t len );
s32_t of1275_read( of1275_client_interface_t *self, of1275_phandle_t phandle,
		   void *buf, s32_t len );
of1275_phandle_t of1275_instance_to_package( of1275_client_interface_t *self, s32_t val );
s32_t of1275_claim( of1275_client_interface_t *self, addr_t virt, u32_t size, u32_t align );

void of1275_puts( of1275_client_interface_t *self, char *str, int len );
void of1275_call_method( of1275_client_interface_t *self, of1275_phandle_t phandle,
			 const char *method, s32_t *results, int nret, int nargs, ...);

void of1275_exit( of1275_client_interface_t *self );
void of1275_quiesce( of1275_client_interface_t *self );
void of1275_enter( of1275_client_interface_t *self );
s32_t of1275_interpret( of1275_client_interface_t *self, const char *forth );
END_DECLS


INLINE of1275_client_interface_t *get_of1275(void)
{
    extern of1275_client_interface_t of1275;
    return &of1275;
}

extern void prom_exit( char * msg );
extern void prom_puts( char * msg );
extern void prom_print_hex( char *s, word_t val );

#endif /* __ARCH__POWERPC64__OF1275_H__ */
