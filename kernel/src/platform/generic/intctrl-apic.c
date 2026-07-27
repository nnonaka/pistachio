/*********************************************************************
 *                
 * Copyright (C) 2002-2008, 2010, Karlsruhe University
 *                
 * File path:     platform/generic/intctrl-apic.c
 * Description:   Implementation of APIC+IOAPIC intctrl (with ACPI)
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
 * $Id: intctrl-apic.c,v 1.15 2007/03/20 14:20:48 stoess Exp $
 *                
 ********************************************************************/

#include <linear_ptab.h>
#include INC_API(tcb.h)
#include INC_API(cpu.h)

#include INC_GLUE(config.h)

#include INC_PLAT(acpi.h)

#include INC_ARCH(ioport.h)
#include INC_ARCH(trapgate.h)
#include INC_GLUE(idt.h)
#include INC_GLUE(space.h)
#include INC_GLUE(intctrl.h)
#include INC_GLUE(hwirq.h)

intctrl_t intctrl;

EXC_INTERRUPT(spurious_interrupt)
{
    printf("spurious interrupt\n");
}

HW_IRQ( 0); HW_IRQ( 1); HW_IRQ( 2); HW_IRQ( 3);
HW_IRQ( 4); HW_IRQ( 5); HW_IRQ( 6); HW_IRQ( 7);
HW_IRQ( 8); HW_IRQ( 9); HW_IRQ(10); HW_IRQ(11);
HW_IRQ(12); HW_IRQ(13); HW_IRQ(14); HW_IRQ(15);
HW_IRQ(16); HW_IRQ(17); HW_IRQ(18); HW_IRQ(19);
HW_IRQ(20); HW_IRQ(21); HW_IRQ(22); HW_IRQ(23);
HW_IRQ(24); HW_IRQ(25); HW_IRQ(26); HW_IRQ(27);
HW_IRQ(28); HW_IRQ(29); HW_IRQ(30); HW_IRQ(31);
HW_IRQ(32); HW_IRQ(33); HW_IRQ(34); HW_IRQ(35);
HW_IRQ(36); HW_IRQ(37); HW_IRQ(38); HW_IRQ(39);
HW_IRQ(40); HW_IRQ(41); HW_IRQ(42); HW_IRQ(43);
HW_IRQ(44); HW_IRQ(45); HW_IRQ(46); HW_IRQ(47);
HW_IRQ(48); HW_IRQ(49);

HW_IRQ_COMMON();

EXC_INTERRUPT(spurious_interrupt_lapic)
{
    printf("spurious lapic interrupt\n");
    local_apic_eoi();
}

EXC_INTERRUPT(spurious_interrupt_ioapic)
{
    printf("spurious ioapic interrupt\n");
    local_apic_eoi();
}

EXC_INTERRUPT(lapic_error_interrupt)
{
    printf("lapic error interrupt (error=%x)\n", local_apic_read_error());
    enter_kdebug("lapic error");
}


typedef void(*hwirqfunc_t)(void);
INLINE hwirqfunc_t get_interrupt_entry(word_t irq)
{
    return (hwirqfunc_t)((word_t)hwirq_0 + ((word_t)hwirq_1 - (word_t)hwirq_0) * irq);
}

static u8_t intctrl_t_setup_idt_entry(intctrl_t *self, word_t irq, u8_t prio)
{
    spinlock_lock(&self->idt_lock);
    /* Compute in full width: truncating to u8_t before the range check below
       would let a large irq wrap around into the accepted range. */
    word_t vector = IDT_IOAPIC_BASE + irq;
    if (vector < IDT_IOAPIC_MAX) {
	idt_add_gate(&idt, vector, IDT_TYPE_INTERRUPT, get_interrupt_entry(irq));
    } else
    {
	TRACEF("IRQ %d, vector=%d, prio=%d, entry=%p\n",
	       irq, vector, prio, get_interrupt_entry(irq));
	UNIMPLEMENTED();
	vector = 0;
    }
    spinlock_unlock(&self->idt_lock);
    return (u8_t) vector;
}

static void intctrl_t_free_idt_entry(intctrl_t *self, word_t irq, u8_t vector)
{
    /* nothing so far--free ITD entry later */
    (void) self; (void) irq; (void) vector;
}


