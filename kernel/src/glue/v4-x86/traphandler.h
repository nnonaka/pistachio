/*********************************************************************
 *                
 * Copyright (C) 2007,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/traphandler.h
 * Description:   
 *                
 * @LICENSE@
 *                
 * $Id:$
 *                
 ********************************************************************/
#ifndef __GLUE__V4_X86__TRAPHANDLER_H__
#define __GLUE__V4_X86__TRAPHANDLER_H__

BEGIN_DECLS
/* debugging exceptions */
void exc_debug(void);
void exc_nmi(void);
void exc_breakpoint(void);

/* gp, pagefault, kip */
void exc_gp(void);
void exc_pagefault(void);
void exc_invalid_opcode(void);

/* fpu  */
void exc_nomath_coproc(void);

/* catcher for all other interrupts and exceptions */
typedef void (*func_exc)(void);
extern u64_t exc_catch_all[IDT_SIZE] UNIT("x86.exc_all");
void exc_catch_common_wrapper(void);
void exc_catch_common(void);
END_DECLS

/* exception-frame register <-> IPC message-register map. A plain global (not
   a class static) so it can be defined in C (x64/exception.c). */
extern const word_t x86_exc_reg_mr2reg[NUM_EXC_REGS][2];

#if defined(__cplusplus)
/* exception handling */
class x86_exc_reg_t
{
public:
    static const word_t mr(word_t num) { return x86_exc_reg_mr2reg[num][0]; };
    static const word_t reg(word_t num) { return x86_exc_reg_mr2reg[num][1]; };
};
#else /* !__cplusplus: C forms of the x86_exc_reg_t static accessors. */
INLINE word_t x86_exc_reg_mr (word_t num)  { return x86_exc_reg_mr2reg[num][0]; }
INLINE word_t x86_exc_reg_reg (word_t num) { return x86_exc_reg_mr2reg[num][1]; }
#endif /* __cplusplus */

/* send_exception_ipc is defined in glue/v4-x86/exception.c (C); keep C linkage
   so it is callable from both languages. */
BEGIN_DECLS
bool send_exception_ipc(x86_exceptionframe_t * frame, word_t exception);
END_DECLS



#endif /* !__GLUE__V4_X86__TRAPHANDLER_H__ */
