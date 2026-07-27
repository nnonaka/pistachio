/*********************************************************************
 *                
 * Copyright (C) 2004, 2010,  Karlsruhe University
 *                
 * File path:     kdb/generic/bootinfo.c
 * Description:   Generic bootinfo dumping
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
 * $Id: bootinfo.cc,v 1.5 2006/10/22 19:43:31 reichelt Exp $
 *                
 ********************************************************************/
#include <debug.h>
#include <kdb/cmd.h>
#include <kdb/kdb.h>

#include INC_API(kernelinterface.h)
#include INC_API(tcb.h)


#define L4_BOOTINFO_MAGIC		((word_t) 0x14b0021d)
#define L4_BOOTINFO_VERSION		1


/**
 * Generic bootinfo record.
 */
struct bootrec_t
{
    word_t	_type;
    word_t	_version;
    word_t	_offset_next;
};
typedef struct bootrec_t bootrec_t;

/* was bootrec_t::type_e */
enum bootrec_type_e {
    bootrec_module	= 0x0001,
    bootrec_simple_exec	= 0x0002,
    bootrec_efitables	= 0x0101,
    bootrec_multiboot	= 0x0102,
};

/* The C++ form returned type_e, which GCC sizes as a 4-byte unsigned int, so
   a garbage record's high 32 bits were dropped.  The (u32_t) cast keeps that
   exact behaviour -- it shows up in the default case, which prints the type. */
INLINE word_t bootrec_type (bootrec_t *self)
{ return (u32_t) self->_type; }

INLINE word_t bootrec_version (bootrec_t *self)
{ return self->_version; }

INLINE bootrec_t * bootrec_next (bootrec_t *self)
{ return (bootrec_t *) ((word_t) self + self->_offset_next); }


/**
 * Bootinfo record for simple binary file.
 */
struct boot_module_t
{
    word_t	type;			// 0x01
    word_t	version;		// 1
    word_t	offset_next;

    word_t	start;
    word_t	size;
    word_t	cmdline_offset;
};
typedef struct boot_module_t boot_module_t;

INLINE const char * boot_module_commandline (boot_module_t *self)
{ return self->cmdline_offset ? (const char *) self + self->cmdline_offset : ""; }


/**
 * Bootinfo record for simple executable image loaded and relocated by
 * the bootloader.
 */
struct boot_simpleexec_t
{
    word_t	type;			// 0x02
    word_t	version;		// 1
    word_t	offset_next;

    word_t	text_pstart;
    word_t	text_vstart;
    word_t	text_size;
    word_t	data_pstart;
    word_t	data_vstart;
    word_t	data_size;
    word_t	bss_pstart;
    word_t	bss_vstart;
    word_t	bss_size;
    word_t	initial_ip;
    word_t	flags;
    word_t	label;
    word_t	cmdline_offset;
};
typedef struct boot_simpleexec_t boot_simpleexec_t;

INLINE const char * boot_simpleexec_commandline (boot_simpleexec_t *self)
{ return self->cmdline_offset ? (const char *) self + self->cmdline_offset : ""; }


/**
 * Bootinfo record for EFI table information.
 */
struct boot_efi_t
{
    word_t	type;			// 0x101
    word_t	version;		// 1
    word_t	offset_next;

    word_t	systab;
    word_t	memmap;
    word_t	memmap_size;
    word_t	memdesc_size;
    word_t	memdesc_version;
};
typedef struct boot_efi_t boot_efi_t;


/**
 * Bootinfo record for multiboot info.
 */
struct boot_mbi_t
{
    word_t	type;			// 0x102
    word_t	version;		// 1
    word_t	offset_next;

    word_t	address;
};
typedef struct boot_mbi_t boot_mbi_t;


/**
 * Main structure for generic bootinfo.
 */
struct bootinfo_t
{
    word_t	_magic;
    word_t	_version;
    word_t	_size;
    word_t	_first_entry;
    word_t	_num_entries;
    word_t	__reserved[3];
};
typedef struct bootinfo_t bootinfo_t;

INLINE word_t bootinfo_safe_get (word_t * fld)
{ return space_readmem_phys ((paddr_t) fld); }

INLINE bool bootinfo_is_valid (bootinfo_t *self)
{ return bootinfo_safe_get (&self->_magic) == L4_BOOTINFO_MAGIC; }

INLINE word_t bootinfo_size_safe (bootinfo_t *self)
{ return bootinfo_safe_get (&self->_size); }

INLINE word_t bootinfo_magic (bootinfo_t *self)   { return self->_magic; }
INLINE word_t bootinfo_version (bootinfo_t *self) { return self->_version; }
INLINE word_t bootinfo_size (bootinfo_t *self)    { return self->_size; }
INLINE word_t bootinfo_entries (bootinfo_t *self) { return self->_num_entries; }

INLINE bootrec_t * bootinfo_first_entry (bootinfo_t *self)
{ return (bootrec_t *) ((word_t) self + self->_first_entry); }