static void intctrl_t_sync_redir_entry(intctrl_redir_table_t *entry, word_t part)
{
    ASSERT(entry && entry->ioapic != NULL);
    spinlock_lock(&entry->ioapic->lock);
    if (part == INTCTRL_SYNC_ALL)
	i82093_set_redir_entry(entry->ioapic->i82093, entry->line, entry->entry);
    else if (part == INTCTRL_SYNC_LOW)
	i82093_set_redir_entry_low(entry->ioapic->i82093, entry->line, entry->entry);
    else {
	/* part == sync_high */
	i82093_set_redir_entry_high(entry->ioapic->i82093, entry->line, entry->entry);
    }
    spinlock_unlock(&entry->ioapic->lock);
}

static bool intctrl_t_init_io_apic(intctrl_t *self, word_t idx, word_t id,
				   word_t irq_base, addr_t paddr)
{
    if (idx >= CONFIG_MAX_IOAPICS)
	return false;

    space_add_mapping(get_kernel_space_c(), (addr_t)(IOAPIC_MAPPING(idx)),
		      paddr,
		      APIC_PGENTSZ,
		      true,	/* writable */
		      true,	/* kernel */
		      true,	/* global */
		      false);	/* uncacheable */

    /* reserve in KIP */
    memory_info_insert(&get_kip()->memory_info, MEMDESC_RESERVED, 0, false,
		       paddr, addr_offset(paddr, X86_PAGE_SIZE));

    intctrl_ioapic_t *ioapic = &self->ioapics[idx];
    /* ioapic_t::init */
    ioapic->id = id;
    ioapic->i82093 = (i82093_t *)(addr_t)(IOAPIC_MAPPING(idx));
    spinlock_init(&ioapic->lock, 0);

    word_t numirqs = i82093_num_irqs(ioapic->i82093);
    TRACE_INIT("\t        realid=%d maxint=%d, version=%d\n",
	       i82093_id(ioapic->i82093), numirqs,
	       i82093_version(ioapic->i82093).ver.version);

    /* VU: we initialize all IO-APIC interrupts. By default all
     *     IRQs are steered to local apic 0. The kernel re-routes
     *     them later on via set_cpu(...)
     */
    for (word_t i = 0; i < numirqs; i++)
    {
	/* ioapic_redir_table_t::set */
	self->redir[irq_base + i].ioapic = ioapic;
	self->redir[irq_base + i].line = i;
	self->redir[irq_base + i].pending = false;
	ioapic_redir_t *entry = &self->redir[irq_base + i].entry;

	/* ISA IRQs are mapped 1:1 --> 0 to 15 */
	if (irq_base + i < 16)
	    ioapic_redir_set_fixed_hwirq(entry, (u32_t) (IDT_IOAPIC_BASE + irq_base + i),
					 false,	/* high active */
					 false,	/* edge triggered */
					 true,	/* masked */
					 0);	/* apic 0 */
	else
	    ioapic_redir_set_fixed_hwirq(entry, (u32_t) (IDT_IOAPIC_BASE + irq_base + i),
					 true,	/* low active */
					 true,	/* level triggered */
					 true,	/* masked */
					 0);	/* apic 0 */
    }

    /* update the interrupt sources */
    self->num_intsources += numirqs;
    if ((irq_base + numirqs - 1) > self->max_intsource)
	self->max_intsource = irq_base + numirqs - 1;
    return true;
}

static void intctrl_t_init_local_apic(void)
{
    TRACE_INIT("\tlocal APIC id=%d, version=%d\n",
	       local_apic_id(), local_apic_version());

    if (!local_apic_enable(IDT_LAPIC_SPURIOUS_INT))
	WARNING ("failed initializing local APIC\n");

    local_apic_set_task_prio(0, 0);

    /* now disable all interrupts */
    local_apic_mask_lvt(LAPIC_LVT_TIMER);
    local_apic_mask_lvt(LAPIC_LVT_PERFCOUNT);
    local_apic_mask_lvt(LAPIC_LVT_LINT0);
    local_apic_mask_lvt(LAPIC_LVT_LINT1);
    local_apic_mask_lvt(LAPIC_LVT_ERROR);

#if defined(CONFIG_CPU_X86_P4)
    local_apic_mask_lvt(LAPIC_LVT_THERMAL_MONITOR);
#endif

    TRACE_INIT("\tlocal APIC error trap gate %d \n", IDT_LAPIC_ERROR);
    idt_add_gate(&idt, IDT_LAPIC_ERROR, IDT_TYPE_INTERRUPT, lapic_error_interrupt);
    local_apic_error_setup(IDT_LAPIC_ERROR);
}

