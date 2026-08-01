/*********************************************************************
 *                
 * Copyright (C) 2005,  National ICT Australia (NICTA)
 *                
 * File path:     kdb/platform/ofg5/reboot.c
 * Description:   G5 system reset
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
 * $Id: reboot.cc,v 1.1 2005/01/18 13:55:14 cvansch Exp $
 *                
 ********************************************************************/
#include <debug.h>
#include <kdb/kdb.h>

#include INC_ARCH(string.h)
#include INC_ARCH(1275tree.h)	// the device tree
#include INC_GLUE(pgent_inline.h)

/*
 * Reboot the box
 */
DECLARE_CMD (cmd_reboot, root, '6', "reset", "Reset system");

u8_t* pmu_regs;

#define VIA_SHIFT	9
#define VIA(x)		((x) << VIA_SHIFT)

#define pmu_read(x)	(*(volatile u8_t*)(pmu_regs + VIA(x)))
#define pmu_write(x, d)	(*(volatile u8_t*)(pmu_regs + VIA(x))) = (d)

/* W65C22S Versatile Interface Adaptor (similar to keylargo?) */
#define	ORB		0x0
#define	ORA		0x1
#define	DDRB		0x2
#define	DDRA		0x3
#define	TIC_L		0x4
#define	TIC_H		0x5
#define	TIL_L		0x6
#define	TIL_H		0x7
#define	T2C_L		0x8
#define	T2C_H		0x9
#define	SR		0xa
#define	ACR		0xb
#define	PCR		0xc
#define	IFR		0xd
#define	IER		0xe
#define	ORA_nh		0xf	/* Same as ORA, but no handshake */

/* ORB Register */
#define ORB_TACK	0x08
#define ORB_TREQ	0x10

/* ACR Register */
#define ACR_SRCTRL	0x1c
#define ACR_SREXT	0x0c
#define ACR_SROUT	0x10

/* PMU Commands */
#define CMD_RESET   0xd0

CMD (cmd_reboot, cg)
{
    of1275_device_t *pmu_dev;
    of1275_device_t *macio_dev;
    of1275_pci_assigned_addresses *macio_ranges;
    struct macio_addresses {
	u32_t	addr;
	u32_t	size;
    } *pmu_ranges;

    char *type;
    u32_t len;
    u64_t pmu_phys;

    if (!(pmu_dev = of1275_tree_find_device_type( get_of1275_tree(), "via-pmu" )))
	goto error;
printf("pmu is %p\n", pmu_dev);

    if (!(macio_dev = of1275_tree_get_parent( get_of1275_tree(), pmu_dev )))
	goto error;

    if (!of1275_device_get_prop( macio_dev, "device_type", (char**)&type, &len ))
	goto error;
printf("mac-io is %p\n", macio_dev);

    if (strncmp(type, "mac-io", 6)) {
printf("invalid parent\n\r");
	goto error;
    }

    if (!of1275_device_get_prop( macio_dev, "assigned-addresses", (char**)&macio_ranges, &len ))
	goto error;

    if (!of1275_device_get_prop( pmu_dev, "reg", (char**)&pmu_ranges, &len ))
	goto error;

    pmu_phys = ((u64_t)macio_ranges[0].pci.addr.a_mid << 32) |
	    macio_ranges[0].pci.addr.a_lo;
    pmu_phys += pmu_ranges[0].addr;

    {
	pgent_t pg1, pg2;
	pgsize_e size = size_4k;

	/* Create a page table entry, noexecute, nocache */
	/* Upstream passed eight arguments -- (space, size, paddr, readable,
	   writable, executable, kernel, attrib) -- to a set_entry that has
	   taken (space, size, paddr, rwx, attrib, kernel) for as long as
	   pgent_inline.h has existed, so this call has never compiled.
	   read|write and no execute is rwx == 6, which is exactly what
	   kdb/arch/powerpc64/macio.c passes for the same kind of device
	   mapping.  Notes §169. */
	pgent_set_entry( &pg1, get_kernel_space(), size, (addr_t)(pmu_phys & ~(0xfff)),
			6, cache_inhibit, true );
	pgent_set_entry( &pg2, get_kernel_space(), size, (addr_t)(((word_t)pmu_phys & ~(0xfff)) + 0x1000),
			6, cache_inhibit, true );
    
	pmu_regs = (u8_t *)((word_t)pmu_phys | DEVICE_AREA_START);

	/* Insert the kernel mapping, bolted */
	pghash_insert_mapping_bolted( get_pghash(), get_kernel_space(),
			(addr_t)((word_t)pmu_regs & ~(0xfff)), &pg1, size, true );
	pghash_insert_mapping_bolted( get_pghash(), get_kernel_space(),
			(addr_t)(((word_t)pmu_regs & ~(0xfff)) + 0x1000), &pg2, size, true );
printf("pmu_regs = %p\n", pmu_regs);
    }

    {
	/* Send reset command */
	word_t acr = pmu_read(ACR);
	pmu_write(ACR, (acr & ~ ACR_SROUT) | ACR_SREXT);
	pmu_write(SR, CMD_RESET);
	word_t orb = pmu_read(ORB);
	pmu_write(ORB, orb & ~ORB_TREQ);
	pmu_read(ORB);
    }

    return CMD_NOQUIT;

error:
    printf("PMU init error, cannot reboot\n");
    return CMD_NOQUIT;
}

/*
 * Powerdown the box
 */
DECLARE_CMD (cmd_powerdown, root, '7', "poweroff", "Power-off the system");

CMD (cmd_powerdown, cg)
{
    printf("Unimplemented\n");
    return CMD_NOQUIT;
}
