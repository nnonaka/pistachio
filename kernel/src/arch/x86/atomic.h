/*********************************************************************
 *                
 * Copyright (C) 2007, 2009-2010,  Karlsruhe University
 *                
 * File path:     arch/x86/atomic.h
 * Description:   
 *                
 * @LICENSE@
 *                
 * $Id:$
 *                
 ********************************************************************/
#ifndef __ARCH__X86__ATOMIC_H__
#define __ARCH__X86__ATOMIC_H__

#ifdef CONFIG_SMP
# define X86_LOCK "lock;"
#else
# define X86_LOCK
#endif


struct atomic_t {
#if defined(__cplusplus)
    word_t operator ++ (int)
	{
	    // %z0 emits the operand size suffix -- without it the assembler
	    // defaults to "addl", which only updates half of a 64 bit word_t.
	    __asm__ __volatile__(X86_LOCK "add%z0 $1, %0" : "+m"(val));
	    return val;
	}

    word_t operator-- (int) 
	{
	    __asm__ __volatile__(X86_LOCK "sub%z0 $1, %0" : "+m"(val));
	    return val;
	}
    
    word_t operator = (word_t val) 
	{ return this->val = val; }

    word_t operator = (int val) 
	{ return this->val = val; }

    bool operator == (word_t val) 
	{ return (this->val == val); }
    
    bool operator == (int val) 
	{ return (this->val == (word_t) val); }

    bool operator != (word_t val) 
	{ return (this->val != val); }

    bool operator != (int val) 
	{ return (this->val != (word_t) val); }

    operator word_t (void) 
	{ return val; }

    
    bool cmpxchg( word_t old_val, word_t new_val )
	{
	    bool result;
	    __asm__ __volatile__ (
		X86_LOCK "cmpxchg %1, %2	\n\t"
		"setz %0		\n\t"

		: "=a" (result)
		: "r" (new_val), "m"(val), "0" (old_val)
		: "memory"
		);
	    return (result);
	}
#endif /* defined(__cplusplus) */

    word_t val;
};

typedef struct atomic_t atomic_t;

#if !defined(__cplusplus)
/* C forms of the atomic_t operators (val is C-visible). */
INLINE word_t atomic_inc (atomic_t *self)
{ __asm__ __volatile__(X86_LOCK "add%z0 $1, %0" : "+m"(self->val)); return self->val; }
INLINE word_t atomic_dec (atomic_t *self)
{ __asm__ __volatile__(X86_LOCK "sub%z0 $1, %0" : "+m"(self->val)); return self->val; }
INLINE word_t atomic_read (const atomic_t *self) { return self->val; }
#endif


#endif /* !__ARCH__X86__ATOMIC_H__ */
