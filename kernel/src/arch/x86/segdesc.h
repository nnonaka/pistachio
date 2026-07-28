/*********************************************************************
 *                
 * Copyright (C) 2007-2008, 2010,  Karlsruhe University
 *                
 * File path:     arch/x86/segdesc.h
 * Description:   
 *                
 * @LICENSE@
 *                
 * $Id:$
 *                
 ********************************************************************/
/* 
 * GDTR, IDTR, LDTR, TR  
 * 
 */
#ifndef __ARCH__X86__SEGDESC_H__
#define __ARCH__X86__SEGDESC_H__

#include INC_ARCH_SA(segdesc.h)

struct x86_descreg_t
{
    union 
    {
	struct {
	    u16_t	   size;
	    word_t	   addr __attribute__((packed));
	} descriptor;
	u16_t  selector;
    };
};
typedef struct x86_descreg_t x86_descreg_t;

/* C free-function API for x86_descreg_t (the C++ methods above are used by
   code still compiled as C++; regtype values mirror regtype_e). */
#define X86_DESCREG_GDTR	0x1
#define X86_DESCREG_LDTR	0x2
#define X86_DESCREG_IDTR	0x3
#define X86_DESCREG_TR		0x4

static inline void x86_descreg_set(x86_descreg_t *self, word_t addr, u16_t size)
{
    self->descriptor.addr = addr;
    self->descriptor.size = size;
}

static inline void x86_descreg_setdescreg(x86_descreg_t *self, int type)
{
    switch (type)
    {
    case X86_DESCREG_GDTR:
	__asm__ __volatile__("lgdt %0\n" : /* No Output */ : "m"(self->descriptor));
	break;
    case X86_DESCREG_IDTR:
	__asm__ __volatile__("lidt %0\n" : /* No Output */ : "m"(self->descriptor));
	break;
    default:
	break;
    }
}

/* the x86_descreg_t(u16_t sel) constructor. */
static inline void x86_descreg_set_sel(x86_descreg_t *self, u16_t sel)
{
    self->selector = sel;
}

static inline void x86_descreg_setselreg(x86_descreg_t *self, int type)
{
    switch (type)
    {
    case X86_DESCREG_LDTR:
	__asm__ __volatile__("lldt %0\n" : /* No Output */ : "m"(self->selector));
	break;
    case X86_DESCREG_TR:
	__asm__ __volatile__("ltr %0\n"  : /* No Output */ : "m"(self->selector));
	break;
    default:
	break;
    }
}

#endif /* !__ARCH__X86__SEGDESC_H__ */
