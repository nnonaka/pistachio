/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     src/platform/ofppc/intctrl.h
 * Description:   
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
 * $Id$
 *                
 ********************************************************************/

#ifndef __PLATFORM__OFPPC__OPIC_H__
#define __PLATFORM__OFPPC__OPIC_H__

#include INC_PLAT(1275tree.h)

#include INC_API(smp.h)

#define OPIC_MAX_SOURCES	2048
#define OPIC_MAX_CPUS		32
#define OPIC_MAX_ISU		16

#define OPIC_NUM_TIMERS		4
#define OPIC_NUM_IPI		4
#define OPIC_NUM_PRI		16
#define OPIC_NUM_VECTORS	256

#define OPIC_REG_ALIGN			16
#define OPIC_CPU_DISABLE		0
#define OPIC_TIMER_FIELDS		4
#define OPIC_SOURCE_FIELDS		2

#define OPIC_GLOBAL_OFFSET	(0x1000)
#define OPIC_REG(d)		((d)*OPIC_REG_ALIGN + OPIC_GLOBAL_OFFSET)
#define OPIC_FEATURE_REPORTING0_REG	OPIC_REG(0)
#define OPIC_FEATURE_REPORTING1_REG	OPIC_REG(1)
#define OPIC_GLOBAL_CONFIG0_REG		OPIC_REG(2)
#define OPIC_GLOBAL_CONFIG1_REG		OPIC_REG(3)
#define OPIC_VENDER0_REG		OPIC_REG(4)
#define OPIC_VENDER1_REG		OPIC_REG(5)
#define OPIC_VENDER2_REG		OPIC_REG(6)
#define OPIC_VENDER3_REG		OPIC_REG(7)
#define OPIC_VENDER_IDENT_REG		OPIC_REG(8)
#define OPIC_CPU_INIT_REG		OPIC_REG(9)
#define OPIC_IPI0_PRIORITY_REG		OPIC_REG(10)
#define OPIC_IPI1_PRIORITY_REG		OPIC_REG(11)
#define OPIC_IPI2_PRIORITY_REG		OPIC_REG(12)
#define OPIC_IPI3_PRIORITY_REG		OPIC_REG(13)
#define OPIC_SPURIOUS_REG		OPIC_REG(14)
#define OPIC_TIMER_FREQUENCY_REG	OPIC_REG(15)
#define OPIC_TIMER0_CURRENT_COUNT_REG	OPIC_REG(16)
#define OPIC_TIMER0_BASE_COUNT_REG	OPIC_REG(17)
#define OPIC_TIMER0_VECTOR_REG		OPIC_REG(18)
#define OPIC_TIMER0_CPU_REG		OPIC_REG(19)

#define OPIC_SRC_OFFSET		(0xf000 + OPIC_GLOBAL_OFFSET)
#define OPIC_SRC_TOT_SIZE	(OPIC_SOURCE_FIELDS * OPIC_MAX_SOURCES * OPIC_REG_ALIGN)
#define OPIC_SRC_REG(d)		((d)*OPIC_REG_ALIGN + OPIC_SRC_OFFSET)
#define OPIC_SRC0_VECTOR_REG		OPIC_SRC_REG(0)
#define OPIC_SRC0_CPU_REG		OPIC_SRC_REG(1)

#define OPIC_CPU_OFFSET		(OPIC_SRC_OFFSET + OPIC_SRC_TOT_SIZE)
#define OPIC_CPU_SIZE		(0x1000)
#define OPIC_CPU_REG(d,cpu)	((d)*OPIC_REG_ALIGN + OPIC_CPU_OFFSET + cpu*OPIC_CPU_SIZE)
#define OPIC_CPU_IPI_DISPATCH0_REG(cpu)		OPIC_CPU_REG(4,cpu)
#define OPIC_CPU_IPI_DISPATCH1_REG(cpu)		OPIC_CPU_REG(5,cpu)
#define OPIC_CPU_IPI_DISPATCH2_REG(cpu)		OPIC_CPU_REG(6,cpu)
#define OPIC_CPU_IPI_DISPATCH3_REG(cpu)		OPIC_CPU_REG(7,cpu)
#define OPIC_CPU_CURRENT_PRIORITY_REG(cpu)	OPIC_CPU_REG(8,cpu)
#define OPIC_CPU_IRQ_ACK_REG(cpu)		OPIC_CPU_REG(10,cpu)
#define OPIC_CPU_EOI_REG(cpu)			OPIC_CPU_REG(11,cpu)

