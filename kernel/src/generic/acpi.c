/*********************************************************************
 *                
 * Copyright (C) 2002, 2003,  Karlsruhe University
 *                
 * File path:     generic/acpi.c
 * Description:   Implementation of ACPI structure walkers
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
 * $Id: acpi.cc,v 1.4 2003/09/24 19:05:31 skoglund Exp $
 *                
 ********************************************************************/

#include <acpi.h>

/* Walk the MADT entries looking for the index'th entry of the given type.
   Translated from acpi_madt_t's methods (the C++ decls in acpi.h carry
   __asm__ labels naming these symbols, so C++ call sites link here). */
acpi_madt_hdr_t* acpi_madt_find (acpi_madt_t *self, u8_t type, int index)
{
    for (word_t i = 0; i < (self->header.len - sizeof (acpi_madt_t));)
    {
	acpi_madt_hdr_t* h = (acpi_madt_hdr_t*) &self->data[i];
	if (h->type == type)
	{
	    if (index == 0)
		return h;
	    index--;
	}
	i += h->len;
    };
    return NULL;
}

acpi_madt_lapic_t* acpi_madt_lapic (acpi_madt_t *self, int index)
{
    return (acpi_madt_lapic_t*) acpi_madt_find (self, 0, index);
}

acpi_madt_ioapic_t* acpi_madt_ioapic (acpi_madt_t *self, int index)
{
    return (acpi_madt_ioapic_t*) acpi_madt_find (self, 1, index);
}

acpi_madt_irq_t* acpi_madt_irq (acpi_madt_t *self, int index)
{
    return (acpi_madt_irq_t*) acpi_madt_find (self, 2, index);
}

acpi_madt_nmi_t* acpi_madt_nmi (acpi_madt_t *self, int index)
{
    return (acpi_madt_nmi_t*) acpi_madt_find (self, 3, index);
}

acpi_madt_lsapic_t * acpi_madt_lsapic (acpi_madt_t *self, int index)
{
    return (acpi_madt_lsapic_t*) acpi_madt_find (self, 7, index);
}

acpi_madt_iosapic_t * acpi_madt_iosapic (acpi_madt_t *self, int index)
{
    return (acpi_madt_iosapic_t*) acpi_madt_find (self, 6, index);
}
