/*********************************************************************
 *                
 * Copyright (C) 1999-2010,  Karlsruhe University
 * Copyright (C) 2008-2009,  Volkmar Uhlig, IBM Corporation
 *                
 * File path:     platform/ppc44x/bic.cc
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
#include <debug.h>
#include <kdb/tracepoints.h>

#include <lib.h>
#include INC_GLUE(intctrl.h)
#include INC_ARCH(string.h)
#include INC_GLUE(space.h)
#include INC_PLAT(bic.h)
#include INC_PLAT(fdt.h)
#include INC_API(kernelinterface.h)

intctrl_t intctrl;

DECLARE_TRACEPOINT(SMP_IPI);

int bgic_get_pending_irq (bgic_t *self, word_t cpu)
{
    int group = -1, irq = -1;
    word_t hierarchy, mask = 0;
    
    hierarchy = self->core_non_crit[cpu];
    sync();

    // handle spurious interrupts...
    if (EXPECT_FALSE(hierarchy == 0))
	goto out;
    
    group = count_leading_zeros(hierarchy);

    if (EXPECT_FALSE(group >= BGP_MAX_GROUPS))
	goto out;
    
    mask = self->groups[group].noncrit_mask[cpu];
    sync();

    if (mask == 0)
	goto out;
    
    irq = bgic_group_to_irq(group) + count_leading_zeros(mask);

 out:
    return irq;
}

void bgic_dump (bgic_t *self)
{
    for (int i = 0; i < BGP_MAX_GROUPS; i++)
    {
	bgic_group_t *g = &self->groups[i];
	printf("%02x: [%p] S:%08x: T:[%08x %08x %08x %08x] M:[%08x %08x %08x %08x]\n",
	       i, g, g->status,
	       g->target_irq[0], g->target_irq[1], g->target_irq[2], g->target_irq[3],
	       g->noncrit_mask[0], g->noncrit_mask[1], g->noncrit_mask[2], g->noncrit_mask[3]);
    }
}

void SECTION (".init") intctrl_init_arch (void)
{
    intctrl_t *self = get_interrupt_ctrl();

    fdt_t *fdt = get_dtree();

    fdt_header_t *hdr = fdt_find_subtree (fdt, "/interrupt-controller");
    if (!hdr)
	panic("Couldn't find interrupt controller in FDT\n");

    fdt_property_t *prop = fdt_find_property_node_in (fdt, hdr, "compatible");

    if (!prop || strcmp(fdt_property_get_string (prop), "ibm,bgic") != 0)
	panic("BGIC: Couldn't find compatible node in FDT\n");

    prop = fdt_find_property_node_in (fdt, hdr, "reg");
    if (!prop || fdt_property_get_len (prop) != 3 * sizeof(u32_t))
	panic("BGIC: Couldn't find valid 'reg' node in FDT (%p, %d)\n", 
	      prop, fdt_property_get_len (prop));

    self->phys_addr = fdt_property_get_u64 (prop, 0);
    self->mem_size = fdt_property_get_word (prop, 2);

    prop = fdt_find_property_node_in (fdt, hdr, "interrupts");
    if (!prop || fdt_property_get_len (prop) != sizeof(u32_t))
	panic("BGIC: Couldn't find valid 'interrupts' node in FDT\n");
    
    self->num_irqs = fdt_property_get_word (prop, 0);
    if (self->num_irqs > BGP_MAX_IRQS)
	panic("BGIC: reported number IRQs of %d exceeds specification\n", self->num_irqs);

    TRACE_INIT("BGIC: %x:%x, %d interrupts\n", 
	       (word_t)(self->phys_addr >> 32), (word_t)self->phys_addr, self->num_irqs);

    // needs to be provided by glue
    intctrl_map ();

    if (!self->ctrl)
	panic("BGIC: mapping IRQ controller failed\n");

    TRACE_INIT("BGIC: remapped at %p\n", self->ctrl);

    bgic_mask_and_clear_all (self->ctrl);

    // route all IRQs to CPU0
    memset(self->routing, 0, sizeof(self->routing));
}

void SECTION(".init") intctrl_init_cpu (int cpu)
{
    intctrl_t *self = get_interrupt_ctrl();

    ASSERT(cpu < 4);

    /* map IPIs */
    intctrl_set_irq_routing (self, intctrl_get_ipi_irq (cpu, 0), cpu);
    intctrl_enable (intctrl_get_ipi_irq (cpu, 0));
}

void intctrl_handle_irq (word_t cpu)
{
    intctrl_t *self = get_interrupt_ctrl();

    int irq = bgic_get_pending_irq (self->ctrl, cpu);
    if (irq < 0 || irq > (int)intctrl_get_number_irqs ())
    {
        printf("spurious interrupt\n");
        return;
    }

#ifdef CONFIG_SMP
    if (irq < 32)
    {
        bgic_ack_irq (self->ctrl, irq);
        handle_smp_ipi(irq);
        return;
    }
#endif

    intctrl_mask (irq);
    handle_interrupt( irq );
}

void intctrl_map (void)
{
    intctrl_t *self = get_interrupt_ctrl();

    self->ctrl = (bgic_t*) space_map_device (get_kernel_space(), self->phys_addr,
					     self->mem_size, true, cache_inhibited);
}

void intctrl_start_new_cpu (word_t cpu)
{
    ASSERT(cpu < 4);
    
    extern word_t secondary_release_reloc;
    secondary_release_reloc = cpu;
}

void intctrl_send_ipi (word_t cpu)
{
    intctrl_t *self = get_interrupt_ctrl();

    TRACEPOINT(SMP_IPI, "irq %d cpu %d\n", intctrl_get_ipi_irq (cpu, 0), cpu);
    bgic_raise_irq (self->ctrl, intctrl_get_ipi_irq (cpu, 0));
}
