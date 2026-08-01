/****************************************************************************
 *
 * Copyright (C) 2003,  National ICT Australia (NICTA)
 *
 * File path:	arch/powerpc64/rtas.h
 * Description:	OpenFirmware Real Time Abstraction Service (RTAS) Interface.
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
 * $Id: rtas.h,v 1.5 2004/06/04 02:14:26 cvansch Exp $
 *
 ***************************************************************************/

#ifndef __ARCH__POWERPC64__RTAS_H__
#define __ARCH__POWERPC64__RTAS_H__

#include INC_ARCH(1275tree.h)
#include <sync.h>

#define	RTAS_SERVICE_UNKNOWN	(~0)

/* Since 64-bit RTAS is broken, we have to use 32-bit RTAS
 * Don't instantiate rtas at/above this value
 */
#define	RTAS_INSTANTIATE_LIMIT	(1ul<<30)

/* Event types */
#define RTAS_EVENT_INTERNAL_ERROR	(0x8<<28)
#define RTAS_EVENT_EPOW_WARNING		(0x4<<28)
#define RTAS_EVENT_POWERMGM_EVENTS	(0x2<<28)
#define RTAS_EVENT_HOTPLUG_EVENTS	(0x1<<28)
#define RTAS_ALL_EVENTS			(0xf<<28)


typedef u32_t rtas_arg_t;
                                                                                                                                                       

/* protected/private members are just members now; rtas.c is still the only
   file that touches them. */
struct rtas_args_t
{
    u32_t token;	    /* Pointer32 to string token    */
    u32_t nargs;	    /* Number of arguments	    */
    u32_t nret;		    /* Number of return values	    */
    rtas_arg_t args[16];    /* Arguments and return values buffer	*/

    rtas_arg_t *rets;	    /* Pointer to return value in args[] above	*/
};
typedef struct rtas_args_t rtas_args_t;


struct rtas_t
{
    word_t entry;	/* physical address pointer */
    word_t base;	/* physical address pointer */
    word_t size;	/* rtas area size */

    spinlock_t lock;
    of1275_device_t *rtas_dev;
};
typedef struct rtas_t rtas_t;

BEGIN_DECLS
/* Out of line in arch/powerpc64/rtas.c.  setup/set_arg/get_ret and
   try_location were protected or private and are file-static there. */
void rtas_init_arch( rtas_t *self );

bool rtas_get_token( rtas_t *self, const char *service, u32_t *token );
/* Two overloads of rtas_call: the variadic one takes the arguments inline,
   the other reads them from an array.  Distinct names in C. */
word_t rtas_call( rtas_t *self, u32_t token, u32_t nargs, u32_t nret, word_t *outputs, ... );
word_t rtas_call_data( rtas_t *self, word_t *data, u32_t token, u32_t nargs, u32_t nret );

/* Convience functions */
void rtas_machine_restart( rtas_t *self );	/* Restart the system */
void rtas_machine_power_off( rtas_t *self );	/* Turn off the power */
void rtas_machine_halt( rtas_t *self );		/* Halt the system */
END_DECLS

INLINE void rtas_init_cpu( rtas_t *self ) { /* dummy */ }	// XXX fixme for SMP

/* RTAS structure
 * Must be initialised in init.c
 */
INLINE rtas_t *get_rtas(void)
{
    extern rtas_t rtas;
    return &rtas;
}

struct rtas_error_log
{
    word_t version:8;			/* Architectural version    */
    word_t severity:3;			/* Severity level of error  */
    word_t disposition:2;		/* Degree of recovery	    */
    word_t extended:1;			/* extended log present?    */
    word_t /* reserved */ :2;		/* Reserved for future use  */
    word_t initiator:4;			/* Initiator of event	    */
    word_t target:4;			/* Target of failed operation */
    word_t type:8;			/* General event or error   */
    word_t extended_log_length:32;	/* length in bytes	    */
    word_t buffer[1];			/* allocated by klimit bump */
};
typedef struct rtas_error_log rtas_error_log;

#endif /* __ARCH__POWERPC64__RTAS_H__ */