/**
 * Copy of bootinfo structure.  Used in order to simplify parsing (no
 * need to access some random physical memory location).
 */
bootinfo_t * bootinfo_copy;



/**
 * Dump generic bootinfo structure
 */
DECLARE_CMD (cmd_dump_bootinfo, root, 'B', "bootinfo",
	     "generic bootinfo");

CMD (cmd_dump_bootinfo, cg)
{
    static word_t kip_bootinfo;
    bootinfo_t * bi = bootinfo_copy;

    if (bi == NULL)
    {
	kip_bootinfo = get_kip ()->boot_info;
	bi = (bootinfo_t *) kip_bootinfo;

	/*
	 * Do some sanity checking to see if this really is a valid
	 * generic BootInfo structure.
	 */

	if (bi == NULL || (word_t) bi > (word_t) GB (2))
	{
	    printf ("Doesn't look like a generic bootinfo structure "
		    "(bootinfo=%p)\n", bi);
	    return CMD_NOQUIT;
	}

	if (! bootinfo_is_valid (bi))
	{
	    printf ("Not a generic bootinfo record (bootinfo=%p).\n", bi);
	    return CMD_NOQUIT;
	}

	/*
	 * OK.  Looks fine.  Make a local copy of the bootinfo
	 * structure (easier to parse).
	 */

	word_t size = (bootinfo_size_safe (bi) + sizeof (word_t) - 1) &
	    ~(sizeof (word_t) - 1);
	word_t alloc_size = (1 << 12);
	while (alloc_size < size)
	    alloc_size <<= 1;

	EXTERN_KMEM_GROUP (kmem_misc);
	bootinfo_copy = (bootinfo_t *) 
	    kmem_alloc(&kmem, kmem_misc, (1UL << alloc_size));

	word_t * src = (word_t *) bi;
	word_t * dst = (word_t *) bootinfo_copy;
	for (;size > 0; size -= sizeof (word_t), src++, dst++)
	    *dst = space_readmem_phys ((paddr_t) src);

	bi = bootinfo_copy;
    }


    /*
     * We are here operating on a local copy of the bootinfo.
     */

    printf ("Generic BootInfo @ %p\n", kip_bootinfo);
    printf ("  magic:        0x%p\n"
	    "  version:      %d\n"
	    "  size:         0x%x\n"
	    "  num records:  %d\n\n",
	    bootinfo_magic (bi), bootinfo_version (bi), bootinfo_size (bi),
	    bootinfo_entries (bi));

    word_t numrec = bootinfo_entries (bi);
    bootrec_t * rec = bootinfo_first_entry (bi);

    for (word_t n = 1; numrec-- > 0; n++, rec = bootrec_next (rec))
    {
	switch (bootrec_type (rec))
	{
	case bootrec_module:
	{
	    boot_module_t * b = (boot_module_t *) rec;
	    printf ("[%d] Simple module (version %d):\n"
		    "  start:     %p\n"
		    "  size:      %p\n"
		    "  cmdline:   %s\n\n",
		    n, b->version, b->start, b->size,
		    boot_module_commandline (b));
	    break;
	}

	case bootrec_simple_exec:
	{
	    boot_simpleexec_t * e = (boot_simpleexec_t *) rec;
	    printf ("[%d] Simple executable (version %d):\n"
		    "  text:      [paddr: %p, vaddr: %p, size: %p]\n"
		    "  data:      [paddr: %p, vaddr: %p, size: %p]\n"
		    "  bss:       [paddr: %p, vaddr: %p, size: %p]\n"
		    "  entry:     %p\n"
		    "  flags:     %p\n"
		    "  label:     %p\n"
		    "  cmdline:   %s\n\n",
		    n, e->version,
		    e->text_pstart, e->text_vstart, e->text_size,
		    e->data_pstart, e->data_vstart, e->data_size,
		    e->bss_pstart,  e->bss_vstart,  e->bss_size,
		    e->initial_ip, e->flags, e->label,
		    boot_simpleexec_commandline (e));
	    break;
	}

	case bootrec_efitables:
	{
	    boot_efi_t * e = (boot_efi_t *) rec;
	    printf ("[%d] EFI Tables (version %d):\n"
		    "  systab:    %p\n"
		    "  memmap:    [addr: %p, size: 0x%x]\n"
		    "  memdesc:   [version: 0x%x, size: 0x%x]\n",
		    n, e->version, e->systab, e->memmap, e->memmap_size,
		    e->memdesc_version, e->memdesc_size);
	    break;
	}

	case bootrec_multiboot:
	{
	    boot_mbi_t * m = (boot_mbi_t *) rec;
	    printf ("[%d] Multiboot info (version %d):\n"
		    "  address:   %p\n",
		    n, m->version, m->address);
	    break;
	}

	default:
	    printf ("[%d] Unknown record (type: 0x%x,  version: %d)\n\n", 
		    n, bootrec_type (rec), bootrec_version (rec));
	}
    }

    return CMD_NOQUIT;
}
