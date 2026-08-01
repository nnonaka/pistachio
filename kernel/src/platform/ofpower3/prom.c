/****************************************************************************
 *
 * Copyright (C) 2003, University of New South Wales
 *
 * File path:	platform/ofpower3/prom.c
 * Description:	OpenFirmware Power3 Setup.
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
 * $Id: prom.cc,v 1.3 2003/10/22 08:05:04 cvansch Exp $
 *
 ***************************************************************************/

#include INC_ARCH(of1275.h)
#include INC_ARCH(rtas.h)
#include INC_ARCH(1275tree.h)
#include INC_GLUE(hwspace.h)
#include INC_API(kernelinterface.h)

#include <debug.h>

word_t boot_cpuid SECTION(".init.data");
word_t boot_cpukhz SECTION(".init.data");
word_t boot_buskhz SECTION(".init.data");

/*
 * Map the position-independent device tree, and install.
 */
SECTION(".init") void of1275_tree_map( addr_t low, addr_t high )
{
    addr_t vaddr = phys_to_virt(low);

    prom_print_hex( "1275 tree found at", (word_t)vaddr );
    prom_puts( "\n\r" );

    of1275_tree_init( get_of1275_tree(), (char *)vaddr );
}


/*
 * Finds and installs the position-independent copy of the
 * OpenFirmware device tree.
 */
/* Renamed, as in platform/ofpower4/prom.c: of1275_tree_init is now the C
   entry point for the device tree object itself. */
SECTION(".init") static void install_of1275_tree( kernel_interface_page_t *kip )
{
    word_t i;

    // Look for the position-independent copy of the OpenFirmware device tree
    // in the kip's memory descriptors.
    for( i = 0; i < memory_info_get_num_descriptors (&kip->memory_info); i++ )
    {
	memdesc_t *mdesc = memory_info_get_memdesc( &kip->memory_info, i );

	if( (memdesc_type (mdesc) == OF1275_KIP_TYPE) &&
		(memdesc_subtype (mdesc) == OF1275_KIP_SUBTYPE) )
	{
	    of1275_tree_map( memdesc_low (mdesc), memdesc_high (mdesc) );
	    return;
	}
    }

    // Not found.  Things won't work, but ...
    prom_puts( "*** Error: the boot loader didn't supply a copy of the\n\r"
	       "*** Open Firmware device tree!\n\r" );
    of1275_tree_init( get_of1275_tree(), NULL );
}


/* Initialise the Platform
 * We are called in real mode - no relocation.
 * Map the kernel so that normal operation can continue
 */
void SECTION(".init") init_plat( word_t ofentry )
{
    word_t pvr;
    u32_t *prop_val, len, cpu, cpu_hz, bus_hz;
    of1275_device_t *chosen;
    of1275_phandle_t cpu_pkg;

    /* Initialise the Open Firmware interface used to setup the RTAS */
    of1275_init( get_of1275(), ofentry );

    /* Initialise position independant the device tree */
    install_of1275_tree( get_kip() );

    /* Initialise the RTAS */
    rtas_init_arch( get_rtas() );

    asm volatile (
	"mfpvr	    %0;"
	: "=r" (pvr)
    );
    switch( (pvr>>16) & 0xffff )
    {
    case 0x35: prom_puts( "Detected Power4 (Spinnaker) " );	break;
    case 0x38: prom_puts( "Detected Power4+	" );	break;
    case 0x40: prom_puts( "Detected Power3	" );	break;
    case 0x41: prom_puts( "Detected Power3+	" );	break;
    default:
	prom_print_hex( "Unknown Processor Version", (pvr >> 16) & 0xffff );
	prom_puts( ", " );
    }
    prom_print_hex( "Revision", (pvr & 0xffff) );
    prom_puts( "\n\r" );

    switch( (pvr>>16) & 0xffff )
    {
    case 0x40: ;
    case 0x41: break;
    default:
	prom_exit( "Unsupported CPU type\n\r" );
    }

    chosen = of1275_tree_find( get_of1275_tree(), "/chosen" );

    if ( !of1275_device_get_prop( chosen, "cpu", (char **)&prop_val, &len ))
    {
	prom_exit( "Unable get property \"cpu\" in /chosen\n\r" );
    }
    cpu_pkg = *prop_val;
//    cpu_pkg = of1275_instance_to_package( get_of1275(), *prop_val );
    of1275_get_prop( get_of1275(), cpu_pkg, "reg", &cpu, sizeof(cpu));

    of1275_get_prop( get_of1275(), cpu_pkg, "clock-frequency", &cpu_hz, sizeof(cpu_hz));
    of1275_get_prop( get_of1275(), cpu_pkg, "bus-frequency", &bus_hz, sizeof(bus_hz));
    
    boot_cpuid = cpu;
    boot_cpukhz = cpu_hz/1000;
    boot_buskhz = bus_hz/1000;

    prom_print_hex( "Boot cpu is", boot_cpuid );
    prom_puts( "\n\r" );
}
