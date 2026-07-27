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

#if defined(__cplusplus)


template <word_t base> class local_apic_t
{

private:
    /* register numbers */
    enum regno_t {
	APIC_ID			=0x020,
	APIC_VERSION		=0x030,
	APIC_TASK_PRIO		=0x080,
	APIC_ARBITR_PRIO	=0x090,
	APIC_PROC_PRIO		=0x0A0,
	APIC_EOI		=0x0B0,
	APIC_LOCAL_DEST		=0x0D0,
	APIC_DEST_FORMAT	=0x0E0,
	APIC_SVR		=0x0F0,
	APIC_ISR_BASE		=0x100,
	APIC_TMR_BASE		=0x180,
	APIC_IRR_BASE		=0x200,
	APIC_ERR_STATUS		=0x280,
	APIC_INTR_CMD1		=0x300,
	APIC_INTR_CMD2		=0x310,
	APIC_LVT_TIMER		=0x320,
	APIC_LVT_THERMAL	=0x330,
	APIC_PERF_COUNTER	=0x340,
	APIC_LVT_LINT0		=0x350,
	APIC_LVT_LINT1		=0x360,
	APIC_LVT_ERROR		=0x370,
	APIC_TIMER_COUNT	=0x380,
	APIC_TIMER_CURRENT	=0x390,
	APIC_TIMER_DIVIDE	=0x3E0
    };
    /* raw register access */
    u32_t read_reg(regno_t reg) { return *((volatile u32_t*)(base + reg)); };
    void write_reg(regno_t reg, u32_t val) { *((volatile u32_t*)(base + reg)) = val; };

    class lint_vector_t
    {
    public:
	union {
	    u32_t raw;
	    struct {
		BITFIELD9(
		    u32_t,
		    vector		: 8,
		    delivery_mode	: 3,
		    reserved_0          : 1,
		    delivery_status	: 1,
		    polarity		: 1,
		    remote_irr		: 1,
		    trigger_mode	: 1,
		    masked		: 1,
		    reserved_1	: 15);
	    } x;
	};
    };

    class command_reg_t
    {
    public:
	union {
	    u32_t raw;
	    struct {
		u32_t vector		: 8;
		u32_t delivery_mode	: 3;
		u32_t destination_mode	: 1;
		u32_t delivery_status	: 1;
		u32_t reserved_0	: 1;
		u32_t level		: 1;
		u32_t trigger_mode	: 1;
		u32_t reserved_1	: 2;
		u32_t destination	: 2;
		u32_t reserved_2	: 12;
	    } x;
	};
    };

    class dest_format_reg_t
    {
    public:
	union {
	    u32_t raw;
	    struct {
		BITFIELD2(
		    u32_t,
		    reserved		: 28,
		    model		: 4);
	    } x;
	};
    };

    class prio_reg_t
    {
    public:
	union {
	    u32_t raw;
	    struct {
		BITFIELD3(
		    u32_t,
		    subprio		: 4,
		    prio		: 4,
		    reserved		: 24);
	    } x;
	};
    };

    class version_reg_t
    {
    public:
	union {
	    u32_t raw;
	    struct {
		BITFIELD4 (
		    u32_t,
		    version		: 8,
		    reserved_0		: 8,
		    max_lvt		: 8,
		    reserved_1		: 8);
	    } x;
	};
    };

    class spurious_int_vector_reg_t
    {
    public:
	union {
	    u32_t raw;
	    struct {
		BITFIELD4(
		    u32_t,
		    vector		: 8,
		    enabled		: 1,
		    focus_processor	: 1,
		    reserved		: 22);
	    } x;
	};
    };
    
    class esr_reg_t
    {
    public:
	union {
	    u32_t raw;
	    struct {
		u32_t tx_csum		: 1;
		u32_t rx_csum		: 1;
		u32_t tx_accept		: 1;
		u32_t rx_accept		: 1;
		u32_t tx_illegal_vector	: 1;
		u32_t rx_illegal_vector : 1;
		u32_t illegal_reg	: 1;
		u32_t reserved		: 25;
	    } x;
	};
    };

    void set_prio(regno_t reg, u8_t prio, u8_t subprio)
	{
	    prio_reg_t preg;
	    preg.raw = read_reg(reg);
	    /* prio and subprio are 4-bit fields in the APIC priority regs */
	    preg.x.prio = prio & 0xF;
	    preg.x.subprio = subprio & 0xF;
	    write_reg(reg, preg.raw);
	}

public:

    u8_t id() { return (u8_t) (read_reg(APIC_ID) >> 24); }
    void set_id(u8_t id) {
	write_reg(APIC_ID, (read_reg(APIC_ID) & 0x00ffffff) | (u32_t)id << 24);
    }
    u8_t version() {
	version_reg_t vreg;
	vreg.raw = read_reg(APIC_VERSION);
	return vreg.x.version;
    }

    bool enable(u8_t spurious_int_vector);
    bool disable();

    void set_task_prio(u8_t prio, u8_t subprio = 0)
	{ set_prio (APIC_TASK_PRIO, prio, subprio); }

    /* interrupt vectors */
    enum lvt_t
    {
	lvt_timer = 0,
	lvt_thermal_monitor = 1,
	lvt_perfcount = 2,
	lvt_lint0 = 3,
	lvt_lint1 = 4,
	lvt_error = 5
    };

    enum delivery_mode_t
    {
	fixed = 0,
	smi = 2,
	nmi = 4,
	init = 5,
	startup = 6,
	extint = 7
    };

    enum pin_polarity_t
    {
	active_high = 0,
	active_low = 1
    };

    enum trigger_mode_t
    {
	edge = 0,
	level = 1
    };

    bool set_vector(lvt_t lvt, delivery_mode_t del_mode,
		    pin_polarity_t polarity,
		    trigger_mode_t trigger_mode);
    
    
public:
    /* Timer handling */
    u32_t timer_get() {
	return read_reg(APIC_TIMER_CURRENT);
    }
    void timer_set(u32_t count) {
	write_reg(APIC_TIMER_COUNT, count);
    }
    void timer_setup(u8_t irq, bool periodic) {
	write_reg(APIC_LVT_TIMER,
		  (((periodic ? 1 : 0) << 17)) | irq);
    }
    void timer_set_divisor(u32_t divisor) {
	divisor &= 0x7;
	u32_t val = read_reg(APIC_TIMER_DIVIDE);
	val &= ~0xf;
	if (divisor == 1)
	    val = val | 0xb;
	else
	{
	    divisor--;
	    val = val | ((divisor << 1) & 0x8) | (divisor & 0x3);
	}
	write_reg(APIC_TIMER_DIVIDE, val);
    }

public:
    /* IRQ handling */
    void EOI()	{ write_reg(APIC_EOI, 0); }

    void mask(lvt_t lvt)
	{
	    lint_vector_t vec;
	    vec.raw = read_reg((regno_t)(APIC_LVT_TIMER + (lvt * 0x10)));
	    vec.x.masked = 1;
	    write_reg((regno_t)(APIC_LVT_TIMER + (lvt * 0x10)), vec.raw);
	}

    void unmask(lvt_t lvt)
	{
	    lint_vector_t vec;
	    vec.raw = read_reg((regno_t)(APIC_LVT_TIMER + (lvt * 0x10)));
	    vec.x.masked = 0;
	    write_reg((regno_t)(APIC_LVT_TIMER + (lvt * 0x10)), vec.raw);
	}

    bool get_trigger_mode(u8_t irq)
	{
	    word_t tmr = read_reg((regno_t)(APIC_TMR_BASE + ((irq & ~0x1f) >> 1)));
	    return tmr & (1 << (irq & 0x1f));
	}

    u32_t read_vector(lvt_t lvt)
	{
	    return read_reg((regno_t)(APIC_LVT_TIMER + (lvt * 0x10)));
	}

public:
    /* SMP handling */
    void send_startup_ipi(u8_t apic_id, void(*startup)(void));
    void send_init_ipi(u8_t apic_id, bool assert);
    void send_nmi(u8_t apic_id);
    void send_ipi(u8_t apic_id, u8_t vector);
    void broadcast_ipi(u8_t vector, bool self = false);
    void broadcast_nmi(bool self = false);
    

public:
    /* Error handling */
    void error_setup(u8_t irq) 
	{
	    write_reg((regno_t) APIC_LVT_ERROR,  irq);
	    write_reg((regno_t) APIC_ERR_STATUS, 0);
	    write_reg((regno_t) APIC_ERR_STATUS, 0);
	}
    
    word_t read_error()
	{
	    write_reg((regno_t) APIC_ERR_STATUS, 0);
	    return read_reg((regno_t) APIC_ERR_STATUS);  
	}
};