/* Each of these was a class with nothing but the union in it. */
#define OPIC_REG_STRUCT(name, fields)			\
    struct name {					\
	union {						\
	    struct { fields } x;			\
	    u32_t raw;					\
	};						\
    };							\
    typedef struct name name

OPIC_REG_STRUCT(opic_global_config0_t,
    BITFIELD5( u32_t,
	base 		: 20,
	unknown0	: 9,
	disable_8259	: 1,
	unknown1	: 1,
	reset		: 1
	);
);

OPIC_REG_STRUCT(opic_feature0_t,
    BITFIELD5( u32_t,
	version		: 8,
	last_cpu	: 5,
	unknown0	: 3,
	last_source	: 11,
	unknown1	: 5
	);
);

OPIC_REG_STRUCT(opic_vector_priority_t,
    BITFIELD9( u32_t,
	vector		: 8,
	unknown0	: 8,
	priority	: 4,
	unknown1	: 2,
	level		: 1,
	positive	: 1,
	unknown2	: 6,
	activity	: 1,
	mask		: 1
	);
);

OPIC_REG_STRUCT(opic_cpu_priority_t,
    BITFIELD2( u32_t,
	priority : 4,
	unknown : 28
	);
);

OPIC_REG_STRUCT(opic_irq_ack_t,
    BITFIELD2( u32_t,
	vector : 8,
	unknown : 24
	);
);

OPIC_REG_STRUCT(opic_timer_count_t,
    BITFIELD2( u32_t,
	count : 31,
	disabled : 1
	);
);

#undef OPIC_REG_STRUCT

/* Class-scoped upstream as intctrl_t::timer0_vec and so on; at file scope
   here.  The names are distinctive enough that nothing collides. */
enum priority_e {
    priority_timer	= 1,
    priority_std_source	= 8,
    priority_std_ipi	= 8,
    priority_spurious	= 15
};

enum vector_e {
    timer0_vec		= 0,
    timer1_vec		= 1,
    timer2_vec		= 2,
    timer3_vec		= 3,
    timer_end_vec	= timer3_vec,
    source_start_vec	= timer_end_vec+1,
    source_end_vec	= 253,
    ipi0_vec		= source_end_vec+1,
    ipi_end_vec		= ipi0_vec,
    spurious_vec	= 255
};

/* generic_intctrl_t was an interface-description base with no members, so as
   in platform/ppc44x the struct stands alone and the members become free
   intctrl_* entry points.  Everything upstream of platform/ofppc/intctrl.h
   calls the same contract bic.h and uic.h expose. */
struct intctrl_t
{
    word_t opic_vaddr;
    word_t opic_paddr;
    word_t opic_size;

    word_t last_vector;
    word_t num_cpus;
};
typedef struct intctrl_t intctrl_t;

INLINE intctrl_t * get_interrupt_ctrl (void)
{
    extern intctrl_t intctrl;
    return &intctrl;
}

/**
 * Little-endian I/O write to an open-pic register.
 * @param reg	The register offset.
 * @param val	The big-endian value, which will be byte reversed when written.
 */
INLINE void opic_out32le (intctrl_t *self, word_t reg, u32_t val)
{
    asm volatile( "stwbrx %0, 0, %1 ; eieio ;"
	    :
	    : "r" (val), "r" (reg + self->opic_vaddr) );
}

