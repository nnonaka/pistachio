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
#if defined(__cplusplus)

    enum regtype_e
    {
	gdtr = 0x1,
	ldtr = 0x2,
	idtr = 0x3,
	tr   = 0x4
    };
    
    x86_descreg_t(word_t addr, u16_t size)
	{ 
	    descriptor.addr = addr; 
	    descriptor.size = size;  
	}

    x86_descreg_t(u16_t sel)
	{ selector = sel; }

    void setdescreg(const regtype_e type)
	{
	    switch(type)
	    {	
	    case gdtr:
		__asm__ __volatile__("lgdt %0\n" : /* No Output */ : "m"(descriptor)); 
		break;
	    case idtr:
		__asm__ __volatile__("lidt %0\n" : /* No Output */ : "m"(descriptor));
		break;
	    default:
		break;
	    }	
	}
    
    void getdescreg(const regtype_e type)
	{
	    
	    switch(type){	
	    case gdtr:
		__asm__ __volatile__("sgdt %0\n" : "=m"(descriptor));
		break;
	    case idtr:
		__asm__ __volatile__("sidt %0\n" : "=m"(descriptor));
		break;
	    default:
		break;
	    }	
    
	}

    void setselreg(const regtype_e type)
	{
	    switch(type)
	    {	
	    case ldtr:
		__asm__ __volatile__("lldt %0\n" : /* No Output */ : "m"(selector));
		break;
	    case tr:
		__asm__ __volatile__("ltr %0\n"  : /* No Output */ : "m"(selector));
		break;
	    default:
		break;
	    }	
	}
    void getselreg(const regtype_e type)
	{
	    
	    switch(type){	
	    case ldtr:
		__asm__ __volatile__("sldt %0\n" : "=m"(selector));
		break;
	    case tr:
		__asm__ __volatile__("str %0\n" : "=m"(selector));
		break;
	    default:
		selector = 0;
		break;
	    }	
	}
#endif /* __cplusplus */
};
typedef struct x86_descreg_t x86_descreg_t;

/* C free-function API for x86_descreg_t (the C++ methods above are used by
   code still compiled as C++; regtype values mirror regtype_e). */
#define X86_DESCREG_GDTR	0x1
#define X86_DESCREG_LDTR	0x2
#define X86_DESCREG_IDTR	0x3
#define X86_DESCREG_TR		0x4

#if !defined(__cplusplus)
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
#endif /* !__cplusplus */

#endif /* !__ARCH__X86__SEGDESC_H__ */
