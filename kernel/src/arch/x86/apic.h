/*********************************************************************
 *
 * Copyright (C) 2002-2004,  Karlsruhe University
 *
 * File path:     arch/x86/apic.h
 * Description:   Driver for the Local APIC in x86 processors
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
 * $Id: apic.h,v 1.4.4.4 2006/12/05 15:53:04 skoglund Exp $
 *
 ********************************************************************/
#ifndef __ARCH__X86__APIC_H__
#define __ARCH__X86__APIC_H__


/*
 * Minimal C API for the local APIC at the fixed kernel mapping, mirroring the
 * local_apic_t<APIC_MAPPINGS_START> template methods that C code (cpu.c) uses.
 * Register offsets and bit layout match the template's regno_t/command_reg_t.
 */
#define X86_LAPIC_SVR		0x0F0
#define X86_LAPIC_EOI		0x0B0
#define X86_LAPIC_INTR_CMD1	0x300
#define X86_LAPIC_INTR_CMD2	0x310
#define X86_LAPIC_LVT_TIMER	0x320
#define X86_LAPIC_TIMER_COUNT	0x380
#define X86_LAPIC_TIMER_CURRENT	0x390
#define X86_LAPIC_TIMER_DIVIDE	0x3E0

INLINE void local_apic_eoi (void)
{
    *(volatile u32_t *)(APIC_MAPPINGS_START + X86_LAPIC_EOI) = 0;
}

/* Timer register access, mirroring local_apic_t<base>::timer_* (timer-apic.c). */
INLINE u32_t local_apic_timer_get (void)
{
    return *(volatile u32_t *)(APIC_MAPPINGS_START + X86_LAPIC_TIMER_CURRENT);
}

INLINE void local_apic_timer_set (u32_t count)
{
    *(volatile u32_t *)(APIC_MAPPINGS_START + X86_LAPIC_TIMER_COUNT) = count;
}

INLINE void local_apic_timer_setup (u8_t irq, bool periodic)
{
    *(volatile u32_t *)(APIC_MAPPINGS_START + X86_LAPIC_LVT_TIMER) =
	(u32_t) (((periodic ? 1 : 0) << 17) | irq);
}

INLINE void local_apic_timer_set_divisor (u32_t divisor)
{
    volatile u32_t *reg =
	(volatile u32_t *)(APIC_MAPPINGS_START + X86_LAPIC_TIMER_DIVIDE);
    u32_t val;
    divisor &= 0x7;
    val = *reg;
    val &= ~0xfu;
    if (divisor == 1)
	val = val | 0xb;
    else
    {
	divisor--;
	val = val | ((divisor << 1) & 0x8) | (divisor & 0x3);
    }
    *reg = val;
}

/* Mirrors local_apic_t<base>::disable(): clears spurious_int_vector_reg_t
   enabled (bit 8) and returns its previous value. */
INLINE bool local_apic_disable (void)
{
    volatile u32_t *svr = (volatile u32_t *)(APIC_MAPPINGS_START + X86_LAPIC_SVR);
    u32_t raw = *svr;
    bool enabled = (raw >> 8) & 1;
    *svr = raw & ~(1u << 8);
    return enabled;
}

/* Mirrors local_apic_t<base>::read_vector(): LVT registers start at 0x320
   (APIC_LVT_TIMER) and are 0x10 apart, indexed by the LAPIC_LVT_* values. */
INLINE u32_t local_apic_read_vector (word_t lvt)
{
    return *(volatile u32_t *)(APIC_MAPPINGS_START + 0x320 + (lvt * 0x10));
}

/* Mirrors local_apic_t<base>::send_nmi().  command_reg_t bit positions:
   vector 0:7, delivery_mode 8:10, destination_mode 11, delivery_status 12,
   level 14, trigger_mode 15, destination 18:19. */
INLINE void local_apic_send_nmi (u8_t apic_id)
{
    volatile u32_t *cmd1 = (volatile u32_t *)(APIC_MAPPINGS_START + X86_LAPIC_INTR_CMD1);
    volatile u32_t *cmd2 = (volatile u32_t *)(APIC_MAPPINGS_START + X86_LAPIC_INTR_CMD2);
    u32_t reg = *cmd1;
    if (reg & (1u << 12))		/* delivery_status */
	return;

    *cmd2 = (u32_t) apic_id << (56 - 32);
    reg = *cmd1;
    reg &= ~0xffu;			/* vector = 0 */
    reg = (reg & ~(0x7u << 8)) | (4u << 8);   /* delivery_mode = nmi (4) */
    reg &= ~(1u << 11);			/* destination_mode = 0 */
    reg &= ~(0x3u << 18);		/* destination = 0 */
    reg |= (1u << 14);			/* level = 1 */
    reg |= (1u << 15);			/* trigger_mode = 1 */
    *cmd1 = reg;
}

/* Mirrors local_apic_t<base>::broadcast_nmi(); destination 2|1 = all-excluding-
   self, 2|0 = all-including-self.  See send_nmi above for the bit positions. */
INLINE void local_apic_broadcast_nmi (bool self)
{
    volatile u32_t *cmd1 = (volatile u32_t *)(APIC_MAPPINGS_START + X86_LAPIC_INTR_CMD1);
    u32_t reg = *cmd1;
    if (reg & (1u << 12))		/* delivery_status */
	return;

    reg = 0;
    reg |= (4u << 8);			/* delivery_mode = nmi (4) */
    reg |= (1u << 11);			/* destination_mode = 1 */
    reg |= ((u32_t) (2 | (self ? 0 : 1)) << 18);   /* destination */
    reg |= (1u << 14);			/* level = 1 */
    *cmd1 = reg;
}

