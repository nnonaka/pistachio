/*********************************************************************
 *                
 * Copyright (C) 2002, 2003,  Karlsruhe University
 *                
 * File path:     kdb/generic/acpi.c
 * Description:   Kernel deubgger ACPI acccess
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
 * $Id: acpi.cc,v 1.4 2003/09/24 19:05:11 skoglund Exp $
 *                
 ********************************************************************/
#include <debug.h>
#include <kdb/kdb.h>
#include <kdb/cmd.h>
#include <kdb/input.h>

#include <acpi.h>

#include INC_GLUE(hwspace.h)
#include INC_PLAT(acpi.h)


/**
 * Root System Descriptor Pointer.
 */
static acpi_rsdp_t * rsdp = NULL;

/**
 * Root System Description Table.
 */
static acpi_rsdt_t * rsdt = NULL;

/**
 * Extended System Description Table.
 */
static acpi_xsdt_t * xsdt = NULL;


void SECTION (SEC_KDEBUG) dump_apic (acpi_madt_t * madt);

/* Was a reference parameter with a defaulted prefix; C takes the pointer and
   every caller passes the prefix.

   The phys_to_virt on the two strings is gone with it.  Callers used to hand
   this a physical pointer; they now hand it one already through the remap
   window, so translating again pointed at mapped-but-unrelated memory and both
   strings printed empty -- while the integer fields beside them, read straight
   off the same header, came out right. */
static void SECTION (SEC_KDEBUG)
dump_acpi_header (acpi_thead_t * h, const char * prefix)
{
    printf ("%sOEM:    [\"%.6s\", tid: \"%.8s\", rev: %d]\n"
	    "%sVendor: [id: 0x%x, rev: 0x%x]\n",
	    prefix, h->oem_id, h->oem_tid, h->oem_rev,
	    prefix, h->creator_id, h->creator_rev);
}


/* The two description tables differ only in the width of their pointers, so
   the dump is one macro rather than two near-identical loops. */
#define DUMP_SDT(name, sdt)						\
    do {								\
	word_t num_entries = ((sdt)->header.len - sizeof (acpi_thead_t)) / \
	    sizeof ((sdt)->ptrs[0]);					\
									\
	printf (name " @ %p\n", (sdt));					\
	dump_acpi_header (&(sdt)->header, "  ");			\
	printf ("\n");							\
									\
	for (word_t i = 0; i < num_entries; i++)			\
	{								\
	    acpi_thead_t * t =						\
		(acpi_thead_t *) acpi_remap ((addr_t) (word_t) (sdt)->ptrs[i]); \
	    printf ("  %.4s @ %p\n", t->sig, t);			\
	    dump_acpi_header (t, "    ");				\
									\
	    if (t->sig[0] == 'A' && t->sig[1] == 'P' &&			\
		t->sig[2] == 'I' && t->sig[3] == 'C')			\
		dump_apic ((acpi_madt_t *) t);				\
	}								\
    } while (0)


/*
 * Command group for ACPI commands.
 */

DECLARE_CMD_GROUP (acpi);

DECLARE_CMD (cmd_acpi, arch, 'a', "acpi", "ACPI access");

CMD (cmd_acpi, cg)
{
    if (rsdp == NULL)
    {
	/* The C++ declaration defaulted the argument to NULL, so this scanned
	   128K from virtual zero and faulted before it could find anything.
	   Scan where the spec says the RSDP is (ACPI 2.0, 5.2.4.1), through
	   the same remap window platform/generic/intctrl-apic.c uses -- the
	   one caller in the tree that locates it successfully. */
	rsdp = acpi_rsdp_locate (acpi_remap ((addr_t) ACPI20_PC99_RSDP_START));
	if (rsdp == NULL)
	{
	    printf ("Could not locate ACPI info.\n");
	    return CMD_NOQUIT;
	}

	/* Both accessors hand back physical pointers. */
	{
	    acpi_rsdt_t * r = acpi_rsdp_rsdt (rsdp);
	    acpi_xsdt_t * x = acpi_rsdp_xsdt (rsdp);
	    rsdt = r ? (acpi_rsdt_t *) acpi_remap ((addr_t) r) : NULL;
	    xsdt = x ? (acpi_xsdt_t *) acpi_remap ((addr_t) x) : NULL;
	}
    }

    return cmd_group_interact (&acpi, cg, "acpi");
}


DECLARE_CMD (cmd_acpi_dump, acpi, 'd', "dump",
	     "dump system description table");

