/*********************************************************************
 *                
 * Copyright (C) 2010,  Karlsruhe Institute of Technology
 *                
 * Filename:      uic.h
 * Author:        Jan Stoess <stoess@kit.edu>
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
 ********************************************************************/
#ifndef __PLATFORM__PPC44X__UIC_H__
#define __PLATFORM__PPC44X__UIC_H__

#include <intctrl.h>
#include <sync.h>

//from Linux (arch/powerpc/include/asm/dcr-native.h)
#define mfdcr(rn) \
          ({      \
                  unsigned long rval; \
                  asm volatile("mfdcr %0,%1" : "=r"(rval) : "i"(rn)); \
                  rval; \
          })
#define mtdcr(rn, val) asm volatile("mtdcr %0,%1" : : "i"(rn), "r"(val))

/*
 * Universal Interrupt Controller register definitions. Each is a separate
 * DCR register.
 */

#define UIC0_DCR_BASE  0xc0
#define UIC1_DCR_BASE  0xd0
//FIXME: What is the correct base address for UIC2?

#define UIC0_SR        (UIC0_DCR_BASE+0x0)  /* UIC status                  */
#define UIC0_SRS       (UIC0_DCR_BASE+0x1)  /* UIC status register set     */
#define UIC0_ER        (UIC0_DCR_BASE+0x2)  /* UIC enable                  */
#define UIC0_CR        (UIC0_DCR_BASE+0x3)  /* UIC critical                */
#define UIC0_PR        (UIC0_DCR_BASE+0x4)  /* UIC polarity                */
#define UIC0_TR        (UIC0_DCR_BASE+0x5)  /* UIC triggering              */
#define UIC0_MSR       (UIC0_DCR_BASE+0x6)  /* UIC masked status           */
#define UIC0_VR        (UIC0_DCR_BASE+0x7)  /* UIC vector                  */
#define UIC0_VCR       (UIC0_DCR_BASE+0x8)  /* UIC vector configuration    */

#define UIC1_SR        (UIC1_DCR_BASE+0x0)  /* UIC status                  */
#define UIC1_SRS       (UIC1_DCR_BASE+0x1)  /* UIC status register set     */
#define UIC1_ER        (UIC1_DCR_BASE+0x2)  /* UIC enable                  */
#define UIC1_CR        (UIC1_DCR_BASE+0x3)  /* UIC critical                */
#define UIC1_PR        (UIC1_DCR_BASE+0x4)  /* UIC polarity                */
#define UIC1_TR        (UIC1_DCR_BASE+0x5)  /* UIC triggering              */
#define UIC1_MSR       (UIC1_DCR_BASE+0x6)  /* UIC masked status           */
#define UIC1_VR        (UIC1_DCR_BASE+0x7)  /* UIC vector                  */
#define UIC1_VCR       (UIC1_DCR_BASE+0x8)  /* UIC vector configuration    */

#if defined(PPC440EPx)
#define UIC2_SR        (UIC2_DCR_BASE+0x0)  /* UIC status                  */
#define UIC2_SRS       (UIC2_DCR_BASE+0x1)  /* UIC status register set     */
#define UIC2_ER        (UIC2_DCR_BASE+0x2)  /* UIC enable                  */
#define UIC2_CR        (UIC2_DCR_BASE+0x3)  /* UIC critical                */
#define UIC2_PR        (UIC2_DCR_BASE+0x4)  /* UIC polarity                */
#define UIC2_TR        (UIC2_DCR_BASE+0x5)  /* UIC triggering              */
#define UIC2_MSR       (UIC2_DCR_BASE+0x6)  /* UIC masked status           */
#define UIC2_VR        (UIC2_DCR_BASE+0x7)  /* UIC vector                  */
#define UIC2_VCR       (UIC2_DCR_BASE+0x8)  /* UIC vector configuration    */
#endif

#if defined(PPC440EPx)
#define INT_LEVEL_MAX 74   /* the UIC has 75 interrupt sources */
#define INT_LEVEL_MIN 0    /* 0-31 in UIC0, 32-63 in UIC1, 64-77 in UIC2     */
#else
#define INT_LEVEL_MAX 63   /* the UIC has 64 interrupt sources */
#define INT_LEVEL_MIN 0    /* 0-31 in UIC0, 32-63 in UIC1      */
#endif

#define INT_LEVEL_UIC0_MIN   0    /* First interrupt level on UIC-0 */
#define INT_LEVEL_UIC0_MAX  31    /* Last interrupt level on UIC-0 */
#define UIC0_NUM_IRQS (INT_LEVEL_UIC0_MAX - INT_LEVEL_UIC0_MIN + 1)
#define INT_LEVEL_UIC1_MIN  32    /* First interrupt level on UIC-1 */
#define INT_LEVEL_UIC1_MAX  63    /* Last interrupt level on UIC-1 */
#define UIC1_NUM_IRQS (INT_LEVEL_UIC1_MAX - INT_LEVEL_UIC1_MIN + 1)
#if defined(PPC440EPx)
#define INT_LEVEL_UIC2_MIN  64    /* First interrupt level on UIC-2 */
#define INT_LEVEL_UIC2_MAX  74    /* Last interrupt level on UIC-2 */
#define UIC2_NUM_IRQS (INT_LEVEL_UIC2_MAX - INT_LEVEL_UIC2_MIN + 1)