INLINE void local_apic_send_ipi (u8_t apic_id, u8_t vector)
{
    volatile u32_t *cmd1 = (volatile u32_t *)(APIC_MAPPINGS_START + X86_LAPIC_INTR_CMD1);
    volatile u32_t *cmd2 = (volatile u32_t *)(APIC_MAPPINGS_START + X86_LAPIC_INTR_CMD2);
    if (*cmd1 & (1u << 12))		/* command_reg_t::delivery_status */
	return;
    *cmd2 = (u32_t) apic_id << 24;	/* destination in high dword bits 56:56 */
    *cmd1 = vector;			/* raw = 0 with vector in low 8 bits */
}

/* C forms mirroring the local_apic_t<base> methods used by platform/generic/
   intctrl-apic.c.  Register offsets are the regno_t enum values; bit positions
   match the reg-struct bitfields (SVR vector[0:7]/enabled[8]/focus[9],
   DEST_FORMAT model[28:31], PRIO subprio[0:3]/prio[4:7], VERSION version[0:7],
   lint_vector masked[16]). */
INLINE u8_t local_apic_id (void)
{ return (u8_t) (*(volatile u32_t *)(APIC_MAPPINGS_START + 0x020) >> 24); }

INLINE void local_apic_set_id (u8_t id)
{
    volatile u32_t *r = (volatile u32_t *)(APIC_MAPPINGS_START + 0x020);
    *r = (*r & 0x00ffffff) | ((u32_t) id << 24);
}

INLINE u8_t local_apic_version (void)
{ return (u8_t) (*(volatile u32_t *)(APIC_MAPPINGS_START + 0x030) & 0xff); }

INLINE void local_apic_set_task_prio (u8_t prio, u8_t subprio)
{
    volatile u32_t *r = (volatile u32_t *)(APIC_MAPPINGS_START + 0x080);
    *r = (*r & ~(u32_t) 0xff) | (((u32_t) (prio & 0xf)) << 4) | (u32_t) (subprio & 0xf);
}

/* lvt_t enum values */
#define LAPIC_LVT_TIMER			0
#define LAPIC_LVT_THERMAL_MONITOR	1
#define LAPIC_LVT_PERFCOUNT		2
#define LAPIC_LVT_LINT0			3
#define LAPIC_LVT_LINT1			4
#define LAPIC_LVT_ERROR			5

INLINE void local_apic_mask_lvt (word_t lvt)
{
    volatile u32_t *r = (volatile u32_t *)(APIC_MAPPINGS_START + 0x320 + (lvt * 0x10));
    *r |= (1u << 16);
}

INLINE bool local_apic_enable (u8_t spurious_int_vector)
{
    if ((local_apic_version () & 0xf0) != 0x10)
	return false;
    if ((spurious_int_vector & 0xf) != 0xf)
	if (local_apic_version () != 0x14)
	    return false;
    /* SVR: set enabled, clear focus_processor, set vector */
    volatile u32_t *svr = (volatile u32_t *)(APIC_MAPPINGS_START + 0x0F0);
    *svr = (*svr & ~(u32_t) 0x3ff) | (1u << 8) | (u32_t) spurious_int_vector;
    /* DEST_FORMAT: flat model */
    volatile u32_t *dest = (volatile u32_t *)(APIC_MAPPINGS_START + 0x0E0);
    *dest = (*dest & ~((u32_t) 0xf << 28)) | ((u32_t) 0xf << 28);
    return true;
}

INLINE void local_apic_error_setup (u8_t irq)
{
    *(volatile u32_t *)(APIC_MAPPINGS_START + 0x370) = irq;	/* LVT_ERROR */
    *(volatile u32_t *)(APIC_MAPPINGS_START + 0x280) = 0;	/* ERR_STATUS */
    *(volatile u32_t *)(APIC_MAPPINGS_START + 0x280) = 0;
}

INLINE word_t local_apic_read_error (void)
{
    *(volatile u32_t *)(APIC_MAPPINGS_START + 0x280) = 0;
    return *(volatile u32_t *)(APIC_MAPPINGS_START + 0x280);
}

/* command_reg_t: vector[0:7] delivery_mode[8:10] destination_mode[11]
   delivery_status[12] level[14] trigger_mode[15] destination[18:19].
   The writable fields below are cleared then re-set; the rest is preserved. */
#define __LAPIC_CMD_FIELDS	((u32_t) 0xCCFFF)

INLINE void local_apic_send_init_ipi (u8_t apic_id, bool assert)
{
    *(volatile u32_t *)(APIC_MAPPINGS_START + 0x310) = ((u32_t) apic_id) << (56 - 32);
    volatile u32_t *cmd1 = (volatile u32_t *)(APIC_MAPPINGS_START + 0x300);
    u32_t raw = *cmd1 & ~__LAPIC_CMD_FIELDS;
    raw |= (5u << 8)			/* delivery_mode = init */
	 | ((assert ? 1u : 0u) << 14)	/* level */
	 | (1u << 15);			/* trigger_mode */
    *cmd1 = raw;
}

INLINE void local_apic_send_startup_ipi (u8_t apic_id, void (*startup_func)(void))
{
    *(volatile u32_t *)(APIC_MAPPINGS_START + 0x310) = ((u32_t) apic_id) << (56 - 32);
    volatile u32_t *cmd1 = (volatile u32_t *)(APIC_MAPPINGS_START + 0x300);
    u32_t raw = *cmd1 & ~__LAPIC_CMD_FIELDS;
    /* the AP starts at 0x000VV000, where VV is sent with the SIPI */
    raw |= ((((u32_t) (word_t) startup_func) >> 12) & 0xff)
	 | (6u << 8);			/* delivery_mode = startup */
    *cmd1 = raw;
}

#endif /* !__ARCH__X86__APIC_H__ */