CMD (cmd_acpi_dump, cg)
{
    printf ("ACPI rev: %d, OEM: \"%.6s\"\n", rsdp->rev, rsdp->oemid);

    if (rsdt)
	DUMP_SDT ("RSDT", rsdt);

    if (xsdt)
	DUMP_SDT ("XSDT", xsdt);

    return CMD_NOQUIT;
}

void dump_apic (acpi_madt_t * madt)
{
    printf ("    Local APIC @ %p\n", madt->local_apic_addr);
    for (word_t i = 0; i < (madt->header.len - sizeof (acpi_madt_t));)
    {
	acpi_madt_hdr_t * h = (acpi_madt_hdr_t *) &madt->data[i];
	switch (h->type)
	{
	case 0:
	{
	    // Local APIC
	    acpi_madt_lapic_t * lapic = (acpi_madt_lapic_t *) h;
	    printf ("      Local APIC  ");
	    printf ("[Id: 0x%x, CPU Id: 0x%x, %s]\n",
		    lapic->id, lapic->apic_processor_id,
		    lapic->flags.enabled ? "enabled" : "disabled");
	    break;
	}    
	case 1:
	{
	    // I/O Apic
	    acpi_madt_ioapic_t * ioapic = (acpi_madt_ioapic_t *) h;
	    printf ("      I/O APIC  ");
	    printf ("[Id: 0x%x, IRQ base: %d, Addr: %p]\n",
		    ioapic->id, ioapic->irq_base, ioapic->address);
	    break;
	}
	case 2:
	{
	    // Interrupt Source Override
	    acpi_madt_irq_t * irq = (acpi_madt_irq_t *) h;
	    word_t p = acpi_madt_irq_get_polarity (irq);
	    word_t t = acpi_madt_irq_get_trigger_mode (irq);
	    printf ("      Interrupt Override  ");
	    printf ("[%s, Bus IRQ: %d, Glob IRQ: %d, Pol: %s, Trigger: %s]\n",
		    irq->src_bus == 0 ? "ISA" : "unknown bus",
		    irq->src_irq, irq->dest,
		    p == ACPI_MADT_CONFORM_POLARITY ? "conform" :
		    p == ACPI_MADT_ACTIVE_HIGH ? "active high" :
		    p == ACPI_MADT_ACTIVE_LOW ? "active low" : "?",
		    t == ACPI_MADT_CONFORM_TRIGGER ? "conform" :
		    t == ACPI_MADT_EDGE ? "edge" :
		    t == ACPI_MADT_LEVEL ? "level" : "?");
	    break;
	}
	case 3:
	{
	    // NMI Source
	    acpi_madt_nmi_t * nmi = (acpi_madt_nmi_t *) h;
	    word_t p = acpi_madt_nmi_get_polarity (nmi);
	    word_t t = acpi_madt_nmi_get_trigger_mode (nmi);
	    printf ("      NMI Source  ");
	    printf ("[Glob IRQ: %d, Pol: %s, Trigger: %s]\n",
		    nmi->irq,
		    p == ACPI_MADT_CONFORM_POLARITY ? "conform" :
		    p == ACPI_MADT_ACTIVE_HIGH ? "active high" :
		    p == ACPI_MADT_ACTIVE_LOW ? "active low" : "?",
		    t == ACPI_MADT_CONFORM_TRIGGER ? "conform" :
		    t == ACPI_MADT_EDGE ? "edge" :
		    t == ACPI_MADT_LEVEL ? "level" : "?");
	    break;
	}
	case 4:
	{
	    // Local APIC NMI
	    printf ("      Local APIC NMI\n");
	    break;
	}
	case 5:
	{
	    // Local APIC Address Override
	    printf ("      Local APIC Address Override\n");
	    break;
	}
	case 6:
	{
	    // I/O SAPIC
	    acpi_madt_iosapic_t * iosapic = (acpi_madt_iosapic_t *) h;
	    printf ("      I/O SAPIC  ");
	    printf ("[Id: 0x%x, IRQ base: %d, Addr: %p]\n", iosapic->id,
		    iosapic->irq_base, iosapic->address);
	    break;
	}
	case 7:
	{
	    // Local SAPIC
	    acpi_madt_lsapic_t * lsapic = (acpi_madt_lsapic_t *) h;
	    printf ("      Local SAPIC  ");
	    printf ("[Id: 0x%x:0x%x, CPU Id: 0x%x, %s]\n",
		    lsapic->id, lsapic->eid, lsapic->apic_processor_id,
		    lsapic->flags.enabled ? "enabled" : "disabled");
	    break;
	}
	case 8:
	{
	    // Platform Interrupt Source
	    printf ("      Platform Interrupt Source\n");
	    break;
	}
	}

	i += h->len;
    }
}
