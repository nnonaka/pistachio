/*********************************************************************
 *                
 * Copyright (C) 2007, 2009,  Karlsruhe University
 *                
 * File path:     arch/x86/trapgate.h
 * Description:   
 *                
 * @LICENSE@
 *                
 * $Id:$
 *                
 ********************************************************************/
#ifndef __ARCH__X86__TRAPGATE_H__
#define __ARCH__X86__TRAPGATE_H__

#include INC_ARCH_SA(trapgate.h)

#if defined(CONFIG_DEBUG)
BEGIN_DECLS
int printf(const char* format, ...);
END_DECLS
#endif

#if defined(CONFIG_DEBUG)
/* register name / debug-register-list tables: plain globals (not class
   statics) so they can be defined in C (x64/exception.c). */
extern const char  * x86_exceptionframe_name[X86_EXCEPTIONREGS_NUM_REGS];
extern const word_t  x86_exceptionframe_dbgreg[18];
#endif

#if defined(__cplusplus)
class x86_exceptionframe_t : public x86_exceptionregs_t
{
public:

#if defined(CONFIG_DEBUG)
    void dump_flags()
	{
	    printf("%c%c%c%c%c%c%c%c%c%c%c",
		   regs[freg] & (1 <<  0) ? 'C' : 'c',
		   regs[freg] & (1 <<  2) ? 'P' : 'p',
		   regs[freg] & (1 <<  4) ? 'A' : 'a',
		   regs[freg] & (1 <<  6) ? 'Z' : 'z',
		   regs[freg] & (1 <<  7) ? 'S' : 's',
		   regs[freg] & (1 << 11) ? 'O' : 'o',
		   regs[freg] & (1 << 10) ? 'D' : 'd',
		   regs[freg] & (1 <<  9) ? 'I' : 'i',
		   regs[freg] & (1 <<  8) ? 'T' : 't',
		   regs[freg] & (1 << 16) ? 'R' : 'r',
		   ((regs[freg] >> 12) & 3) + '0'
		);
	}

    void dump ()
	{
            if (regs[csreg] == X86_KCS)
            {
                printf("fault addr: %8x\tstack: %8x\terror code: %x frame: %p\n",
                       regs[ipreg], (word_t) this + sizeof(*this) - 2 * sizeof(word_t), 
                       error,  this);
            }
            else
            {
                printf("fault addr: %8x\tstack: %8x\terror code: %x frame: %p\n",
                       regs[ipreg], regs[spreg], error,  this);
            }
	    
	    for (word_t r=0; r < num_dbgregs; r++)
	    {
		printf("\t%s: %wx", x86_exceptionframe_name[x86_exceptionframe_dbgreg[r]], regs[x86_exceptionframe_dbgreg[r]]);

		if (x86_exceptionframe_dbgreg[r] == freg)
		{ 
		    printf(" ["); dump_flags(); printf("]"); 
		} 
		if ((r+1) % 2 == 0) printf("\n");

	    }
	}
#endif /* defined(CONFIG_DEBUG */

};
#else /* !defined(__cplusplus) */
/*
 * C sees the exception frame as a plain register frame (same layout as the
 * C++ class, which adds only static members and methods on top of the base).
 */
struct x86_exceptionframe_t { struct x86_exceptionregs_t __base; };
#endif /* defined(__cplusplus) */

typedef struct x86_exceptionframe_t x86_exceptionframe_t;

#endif /* !__ARCH__X86__TRAPGATE_H__ */