word_t intctrl_t_get_number_irqs(intctrl_t *self)
{
    return self->max_intsource + 1;
}

void intctrl_t_init_arch(intctrl_t *self)
{
    /* first initialize object */
    for (word_t i = 0; i < NUM_REDIR_ENTRIES; i++)
	self->redir[i].ioapic = NULL;	/* mark entry invalid */

    self->max_intsource = 0;
    self->num_intsources = 0;
    self->num_ioapics = 0;

    /* mask all IRQs on PIC1 and PIC2 */
    out_u8(0x21, 0xff);
    out_u8(0xa1, 0xff);

    /* setup spurious interrupt vector */
    idt_add_gate(&idt, IDT_LAPIC_SPURIOUS_INT, IDT_TYPE_INTERRUPT, spurious_interrupt_lapic);
    idt_add_gate(&idt, IDT_IOAPIC_SPURIOUS, IDT_TYPE_INTERRUPT, spurious_interrupt_ioapic);

    /* now walk the ACPI structure */
    addr_t addr = acpi_remap((addr_t)ACPI20_PC99_RSDP_START);
    acpi_rsdp_t* rsdp = acpi_rsdp_locate(addr);
    TRACE_INIT("  Parsing ACPI tables\n", rsdp);

    TRACE_INIT("\tRSDP is at %p\n", rsdp);

    if (rsdp == NULL)
    {
	TRACE_INIT("\tAssuming local APIC defaults");
	space_add_mapping(get_kernel_space_c(), (addr_t)(APIC_MAPPINGS_START),
			  (addr_t)(0xFEE00000),
			  APIC_PGENTSZ, true, true, true, false);

	if (local_apic_version() >= 0x20)
	    panic("no local APIC found--system unusable");

	/* reserve in KIP */
	memory_info_insert(&get_kip()->memory_info, MEMDESC_RESERVED, 0, false,
			   (addr_t)(APIC_MAPPINGS_START),
			   (addr_t)(APIC_MAPPINGS_START + X86_PAGE_SIZE));

	cpu_add_cpu(local_apic_id());
	return;
    }

    acpi_rsdt_t *rsdt = NULL, *rsdt_phys = acpi_rsdp_rsdt(rsdp);
    acpi_xsdt_t *xsdt = NULL, *xsdt_phys = acpi_rsdp_xsdt(rsdp);

    if ((rsdt_phys == NULL) && (xsdt_phys == NULL))
	return;

    TRACE_INIT("\tRSDT is at %p\n", rsdt_phys);
    TRACE_INIT("\tXSDT is at %p\n", xsdt_phys);

    acpi_fadt_t *fadt = NULL, *_fadt;
    acpi_madt_t *madt = NULL, *_madt;

    if (xsdt_phys != NULL)
    {
	xsdt = (acpi_xsdt_t*)acpi_remap(xsdt_phys);
	fadt = (acpi_fadt_t*) acpi_xsdt_find(xsdt, "FACP", (addr_t) xsdt_phys);
	madt = (acpi_madt_t*) acpi_xsdt_find(xsdt, "APIC", (addr_t) xsdt_phys);
    }
    if ((madt == NULL) && (rsdt_phys != NULL))
    {
	rsdt = (acpi_rsdt_t*)acpi_remap(rsdt_phys);
	acpi_rsdt_list(rsdt, rsdt_phys);
	fadt = (acpi_fadt_t*) acpi_rsdt_find(rsdt, "FACP", (addr_t) rsdt_phys);
	madt = (acpi_madt_t*) acpi_rsdt_find(rsdt, "APIC", (addr_t) rsdt_phys);
    }

    if (fadt)
    {
	_fadt = (acpi_fadt_t*)acpi_remap(fadt);
	TRACE_INIT("\tFADT is at %p (remap %p), pmtimer IO port %x\n",
		   fadt, _fadt, acpi_fadt_pmtimer_ioport(_fadt));

	self->pmtimer_available = true;
	self->pmtimer_ioport = acpi_fadt_pmtimer_ioport(_fadt);
    }
    else
	self->pmtimer_available = false;

    if (madt == NULL)
	return;

    _madt = (acpi_madt_t*)acpi_remap(madt);

    TRACE_INIT("\tMADT is at %p (remap %p), local APICs @ %p\n",
	       madt, _madt, _madt->local_apic_addr);

    TRACE_INIT("  Mapping local APICs at %p to %p\n",
	       _madt->local_apic_addr, APIC_MAPPINGS_START);
    space_add_mapping(get_kernel_space_c(), (addr_t)(APIC_MAPPINGS_START),
		      (addr_t)((word_t) _madt->local_apic_addr),
		      APIC_PGENTSZ, true, true, true, false);

    /* reserve in KIP */
    memory_info_insert(&get_kip()->memory_info, MEMDESC_RESERVED, 0, false,
		       (addr_t)(APIC_MAPPINGS_START),
		       (addr_t)(APIC_MAPPINGS_START + X86_PAGE_SIZE));

    x86_mmu_flush_tlb(false);

    /* local apic */
    {
	word_t total_cpus = 0;
	acpi_madt_lapic_t* p;
	for (int i = 0; ((p = acpi_madt_lapic(_madt, i)) != NULL); i++)
	{
	    TRACE_INIT("\tlocal APIC: apic_id=%d use=%s proc_id=%d\n",
		       p->id, p->flags.enabled ? "ok" : "disabled",
		       p->apic_processor_id);
	    if (p->flags.enabled) {
		cpu_add_cpu(p->id);
		total_cpus++;
	    }
	}
	TRACE_INIT("  Found %d active CPUs, boot CPU is %x\n",
		   total_cpus, local_apic_id());

	if (total_cpus > cpu_count)
	    printf("  WARNING: system has %d CPUs, but kernel supports %d\n",
		   total_cpus, cpu_count);
#ifndef CONFIG_SMP
	/* make sure the boot CPU is the first */
	cpu_set_id(cpu_get(0), local_apic_id());
#endif
    }

    TRACE_INIT("  Initializing IOAPICs\n");

    /* IO APIC */
    {
	acpi_madt_ioapic_t* p;

	for (int i = 0; ((p = acpi_madt_ioapic(_madt, i)) != NULL); i++)
	{
	    TRACE_INIT("\tIOAPIC: id=%d irq_base=%d addr=%p\n",
		       p->id, p->irq_base, p->address);
	    if ((p->address & page_mask(APIC_PGENTSZ)) != 0)
	    {
		TRACE_INIT("\t  APIC %d IS MISALIGNED (%p). Ignoring!\n",
			   i, p->address);
		continue;
	    }
	    if ( intctrl_t_init_io_apic(self, self->num_ioapics, p->id, p->irq_base,
					addr_offset(0, p->address)) )
		self->num_ioapics++;
	}
    }

    /* IRQ source overrides */
    {
	acpi_madt_irq_t* p;
	for (int i = 0; ((p = acpi_madt_irq(_madt, i)) != NULL); i++)
	{
	    TRACE_INIT("  IRQ source override: "
		       "srcbus=%d, srcirq=%d, dest=%d, "
		       "%s, trigger=%s \n",
		       p->src_bus, p->src_irq, p->dest,
		       acpi_madt_irq_get_polarity(p) == 0 ? "conform pol." :
		       acpi_madt_irq_get_polarity(p) == 1 ? "active high" :
		       acpi_madt_irq_get_polarity(p) == 3 ? "active low" : "reserved",
		       acpi_madt_irq_get_trigger_mode(p) == 0 ? "conform" :
		       acpi_madt_irq_get_trigger_mode(p) == 1 ? "edge" :
		       acpi_madt_irq_get_trigger_mode(p) == 3 ? "level" : "reserved");

	    /* source overrides only exist for ISA (see ACPI spec),
	     * hence conform means high active, edge triggered
	     * An override means that the original src irq is
	     * connected to dest. For example the timer is usually connected
	     * to IRQ 0, but in APIC mode it is redirected to IRQ 2.
	     * We do not eliminate the redirections but have to
	     * care about polarity.
	     */
	    ioapic_redir_t * entry = &self->redir[p->dest].entry;
	    entry->x.polarity =
		(acpi_madt_irq_get_polarity(p) == ACPI_MADT_ACTIVE_HIGH) ? 0 : 1;
	    entry->x.trigger_mode =
		(acpi_madt_irq_get_trigger_mode(p) == ACPI_MADT_LEVEL) ? 1 : 0;
	}
    }

    /* Any other tables ? */
    {
	acpi_madt_hdr_t* p;
	for (u8_t t = 3; t <= 8; t++)
	    for (int i = 0; ((p = acpi_madt_find(_madt, t, i)) != NULL); i++)
		TRACE_INIT("MADT: found unknown type=%d, len=%d\n", p->type, p->len);
    }

    TRACE_INIT("  %d IRQ input lines, max IRQ ID is %d\n",
	       self->num_intsources, self->max_intsource);

    for (word_t i = 0; i <= self->max_intsource; i++)
    {
	if (self->redir[i].ioapic != NULL)
	{
	    TRACE_INIT("\tIRQ %2d: APIC %d, line %2d, %s, %s active\n",
		       i, self->redir[i].ioapic->id, self->redir[i].line,
		       self->redir[i].entry.x.trigger_mode ? "level" : "edge",
		       self->redir[i].entry.x.polarity ? "low" : "high");
	    /* route all IO-APIC IRQs to a spurious IRQ handler */
	    self->redir[i].entry.x.vector = IDT_IOAPIC_SPURIOUS;
	    intctrl_t_sync_redir_entry(&self->redir[i], INTCTRL_SYNC_ALL);
	}
    }
}


