/****************************************************************************
 *
 * Copyright (C) 2003, University of New South Wales
 *
 * File path:	platform/ofpower3/opic.c
 * Description:	OpenPIC interrupt controller.
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
 * $Id: opic.cc,v 1.3 2003/10/27 07:34:05 cvansch Exp $
 *
 ***************************************************************************/

#include <linear_ptab.h>

#include INC_GLUE(intctrl.h)
#include INC_ARCH(1275tree.h)
#include INC_ARCH(pgent.h)
#include INC_ARCH(segment.h)
#include INC_PLAT(opic.h)

intctrl_t intctrl;

open_pic_t *opic = NULL;

SECTION(".init") void intctrl_init_arch (void)
{
    of1275_device_t *root;
    u32_t *prop, *cells, len;
    word_t n, address, i, freq;
    pgent_t pg;
    pgsize_e size;
    open_pic_feature0_t f;
    const char *version;

    root = of1275_tree_find( get_of1275_tree(), "/" );
    printf( "OpenPIC init\n" );

    /* Find the Open PIC if present */
    if ( !of1275_device_get_prop( root, "platform-open-pic", (char **)&prop, &len ) )
    {
	printf( "*** no open-pic interrupt controller found\n" );
	return;
    }

    of1275_device_get_prop( root, "#address-cells", (char **)&cells, &len );

    n = *cells;

    for( address=0; n > 0; n-- )
	address = (address << 32) + *prop++;

    printf( "OpenPIC found at: %p\n", address);
    opic = (open_pic_t*)(address | DEVICE_AREA_START);

    /* XXX - we should lookup mapping first */
#ifdef CONFIG_POWERPC64_LARGE_PAGES
    size = size_16m;
#else
    size = size_4k;
#endif

    /* Insert mappings for hash page table */
    for ( i = 0; i < (sizeof(open_pic_t)); i += page_size(size))
    {
	/* Create a page table entry, noexecute, nocache.  set_entry took
	   eight arguments here; the same pre-rwx signature as
	   kdb/platform/ofg5/reboot.cc (§169), and the same translation:
	   read|write, no execute, kernel, is rwx == 6. */
	pgent_set_entry( &pg, get_kernel_space(), size,
			(addr_t)(address + i), 6, cache_inhibit, true );

	pghash_insert_mapping_bolted( get_pghash(), get_kernel_space(),
			(addr_t)(((word_t)opic) + i), &pg, size, true );
    }

    f = open_pic_get_feature0( opic );

    switch (f.x.version)
    {
    case 1: version = "1.0"; break;
    case 2: version = "1.2"; break;
    case 3: version = "1.3"; break;
    default: version = "??"; break;
    }

    printf( "OpenPIC version %s (%d CPUS, %d IRQ sources)\n", version,
				    f.x.last_cpu + 1, f.x.last_source + 1 );

    freq = open_pic_get_timer_frequency( opic );
    printf( "OpenPIC timer frequency = %d.%06d MHz\n", freq / 1000000, freq % 1000000 );
}

