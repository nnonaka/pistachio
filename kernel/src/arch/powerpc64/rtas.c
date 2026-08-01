/****************************************************************************
 *
 * Copyright (C) 2003,  National ICT Australia (NICTA)
 *
 * File path:	arch/powerpc64/rtas.c
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
 * $Id: rtas.cc,v 1.6 2004/06/04 03:40:20 cvansch Exp $
 *
 ***************************************************************************/

#include INC_API(kernelinterface.h)
#include INC_ARCH(of1275.h)
#include INC_ARCH(rtas.h)
#include INC_PLAT(prom.h)
#include INC_GLUE(hwspace.h)
#include <stdarg.h>
#include <debug.h>

/* The RTAS structure */
rtas_t rtas;

extern addr_t kip_get_phys_mem( kernel_interface_page_t *kip );
extern char _end_kernel_phys[];

EXTERN_C void __call_rtas( void * arg );

/* were protected members of rtas_args_t */
static void rtas_args_setup( rtas_args_t *self, u32_t token, u32_t nargs, u32_t nret )
{
    word_t i;

    ASSERT((nargs+nret) < 16);

    self->token = token;
    self->nargs = nargs;
    self->nret  = nret;
    self->rets  = (rtas_arg_t *)&(self->args[nargs]);

    for (i = 0; i < nret; i++)
	self->rets[i] = 0;
}

static void rtas_args_set_arg( rtas_args_t *self, u32_t num, rtas_arg_t value )
{
    self->args[num] = value;
}

static rtas_arg_t rtas_args_get_ret( rtas_args_t *self, u32_t num )
{
    return self->rets[num];
}

static bool rtas_try_location( rtas_t *self, word_t phys_start, word_t size );

/* Initialise the RTAS
 * Note, we are running with relocation off.
 */
void SECTION(".init") rtas_init_arch( rtas_t *self )
{
    of1275_phandle_t prom_rtas;

    self->base = 0;
    self->entry = 0;
    spinlock_init (&self->lock, 0);

    prom_puts( "Initialising IBM RTAS extensions\n\r" );

    self->rtas_dev = of1275_tree_find( get_of1275_tree(), "/rtas" );
    prom_rtas = of1275_find_device( get_of1275(), "/rtas" );

    // Sanity check 1275tree
    ASSERT((u32_t)prom_rtas == of1275_device_get_handle (self->rtas_dev));

    if (prom_rtas != OF1275_INVALID_PHANDLE)
    {
	u32_t rtas_size;
	word_t rtas_alloc_size;
	word_t phys_start;
	word_t total_mem;
	kernel_interface_page_t * kip = get_kip();
	bool found = false;

	of1275_get_prop( get_of1275(), prom_rtas, "rtas-size", &rtas_size, sizeof(rtas_size));

	self->size = rtas_size;

	rtas_alloc_size = (word_t)addr_align_up ((addr_t)(word_t)rtas_size, POWERPC64_PAGE_SIZE);
	
	total_mem = (word_t)kip_get_phys_mem(kip);

	for( phys_start = (word_t)_end_kernel_phys; 
		phys_start < (total_mem - rtas_alloc_size); 
		phys_start += KB(4))
	{
	    if( rtas_try_location(self, phys_start, rtas_alloc_size) )
	    {
		s32_t results[2];

		found = true;
		// Insert a KIP memory descriptor to protect the page hash.
		memory_info_insert( &kip->memory_info, MEMDESC_RESERVED, 0, false,
		    (addr_t)phys_start, (addr_t)(phys_start + rtas_alloc_size) );

		self->base = phys_start;
		prom_rtas = of1275_open( get_of1275(), "/rtas" );

		of1275_call_method( get_of1275(), prom_rtas, "instantiate-rtas",
				    results, 2, 1, (u32_t)self->base);

		self->entry = results[1];
		break;
	    }
	}
	if (!found)
	{
	    prom_puts( "No physical area large enough for RTAS found\n\r" );
	of1275_exit( get_of1275() );
	}
    }
    else
    {
	prom_puts( "RTAS extensions not found\n\r" );
	of1275_exit( get_of1275() );
    }

    if (self->entry == 0)
    {
	prom_puts( "RTAS instatiate failed\n\r" );
	of1275_exit( get_of1275() );
    } else
    {
	prom_print_hex( "RTAS installed at", self->base );
	prom_print_hex( ", size", self->size );
	prom_puts( "\n\r" );
	prom_print_hex( "RTAS entry", self->entry );
	prom_puts( "\n\r" );
    }
}