/**
 * Big-endian I/O write to an open-pic register.
 * @param reg	The register offset.
 * @param val	The value to write to the register.
 */
INLINE void opic_out32be (intctrl_t *self, word_t reg, u32_t val)
{
    asm volatile( "stw %0, 0(%1) ; eieio ;"
	    :
	    : "r" (val), "r" (reg + self->opic_vaddr) );
}

/**
 * Little-endian I/O read from an open-pic register.
 * @param reg	The register offset.
 *
 * The return value is converted to big-endian.
 */
INLINE u32_t opic_in32le (intctrl_t *self, word_t reg)
{
    u32_t val;
    asm volatile( "lwbrx %0, 0, %1 ; eieio ;"
	    : "=r" (val)
	    : "r" (reg + self->opic_vaddr) );
    return val;
}

/**
 * Big-endian I/O read from an open-pic register.
 * @param reg	The register offset.
 *
 * Upstream writes `lwz %0, 0, %1', which is not a form lwz has -- the indexed
 * load is lwzx, and the displacement form is lwz rD,d(rA), as opic_out32be's
 * stw correctly uses.  It has never been assembled: nothing calls this
 * function, in master or here.  Corrected to the displacement form to match
 * its own store counterpart, and left uncalled.  Notes §144.
 */
INLINE u32_t opic_in32be (intctrl_t *self, word_t reg)
{
    u32_t val;
    asm volatile( "lwz %0, 0(%1) ; eieio ;"
	    : "=r" (val)
	    : "r" (reg + self->opic_vaddr) );
    return val;
}

INLINE opic_feature0_t opic_get_feature0 (intctrl_t *self)
{
    opic_feature0_t t;
    t.raw = opic_in32le (self, OPIC_FEATURE_REPORTING0_REG);
    return t;
}

INLINE void opic_set_feature0 (intctrl_t *self, opic_feature0_t val)
{ opic_out32le (self, OPIC_FEATURE_REPORTING0_REG, val.raw); }

INLINE opic_global_config0_t opic_get_global_config0 (intctrl_t *self)
{
    opic_global_config0_t val;
    val.raw = opic_in32le (self, OPIC_GLOBAL_CONFIG0_REG);
    return val;
}

INLINE void opic_set_global_config0 (intctrl_t *self, opic_global_config0_t val)
{ opic_out32le (self, OPIC_GLOBAL_CONFIG0_REG, val.raw); }

INLINE opic_vector_priority_t opic_get_vector_priority (intctrl_t *self, word_t reg)
{
    opic_vector_priority_t t;
    t.raw = opic_in32le (self, reg);
    return t;
}

/* Defined out of line in opic.c: it is the only one of these that is not a
   single register access. */
void opic_write_vector_priority (intctrl_t *self, word_t reg, opic_vector_priority_t val);

INLINE void opic_set_reg_mask (intctrl_t *self, word_t reg, u32_t mask)
{
    opic_vector_priority_t t;

    t = opic_get_vector_priority (self, reg);
    t.x.mask = mask;
    opic_write_vector_priority (self, reg, t);
}

INLINE opic_cpu_priority_t opic_get_current_task_priority (intctrl_t *self, word_t cpu)
{
    opic_cpu_priority_t val;
    val.raw = opic_in32le (self, OPIC_CPU_CURRENT_PRIORITY_REG(cpu));
    return val;
}

INLINE void opic_set_current_task_priority (intctrl_t *self, u32_t priority, word_t cpu)
{
    opic_cpu_priority_t p;
    p.x.priority = priority;
    opic_out32le (self, OPIC_CPU_CURRENT_PRIORITY_REG(cpu), p.raw);
}

INLINE opic_irq_ack_t opic_get_irq_ack (intctrl_t *self, word_t cpu)
{
    opic_irq_ack_t val;
    val.raw = opic_in32le (self, OPIC_CPU_IRQ_ACK_REG(cpu));
    return val;
}