template <word_t base>
INLINE bool local_apic_t<base>::enable(u8_t spurious_int_vector)
{
    /* check that this is really an APIC */
    if ((version() & 0xf0) != 0x10)
	return false;

    /* only P4, Xeon, and Opteron support arbitrary SIV - check! */
    if ((spurious_int_vector & 0xf) != 0xf)
    {
	if (version() != 0x14)
	    return false;
    }

    /* enable and set spurious interrupt vector */
    spurious_int_vector_reg_t svr;
    svr.raw = read_reg(APIC_SVR);
    svr.x.enabled = 1;
    svr.x.focus_processor = 0;
    svr.x.vector = spurious_int_vector;
    write_reg(APIC_SVR, svr.raw);

    /* set to flat model -- other models dropped with P4 anyway */
    dest_format_reg_t dest;
    dest.raw = read_reg(APIC_DEST_FORMAT);
    dest.x.model = 0xf; // flat model
    write_reg(APIC_DEST_FORMAT, dest.raw);

    return true;
}

template <word_t base>
INLINE bool local_apic_t<base>::disable()
{
    spurious_int_vector_reg_t svr;
    svr.raw = read_reg(APIC_SVR);
    bool enabled = svr.x.enabled;
    svr.x.enabled = 0; 
    write_reg(APIC_SVR, svr.raw);
    return enabled;
}