bool rtas_get_token( rtas_t *self, const char *service, u32_t *token )
{
    u32_t len;
    char *data;

    if (of1275_device_get_prop( self->rtas_dev, service, &data, &len ))
    {
	if (len == sizeof(u32_t))
	{
	    *token = *(u32_t*)data;
	    return true;
	}
    }
    return false;
}

/* These must be static global */
static rtas_args_t rtas_args;

word_t rtas_call( rtas_t *self, u32_t token, u32_t nargs, u32_t nret, word_t *outputs, ... )
{
    va_list list;
    word_t i;

    rtas_args_setup( &rtas_args, token, nargs, nret );

    va_start(list, outputs);
    for (i = 0; i < nargs; i++)
	rtas_args_set_arg( &rtas_args, i, (rtas_arg_t)(va_arg(list, word_t) & 0xffffffff));
    va_end(list);

    spinlock_lock (&self->lock);

    __call_rtas((void *)virt_to_phys(&rtas_args));

    spinlock_unlock (&self->lock);

    if (nret > 1 && outputs != NULL)
        for (i = 0; i < nret-1; ++i)
	    outputs[i] = rtas_args_get_ret(&rtas_args, i+1);

    return (word_t)((nret > 0) ? rtas_args_get_ret(&rtas_args, 0) : 0);
}

word_t rtas_call_data( rtas_t *self, word_t *data, u32_t token, u32_t nargs, u32_t nret )
{
    word_t i;

    rtas_args_setup( &rtas_args, token, nargs, nret );

    for (i = 0; i < nargs; i++)
	rtas_args_set_arg( &rtas_args, i, data[i] & 0xffffffff );

    spinlock_lock (&self->lock);

    __call_rtas((void *)virt_to_phys(&rtas_args));

    spinlock_unlock (&self->lock);

    if (nret > 1 )
        for (i = 0; i < nret-1; ++i)
	    data[nargs + i] = rtas_args_get_ret(&rtas_args, i+1);

    return (word_t)((nret > 0) ? rtas_args_get_ret(&rtas_args, 0) : 0);
}

void rtas_machine_restart( rtas_t *self )
{
    u32_t reboot_token;

    rtas_get_token( self, "system-reboot", &reboot_token );
    rtas_call( self, reboot_token, 0, 1, NULL );

    /* We should never get here */
    asm volatile (".long 0x00000000;");
}

void rtas_machine_power_off( rtas_t *self )
{
    u32_t poweroff_token;

    rtas_get_token( self, "power-off", &poweroff_token );
    rtas_call( self, poweroff_token, 0, 1, NULL );

    /* We should never get here */
    asm volatile (".long 0x00000000;");
}

void rtas_machine_halt( rtas_t *self )
{
    u32_t poweroff_token;

    rtas_get_token( self, "power-off", &poweroff_token );
    rtas_call( self, poweroff_token, 0, 1, NULL );

    /* We should never get here */
    asm volatile (".long 0x00000000;");
}

static SECTION(".init") bool rtas_try_location( rtas_t *self, word_t phys_start, word_t size )
{
    kernel_interface_page_t *kip = get_kip();
    word_t phys_end = phys_start + size;
    word_t i;

    if ((word_t)get_kip()->sigma0.mem_region.high > phys_start)
	return false;
    if ((word_t)get_kip()->sigma1.mem_region.high > phys_start)
	return false;
    if ((word_t)get_kip()->root_server.mem_region.high > phys_start)
	return false;


    // Walk through the KIP's memory descriptors and search for any
    // reserved memory regions that collide with our intended memory
    // allocation.
    for( i = 0; i < memory_info_get_num_descriptors (&kip->memory_info); i++ )
    {
	memdesc_t *mdesc = memory_info_get_memdesc( &kip->memory_info, i );
	word_t low, high;

	if( (memdesc_type (mdesc) == MEMDESC_CONVENTIONAL) || memdesc_is_virtual (mdesc) )
	    continue;

	low = (word_t)memdesc_low (mdesc);
	high = (word_t)memdesc_high (mdesc);

	if( (phys_start < low) && (phys_end > high) )
	    return false;
	if( (phys_start >= low) && (phys_start < high) )
	    return false;
	if( (phys_end > low) && (phys_end <= high) )
	    return false;
    }

    return true;
}