void intctrl_t_init_cpu(intctrl_t *self)
{
    (void) self;
    intctrl_t_init_local_apic();
}

void intctrl_t_mask(intctrl_t *self, word_t irq)
{
    if (irq >= intctrl_t_get_number_irqs(self))
	return;

    ASSERT(self->redir[irq].ioapic != NULL);
    ioapic_redir_mask_irq(&self->redir[irq].entry);

    if (ioapic_redir_is_level_triggered(&self->redir[irq].entry))
	intctrl_t_sync_redir_entry(&self->redir[irq], INTCTRL_SYNC_LOW);
}

bool intctrl_t_unmask(intctrl_t *self, word_t irq)
{
    if (irq >= intctrl_t_get_number_irqs(self))
	return false;

    ASSERT(self->redir[irq].ioapic != NULL);

    if (ioapic_redir_is_edge_triggered(&self->redir[irq].entry))
    {
	if (self->redir[irq].pending)
	{
	    self->redir[irq].pending = false;
	    return true; /* leave IRQ masked, since there was another pending */
	}
	ioapic_redir_unmask_irq(&self->redir[irq].entry);
    }
    else
    {
	ioapic_redir_unmask_irq(&self->redir[irq].entry);
	intctrl_t_sync_redir_entry(&self->redir[irq], INTCTRL_SYNC_LOW);
    }
    return false;
}