#endif

#define INT_IS_CRT      (true)         /* Int. is critical (for handler) */
#define INT_IS_NOT_CRT  (!INT_IS_CRT)  /* Int. is not critical */


#define VEC_TO_BIT_SHIFT    31      /* max shift when setting bits */
#define VEC_TO_MSK_SHIFT    32      /* max shift when masking bits */

#define vecToUicBit(v)      (0x00000001 << (VEC_TO_BIT_SHIFT - (v)))
#define vecToUicMask(v)     (0xffffffff << (VEC_TO_MSK_SHIFT - (v)))

#define BGP_MAX_CORE	4
#define BGP_MAX_GROUPS	15
#define BGP_MAX_IRQS	(BGP_MAX_GROUPS * 32)

//all interrupts are non-critical for now
#define UIC0_INTR_CRITICAL 0x00000000
#define UIC1_INTR_CRITICAL 0x00000000
#if defined(PPC440EPx)
#define UIC2_INTR_CRITICAL 0x00000000
#endif

#define UIC0_INTR_POLARITY 0xffffffff
#define UIC1_INTR_POLARITY 0xffffffff
#if defined(PPC440EPx)
#define UIC2_INTR_POLARITY 0xffffffff
#endif

#define UIC0_INTR_TRIGGER 0x00000000
#define UIC1_INTR_TRIGGER 0x00000000
#if defined(PPC440EPx)
#define UIC2_INTR_TRIGGER 0x00000000
#endif

/* generic_intctrl_t was an interface-description base with no members; it is
   gone, so the struct stands alone, exactly as bic.h's does.  The two headers
   are alternative definitions of the same object and the same intctrl_* entry
   points -- platform/ppc44x/intctrl.h picks one by subplatform -- so the
   surface here is kept name-for-name with bic.h's. */
struct intctrl_t
{
    /* we can route to 4 CPUs, and thus can encode 16 targets in a word */
    u8_t routing[BGP_MAX_IRQS / 4];	/* 4 IRQs per byte */
    spinlock_t lock;
    word_t num_irqs;
    word_t mem_size;
    word_t uic1_dchain_mask;
#if defined(PPC440EPx)
    word_t uic2_dchain_mask;
#endif
};
typedef struct intctrl_t intctrl_t;

/* Defined here rather than in glue/v4-powerpc/intctrl.h, which includes this
   header before it would get to the definition -- the inline entry points
   below need it. */
INLINE intctrl_t * get_interrupt_ctrl (void)
{
    extern intctrl_t intctrl;
    return &intctrl;
}

INLINE word_t intctrl_get_irq_routing (intctrl_t *self, word_t irq)
{
    word_t shift = (irq % 4) * 2;
    return (self->routing[irq / 4] >> shift) & 3;
}

INLINE void intctrl_set_irq_routing (intctrl_t *self, word_t irq, word_t cpu)
{
    word_t shift = (irq % 4) * 2;
    self->routing[irq / 4] = (self->routing[irq / 4] & ~(3 << shift)) | (cpu << shift);
}

INLINE word_t intctrl_get_ipi_irq (word_t cpu, word_t ipi)
{ return cpu * 8 + ipi; }

INLINE word_t intctrl_get_number_irqs (void)
{ return INT_LEVEL_MAX + 1; }

INLINE bool intctrl_is_irq_available (word_t irq)
{ return irq >= INT_LEVEL_MIN && irq <= INT_LEVEL_MAX; }

/* Out of line in uic.c: each one needs mtdcr/mfdcr on a register selected at
   run time, so none of them collapses to a constant. */
void intctrl_mask (word_t irq);
bool intctrl_unmask (word_t irq);
bool intctrl_is_masked (word_t irq);
bool intctrl_is_pending (word_t irq);

INLINE void intctrl_enable (word_t irq)
{
    if (intctrl_unmask (irq))
	handle_interrupt (irq);
}

INLINE void intctrl_disable (word_t irq)
{ intctrl_mask (irq); }

/* Upstream returns is_masked, not its negation; bic.h has the same inversion.
   Preserved rather than fixed -- nothing in the tree calls it. */
INLINE bool intctrl_is_enabled (word_t irq)
{ return intctrl_is_masked (irq); }

INLINE void intctrl_set_cpu (word_t irq, word_t cpu)
{
    intctrl_set_irq_routing (get_interrupt_ctrl(), irq, cpu);
    if (!intctrl_is_masked (irq))
	intctrl_unmask (irq);
}

/* Defined in platform/ppc44x. */
void intctrl_init_arch (void);
void intctrl_init_cpu (int cpu);
void intctrl_handle_irq (word_t cpu);	/* handler invoked on interrupt */
void intctrl_map (void);		/* map routine provided by glue */
void intctrl_start_new_cpu (word_t cpu);	/* SMP support */
void intctrl_send_ipi (word_t cpu);
void intctrl_dump (void);		/* debug */

#endif /* !__PLATFORM__PPC44X__UIC_H__ */