INLINE void opic_clear_eoi (intctrl_t *self, word_t cpu)
{ opic_out32le (self, OPIC_CPU_EOI_REG(cpu), 0); }

/* Timers. */

INLINE u32_t opic_get_timer_freq (intctrl_t *self)
{ return opic_in32le (self, OPIC_TIMER_FREQUENCY_REG); }

INLINE void opic_set_timer_freq (intctrl_t *self, u32_t freq)
{ opic_out32le (self, OPIC_TIMER_FREQUENCY_REG, freq); }

INLINE void opic_set_timer_vector_priority (intctrl_t *self, word_t timer, opic_vector_priority_t val)
{
    ASSERT( timer < OPIC_NUM_TIMERS );
    opic_write_vector_priority (self,
	    OPIC_TIMER0_VECTOR_REG + OPIC_REG_ALIGN*timer*OPIC_TIMER_FIELDS,
	    val);
}

INLINE opic_vector_priority_t opic_get_timer_vector_priority (intctrl_t *self, word_t timer)
{
    opic_vector_priority_t vp;

    ASSERT( timer < OPIC_NUM_TIMERS );
    vp.raw = opic_in32le (self,
	    OPIC_TIMER0_VECTOR_REG + OPIC_REG_ALIGN*timer*OPIC_TIMER_FIELDS);
    return vp;
}

INLINE void opic_set_timer_cpu (intctrl_t *self, word_t timer, u32_t cpu)
{
    ASSERT( timer < OPIC_NUM_TIMERS );
    opic_out32le (self,
	    OPIC_TIMER0_CPU_REG + OPIC_REG_ALIGN*timer*OPIC_TIMER_FIELDS,
	    cpu);
}

INLINE void opic_timer_set_count (intctrl_t *self, word_t timer, opic_timer_count_t val)
{
    ASSERT( timer < OPIC_NUM_TIMERS );
    opic_out32le (self,
	    OPIC_TIMER0_BASE_COUNT_REG + OPIC_REG_ALIGN*timer*OPIC_TIMER_FIELDS,
	    val.raw);
}

INLINE void opic_restart_timer (intctrl_t *self, word_t timer)
{
    opic_timer_count_t t;
    t.x.count = 0x1000;
    t.x.disabled = 0;
    opic_timer_set_count (self, timer, t);
}

INLINE u32_t opic_get_timer_count (intctrl_t *self, word_t timer)
{
    ASSERT( timer < OPIC_NUM_TIMERS );
    return opic_in32le (self,
	    OPIC_TIMER0_CURRENT_COUNT_REG +
	    OPIC_REG_ALIGN*timer*OPIC_TIMER_FIELDS);
}

INLINE void opic_mask_timer (intctrl_t *self, word_t timer)
{
    opic_set_reg_mask (self,
	    OPIC_TIMER0_VECTOR_REG + OPIC_REG_ALIGN*timer*OPIC_TIMER_FIELDS,
	    1);
}

INLINE void opic_unmask_timer (intctrl_t *self, word_t timer)
{
    opic_set_reg_mask (self,
	    OPIC_TIMER0_VECTOR_REG + OPIC_REG_ALIGN*timer*OPIC_TIMER_FIELDS,
	    0);
}

/* Sources. */

INLINE void opic_set_source_vector_priority (intctrl_t *self, word_t source, opic_vector_priority_t val)
{
    opic_write_vector_priority (self,
	    OPIC_SRC0_VECTOR_REG + source*OPIC_REG_ALIGN*OPIC_SOURCE_FIELDS,
	    val);
}

INLINE void opic_set_source_cpu (intctrl_t *self, word_t source, u32_t cpu)
{
    opic_out32le (self,
	    OPIC_SRC0_CPU_REG + source*OPIC_REG_ALIGN*OPIC_SOURCE_FIELDS,
	    cpu);
}