bool intctrl_t_is_masked(intctrl_t *self, word_t irq)
{
    if (irq >= intctrl_t_get_number_irqs(self))
	return false;
    return ioapic_redir_is_masked_irq(&self->redir[irq].entry);
}

void intctrl_t_enable(intctrl_t *self, word_t irq)
{
    if (irq >= intctrl_t_get_number_irqs(self))
	return;
    /* IRQ is unassigned? */
    if (self->redir[irq].entry.x.vector == IDT_IOAPIC_SPURIOUS) {
	u8_t vector = intctrl_t_setup_idt_entry(self, irq, 0);
	if (!vector) {
	    printf("IRQ %d: association failed, no free vector\n", irq);
	    return;
	}
	self->redir[irq].entry.x.vector = vector;
    }

    self->redir[irq].pending = false;
    ioapic_redir_unmask_irq(&self->redir[irq].entry);
    intctrl_t_sync_redir_entry(&self->redir[irq], INTCTRL_SYNC_LOW);
}

void intctrl_t_disable(intctrl_t *self, word_t irq)
{
    if (irq >= intctrl_t_get_number_irqs(self))
	return;
    word_t vector = self->redir[irq].entry.x.vector;
    self->redir[irq].entry.x.vector = IDT_IOAPIC_SPURIOUS;
    self->redir[irq].pending = false;
    ioapic_redir_mask_irq(&self->redir[irq].entry);
    intctrl_t_sync_redir_entry(&self->redir[irq], INTCTRL_SYNC_LOW);
    intctrl_t_free_idt_entry(self, irq, (u8_t) vector);
}

bool intctrl_t_is_pending(intctrl_t *self, word_t irq)
{
    return self->redir[irq].pending;
}

