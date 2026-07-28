/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2006-2007,  Karlsruhe University
 *                
 * File path:     platform/pc99/82093.h
 * Description:   Driver for IO-APIC 82093
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
 * $Id: 82093.h,v 1.10 2006/10/19 22:57:36 ud3 Exp $
 *                
 ********************************************************************/
#ifndef __PLATFORM__PC99__82093_H__
#define __PLATFORM__PC99__82093_H__

// the 82093 supports 24 IRQ lines
#define I82093_NUM_IRQS		24

typedef union {
    struct {
	u32_t version	: 8;
	u32_t		: 8;
	u32_t max_lvt	: 8;
	u32_t reserved0	: 8;
    } __attribute__((packed)) ver;
    u32_t raw;
} ioapic_version_t;
    

/* C mirrors of ioapic_redir_t / i82093_t (plain data + register pokes; same
   layouts).  Used by platform/generic/intctrl-apic.c. */
struct ioapic_redir_t {
    union {
	struct{
	    word_t vector			:  8;
	    word_t delivery_mode		:  3;
	    word_t dest_mode			:  1;
	    word_t delivery_status		:  1;
	    word_t polarity			:  1;
	    word_t irr				:  1;
	    word_t trigger_mode			:  1;
	    word_t mask				:  1;
	    word_t       			: 15;
	    union {
		struct {
		    word_t			: 24;
		    word_t physical_dest	:  8;
		} __attribute__((packed)) physical;

		struct {
		    word_t			: 24;
		    word_t logical_dest		:  8;
		} __attribute__((packed)) logical;
	    } dest;
	} x;
	u32_t raw[2];
    };
} __attribute__((packed));
typedef struct ioapic_redir_t ioapic_redir_t;

INLINE void ioapic_redir_set_fixed_hwirq (ioapic_redir_t *self, u32_t vector,
					  bool low_active, bool level_triggered,
					  bool masked, u32_t apicid)
{
    self->x.vector = (u8_t) vector;
    self->x.delivery_mode = 0;		/* fixed */
    self->x.dest_mode = 0;		/* physical mode */
    self->x.polarity = low_active ? 1 : 0;
    self->x.trigger_mode = level_triggered ? 1 : 0;
    self->x.mask = masked ? 1 : 0;
    self->x.dest.physical.physical_dest = apicid & 0xff;
}
INLINE void ioapic_redir_set_phys_dest (ioapic_redir_t *self, u32_t apicid)
{ self->x.dest.physical.physical_dest = apicid & 0xff; }
INLINE u32_t ioapic_redir_get_phys_dest (ioapic_redir_t *self)
{ return (u32_t) self->x.dest.physical.physical_dest; }
INLINE void ioapic_redir_mask_irq (ioapic_redir_t *self)	{ self->x.mask = 1; }
INLINE void ioapic_redir_unmask_irq (ioapic_redir_t *self)	{ self->x.mask = 0; }
INLINE bool ioapic_redir_is_masked_irq (ioapic_redir_t *self)	{ return self->x.mask == 1; }
INLINE bool ioapic_redir_is_level_triggered (ioapic_redir_t *self) { return self->x.trigger_mode == 1; }
INLINE bool ioapic_redir_is_edge_triggered (ioapic_redir_t *self)  { return self->x.trigger_mode == 0; }

/* The IOAPIC itself is addressed through a register-select / data window pair
   at offset 0 / 0x10 of its mapping, so the "object" is just that address. */
struct i82093_t { u32_t __regsel; } __attribute__((packed));
typedef struct i82093_t i82093_t;

#define I82093_IOAPIC_ID	0x00
#define I82093_IOAPIC_VER	0x01
#define I82093_IOAPIC_ARBID	0x02
#define I82093_IOAPIC_REDIR0	0x10

INLINE u32_t i82093_get (i82093_t *self, u32_t reg)
{
    *(__volatile__ u32_t *) self = reg;
    return *(__volatile__ u32_t *) (((word_t) self) + 0x10);
}
INLINE void i82093_set (i82093_t *self, u32_t reg, u32_t val)
{
    *(__volatile__ u32_t *) self = reg;
    *(__volatile__ u32_t *) (((word_t) self) + 0x10) = val;
}
INLINE u32_t i82093_reread (i82093_t *self)
{ return *(__volatile__ u32_t *) (((word_t) self) + 0x10); }

INLINE u8_t i82093_id (i82093_t *self)
{ return (u8_t) (i82093_get (self, I82093_IOAPIC_ID) >> 24); }
INLINE ioapic_version_t i82093_version (i82093_t *self)
{ ioapic_version_t v; v.raw = i82093_get (self, I82093_IOAPIC_VER); return v; }
INLINE word_t i82093_num_irqs (i82093_t *self)
{ return i82093_version (self).ver.max_lvt + 1; }

/* VU: masking an IRQ on the IOAPIC only becomes active after performing a read
   on the data register -- hence the reread() when the entry is masked. */
INLINE void i82093_set_redir_entry (i82093_t *self, word_t idx, ioapic_redir_t redir)
{
    ASSERT (idx < i82093_num_irqs (self));
    i82093_set (self, (u32_t) (0x11 + (idx * 2)), redir.raw[1]);
    i82093_set (self, (u32_t) (0x10 + (idx * 2)), redir.raw[0]);
    if (redir.x.mask) i82093_reread (self);
}
INLINE void i82093_set_redir_entry_low (i82093_t *self, word_t idx, ioapic_redir_t redir)
{
    ASSERT (idx < i82093_num_irqs (self));
    i82093_set (self, (u32_t) (0x10 + (idx * 2)), redir.raw[0]);
    if (redir.x.mask) i82093_reread (self);
}
INLINE void i82093_set_redir_entry_high (i82093_t *self, word_t idx, ioapic_redir_t redir)
{
    ASSERT (idx < i82093_num_irqs (self));
    i82093_set (self, (u32_t) (0x11 + (idx * 2)), redir.raw[1]);
}
INLINE ioapic_redir_t i82093_get_redir_entry (i82093_t *self, word_t idx)
{
    ASSERT (idx < i82093_num_irqs (self));
    ioapic_redir_t redir;
    redir.raw[1] = i82093_get (self, (u32_t) (0x11 + (idx * 2)));
    redir.raw[0] = i82093_get (self, (u32_t) (0x10 + (idx * 2)));
    return redir;
}

#endif /* !__PLATFORM__PC99__82093_H__ */