INLINE void opic_mask_source (intctrl_t *self, word_t source)
{
    opic_set_reg_mask (self,
	    OPIC_SRC0_VECTOR_REG + source*OPIC_REG_ALIGN*OPIC_SOURCE_FIELDS,
	    1);
}

INLINE void opic_unmask_source (intctrl_t *self, word_t source)
{
    opic_set_reg_mask (self,
	    OPIC_SRC0_VECTOR_REG + source*OPIC_REG_ALIGN*OPIC_SOURCE_FIELDS,
	    0);
}

/**************************************************************************/

INLINE bool opic_is_timer_vector (intctrl_t *self, word_t vector)
{
    return (vector >= timer0_vec) && (vector <= timer_end_vec);
}

INLINE bool opic_is_source_vector (intctrl_t *self, word_t vector)
{
    return (vector >= source_start_vec) && (vector <= self->last_vector);
}

/* L4 query functions. */

INLINE word_t intctrl_get_number_cpus (void)
{ return get_interrupt_ctrl()->num_cpus; }

INLINE word_t intctrl_get_number_irqs (void)
{ return get_interrupt_ctrl()->last_vector; }

INLINE bool intctrl_is_irq_available (word_t irq)
{ return irq < get_interrupt_ctrl()->last_vector; }

/* L4 modification functions. */

INLINE void intctrl_set_cpu (word_t irq, word_t cpu)
{
    intctrl_t *self = get_interrupt_ctrl();

    if( EXPECT_TRUE(opic_is_source_vector (self, irq)) )
	opic_set_source_cpu (self, irq - source_start_vec, cpu);

    else if( opic_is_timer_vector (self, irq) )
	opic_set_timer_cpu (self, irq - timer0_vec, cpu);

    else
	enter_kdebug( "unknown irq" );
}

INLINE void intctrl_mask (word_t irq)
{
    intctrl_t *self = get_interrupt_ctrl();

    if( EXPECT_TRUE(opic_is_source_vector (self, irq)) )
	opic_mask_source (self, irq - source_start_vec);

    else if( opic_is_timer_vector (self, irq) )
	opic_mask_timer (self, irq - timer0_vec);

    else
	enter_kdebug( "unknown irq" );
}

INLINE bool intctrl_unmask (word_t irq)
{
    intctrl_t *self = get_interrupt_ctrl();

    if( EXPECT_TRUE(opic_is_source_vector (self, irq)) )
	opic_unmask_source (self, irq - source_start_vec);

    else if( opic_is_timer_vector (self, irq) )
	opic_unmask_timer (self, irq - timer0_vec);

    else
	enter_kdebug( "unknown irq" );

    return false;
}

INLINE void intctrl_mask_and_ack (word_t irq)
{
    intctrl_mask (irq);
    opic_clear_eoi (get_interrupt_ctrl(), get_current_cpu());
}

INLINE void intctrl_ack (word_t irq)
{
    opic_clear_eoi (get_interrupt_ctrl(), get_current_cpu());
}

INLINE void intctrl_enable (word_t irq)		{ intctrl_unmask (irq); }
INLINE void intctrl_disable (word_t irq)	{ intctrl_mask (irq); }

#if defined(CONFIG_SMP)
/* cpu functions. */
INLINE void opic_send_ipi0 (intctrl_t *self, word_t src_cpu, word_t dst_cpu_mask)
{ opic_out32le (self, OPIC_CPU_IPI_DISPATCH0_REG(src_cpu), dst_cpu_mask); }
#endif

/* Defined in platform/ofppc/opic.c. */
void intctrl_init_arch (void);
void intctrl_init_cpu (word_t cpu);
void intctrl_bat_map (void);
void intctrl_handle_irq (word_t irq);
void intctrl_enable_timer (word_t timer);	/* debug */
#if defined(CONFIG_SMP)
void intctrl_start_new_cpu (word_t cpu);
#endif

#endif	/* __PLATFORM__OFPPC__OPIC_H__ */