void intctrl_t_set_cpu(intctrl_t *self, word_t irq, word_t cpu)
{
    if (irq >= intctrl_t_get_number_irqs(self))
	return;

    if (ioapic_redir_get_phys_dest(&self->redir[irq].entry) != cpu_get_id(cpu_get((cpuid_t) cpu)))
    {
	ioapic_redir_set_phys_dest(&self->redir[irq].entry,
				   (u32_t) cpu_get_id(cpu_get((cpuid_t) cpu)));
	/* JS: for edge triggered IRQs, sw and hw redir entry may
	 * not be in sync. If we sync here, we may destroy the logic.
	 * We therefore only sync the upper half */
	intctrl_t_sync_redir_entry(&self->redir[irq], INTCTRL_SYNC_HIGH);
    }
}

/* handler invoked on interrupt (called from the hwirq_common asm stub with
   $intctrl in the first argument register) */
void intctrl_t_handle_irq(intctrl_t *self, word_t irq)
{
    bool deliver = true;

    /* edge triggered IRQs are marked as pending if masked */
    if ( ioapic_redir_is_edge_triggered(&self->redir[irq].entry) &&
	 self->redir[irq].entry.x.mask )
    {
	self->redir[irq].pending = true;
	deliver = false;
    }

    intctrl_t_mask(self, irq);
    local_apic_eoi();

    if (deliver)
	handle_interrupt(irq);
}

bool intctrl_t_is_irq_available(intctrl_t *self, word_t irq)
{
    ASSERT(irq < intctrl_t_get_number_irqs(self));
    if (irq == 9) return false;
    return self->redir[irq].ioapic != NULL;
}

static word_t intctrl_t_pmtimer_read(intctrl_t *self)
{
    u32_t first, second;

    /* the ACPI PM_TMR_BLK field is 32 bits wide, but it always holds
     * an x86 I/O port address, which is only 16 bits */
    first = second = in_u32((u16_t) self->pmtimer_ioport) & INTCTRL_PMTIMER_MASK;

    while (first == second)
    {
	second = in_u32((u16_t) self->pmtimer_ioport) & INTCTRL_PMTIMER_MASK;
	x86_pause();
    }
    return second;
}


/* C entry points wrapping the intctrl_t methods (declared in glue intctrl.h). */
bool intctrl_has_pmtimer(void)
{
    return get_interrupt_ctrl()->pmtimer_available;
}

void intctrl_pmtimer_wait(word_t ms)
{
    intctrl_t *self = get_interrupt_ctrl();
    /* Need to wait ms * pmtimer_ticks / 1000; */
    const word_t delta = (ms * INTCTRL_PMTIMER_TICKS) / 1000;
    word_t start = intctrl_t_pmtimer_read(self);

    if ((start + delta) >= INTCTRL_PMTIMER_MASK)
    {
	while (intctrl_t_pmtimer_read(self) >= start)
	    /* wait until overlap */;
    }

    while (intctrl_t_pmtimer_read(self) < ((start + delta) & INTCTRL_PMTIMER_MASK))
	/* wait */;
}

word_t intctrl_get_number_irqs(void)
{
    return intctrl_t_get_number_irqs(get_interrupt_ctrl());
}

bool intctrl_is_irq_available(word_t irq)
{
    return intctrl_t_is_irq_available(get_interrupt_ctrl(), irq);
}

void intctrl_mask(word_t irq)
{
    intctrl_t_mask(get_interrupt_ctrl(), irq);
}

bool intctrl_unmask(word_t irq)
{
    return intctrl_t_unmask(get_interrupt_ctrl(), irq);
}

void intctrl_enable(word_t irq)
{
    intctrl_t_enable(get_interrupt_ctrl(), irq);
}

void intctrl_disable(word_t irq)
{
    intctrl_t_disable(get_interrupt_ctrl(), irq);
}

bool intctrl_is_pending(word_t irq)
{
    return intctrl_t_is_pending(get_interrupt_ctrl(), irq);
}

void intctrl_set_cpu(word_t irq, word_t cpu)
{
    intctrl_t_set_cpu(get_interrupt_ctrl(), irq, cpu);
}

void intctrl_init_cpu(void)
{
    intctrl_t_init_cpu(get_interrupt_ctrl());
}

void intctrl_init_arch(void)
{
    intctrl_t_init_arch(get_interrupt_ctrl());
}

/* local_apic ops for the SMP AP-startup path (init.c). */
word_t apic_get_id(void)
{
    return local_apic_id();
}

void apic_send_init_ipi(word_t id, bool assert)
{
    local_apic_send_init_ipi((u8_t) id, assert);
}

void apic_send_startup_ipi(word_t id, void (*startup)(void))
{
    local_apic_send_startup_ipi((u8_t) id, startup);
}