template <word_t base>
INLINE void local_apic_t<base>::send_startup_ipi(u8_t apic_id, void(*startup_func)(void))
{
    // destination
    write_reg(APIC_INTR_CMD2, ((word_t)apic_id) << (56 - 32));
    command_reg_t reg;
    reg.raw = read_reg(APIC_INTR_CMD1);
    // the startup-address of the receiving processor is
    // 0x000VV000, where VV is sent with the SIPI.
    word_t startup_vector = (word_t) startup_func;
    reg.x.vector = ((u32_t)startup_vector) >> 12 & 0xff; ;
    reg.x.delivery_mode = startup;
    reg.x.destination_mode = 0;
    reg.x.destination = 0;
    reg.x.level = 0;
    reg.x.trigger_mode = 0;
    write_reg(APIC_INTR_CMD1, reg.raw);
    
}

template <word_t base>
INLINE void local_apic_t<base>::send_init_ipi(u8_t apic_id, bool assert)
{

    // destination
    write_reg(APIC_INTR_CMD2, ((word_t)apic_id) << (56 - 32));
    command_reg_t reg;
    reg.raw = read_reg(APIC_INTR_CMD1);
    
    reg.x.vector = 0;
    reg.x.delivery_mode = init;
    reg.x.destination_mode = 0;
    reg.x.destination = 0;
    reg.x.level = assert ? 1 : 0;
    reg.x.trigger_mode = 1;
    write_reg((regno_t) (APIC_INTR_CMD1), reg.raw);

}

template <word_t base>
INLINE void local_apic_t<base>::send_nmi(u8_t apic_id)
{
    command_reg_t reg;
    reg.raw = read_reg(APIC_INTR_CMD1);
    if (reg.x.delivery_status != 0) return;

    // destination
    write_reg(APIC_INTR_CMD2, ((word_t)apic_id) << (56 - 32));
    reg.raw = read_reg(APIC_INTR_CMD1);
    reg.x.vector = 0;
    reg.x.delivery_mode = nmi;
    reg.x.destination_mode = 0;
    reg.x.destination = 0;
    reg.x.level = 1;
    reg.x.trigger_mode = 1;
    write_reg(APIC_INTR_CMD1, reg.raw);
    
}


template <word_t base>
INLINE void local_apic_t<base>::send_ipi(u8_t apic_id, u8_t vector)
{
    command_reg_t reg;
    reg.raw = read_reg(APIC_INTR_CMD1);
    if (reg.x.delivery_status != 0) return;

    // destination
    write_reg(APIC_INTR_CMD2, ((word_t)apic_id) << (56 - 32));
    reg.raw = 0;
    reg.x.vector = vector;
    write_reg(APIC_INTR_CMD1, reg.raw);
 
}

template <word_t base>
INLINE void local_apic_t<base>::broadcast_ipi(u8_t vector, bool self)
{
    command_reg_t reg;
    reg.raw = read_reg(APIC_INTR_CMD1);
    if (reg.x.delivery_status != 0) return;
    
    reg.raw = 0;
    reg.x.destination_mode = 1;
    reg.x.destination = 2 | (self ? 0 : 1);
    reg.x.vector = vector;
    write_reg(APIC_INTR_CMD1, reg.raw);
}


template <word_t base>
INLINE void local_apic_t<base>::broadcast_nmi(bool self)
{
    command_reg_t reg;
    reg.raw = read_reg(APIC_INTR_CMD1);
    if (reg.x.delivery_status != 0) return;
    
    reg.raw = 0;
    reg.x.vector = 0;
    reg.x.delivery_mode = nmi;
    reg.x.destination_mode = 1;
    reg.x.destination = 2 | (self ? 0 : 1);
    reg.x.level = 1;
    write_reg(APIC_INTR_CMD1, reg.raw);
   
}

#endif /* __cplusplus */

/*
 * Minimal C API for the local APIC at the fixed kernel mapping, mirroring the
 * local_apic_t<APIC_MAPPINGS_START> template methods that C code (cpu.c) uses.
 * Register offsets and bit layout match the template's regno_t/command_reg_t.
 */
#if !defined(__cplusplus)
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
#endif /* !__cplusplus */

#endif /* !__ARCH__X86__APIC_H__ */
