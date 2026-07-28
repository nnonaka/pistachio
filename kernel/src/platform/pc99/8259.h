/*********************************************************************
 *                
 * Copyright (C) 2002-2004, 2007,  Karlsruhe University
 *                
 * File path:     platform/pc99/8259.h
 * Description:   Driver for i8259 Programmable Interrupt Controller
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
 * $Id: 8259.h,v 1.10 2004/03/15 20:36:29 ud3 Exp $
 *                
 ********************************************************************/
#ifndef __PLATFORM__PC99__8259_H__
#define __PLATFORM__PC99__8259_H__

#include INC_ARCH(ioport.h)	/* for in_u8/out_u8	*/

/**
 * Driver for i8259 PIC
 *
 * Was `template<u16_t base> class i8259_pic_t'.  The template parameter gave
 * compile-time resolution of the control register addresses; in C the base is
 * an ordinary field, so out_u8 takes it in %dx rather than as an immediate.
 * There are exactly two instances (master 0x20, slave 0xa0), both touched only
 * on init/mask/ack paths, so the lost immediate costs nothing measurable.
 * See notes §117.
 *
 * Note:
 *   Depending on whether I8259_CACHE_PICSTATE is defined or not objects will
 *   cache the mask register or not.  Thus it is not wise to blindly
 *   instanciate them all over the place because the cached state would not be
 *   shared.  Intended use is a single object per PIC.
 *
 * Assumptions:
 * - base can be passed as port to in_u8/out_u8
 * - The PIC's A0=0 register is located at base
 * - The PIC's A0=1 register is located at base+1
 * - PICs in unbuffered cascade mode
 */

/* Enable PIC state caching */
#define I8259_CACHE_PICSTATE

struct i8259_pic_t {
    u16_t	base;
#if defined(I8259_CACHE_PICSTATE)
    u8_t	mask_cache;
#endif
};
typedef struct i8259_pic_t i8259_pic_t;

/*
 * The cached and uncached forms differ in where mask_cache lives: a field, or
 * a local read back from the port.  Reading it into a local named the same way
 * keeps the two bodies identical below, as the C++ did with its #if inside
 * each method.
 */
#if defined(I8259_CACHE_PICSTATE)
#define I8259_LOAD_MASK(self)	/* cached in self->mask_cache */
#define I8259_MASK(self)	((self)->mask_cache)
#else
#define I8259_LOAD_MASK(self)	u8_t __mask_cache = in_u8 ((self)->base + 1)
#define I8259_MASK(self)	__mask_cache
#endif

/**
 *	Unmask interrupt
 *	@param irq	interrupt line to unmask
 */
INLINE void i8259_unmask (i8259_pic_t *self, word_t irq)
{
    I8259_LOAD_MASK (self);
    I8259_MASK (self) &= (u8_t) ~(1 << irq);
    out_u8 (self->base + 1, I8259_MASK (self));
}

/**
 *	Mask interrupt
 *	@param irq	interrupt line to mask
 */
INLINE void i8259_mask (i8259_pic_t *self, word_t irq)
{
    I8259_LOAD_MASK (self);
    I8259_MASK (self) |= (u8_t) (1 << irq);
    out_u8 (self->base + 1, I8259_MASK (self));
}

/**
 *	Send specific EOI
 *	@param irq	interrupt line to ack
 */
INLINE void i8259_ack (i8259_pic_t *self, word_t irq)
{
    out_u8 (self->base, (u8_t) (0x60 + irq));
}

/**
 *  Check if interrupt is masked
 *  @param irq      interrupt line
 *  @return         true when masked, false otherwise
 */
INLINE bool i8259_is_masked (i8259_pic_t *self, word_t irq)
{
    I8259_LOAD_MASK (self);
    return (I8259_MASK (self) & (1 << irq)) != 0;
}

/**
 *	initialize PIC
 *	@param vector_base	8086-style vector number base
 *	@param slave_info	slave mask for master or slave id for slave
 *
 *	Initializes the PIC in 8086-mode:
 *  - not special-fully-nested mode
 *	- reporting vectors VECTOR_BASE...VECTOR_BASE+7
 *	- all inputs masked
 */
INLINE void i8259_init (i8259_pic_t *self, u16_t base, u8_t vector_base, u8_t slave_info)
{
    self->base = base;
    I8259_LOAD_MASK (self);
    I8259_MASK (self) = 0xFF;

    /*
      ICW1:
        0x10 | NEED_ICW4 | CASCADE_MODE | EDGE_TRIGGERED
    */
    out_u8 (self->base, 0x11);

    /*
      ICW2:
      - 8086 mode irq vector base
        PIN0->IRQ(base), ..., PIN7->IRQ(base+7)
    */
    out_u8 (self->base + 1, vector_base);

    /*
      ICW3:
       - master: slave list
         Set bits mark input PIN as connected to a slave
       - slave: slave id
         This PIC is connected to the master's pin SLAVE_ID
       Note: The caller knows whether its a master or not -
             the handling is the same.
    */
    out_u8 (self->base + 1, slave_info);

    /*
      ICW4:
        8086_MODE | NORMAL_EOI | NONBUFFERED_MODE | NOT_SFN_MODE
     */
    out_u8 (self->base + 1, 0x01); /* mode - *NOT* fully nested */

    /*
      OCW1:
       - set initial mask
    */
    out_u8 (self->base + 1, I8259_MASK (self));
}

#endif /* !__PLATFORM__PC99__8259_H__ */
