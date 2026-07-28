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

    word_t val;
};

typedef struct atomic_t atomic_t;

/* C forms of the atomic_t operators (val is C-visible). */
INLINE word_t atomic_inc (atomic_t *self)
{ __asm__ __volatile__(X86_LOCK "add%z0 $1, %0" : "+m"(self->val)); return self->val; }
INLINE word_t atomic_dec (atomic_t *self)
{ __asm__ __volatile__(X86_LOCK "sub%z0 $1, %0" : "+m"(self->val)); return self->val; }
INLINE word_t atomic_read (const atomic_t *self) { return self->val; }
INLINE word_t atomic_set (atomic_t *self, word_t val) { return self->val = val; }
INLINE bool atomic_cmpxchg (atomic_t *self, word_t old_val, word_t new_val)
{
    bool result;
    __asm__ __volatile__ (
	X86_LOCK "cmpxchg %1, %2	\n\t"
	"setz %0		\n\t"
	: "=a" (result)
	: "r" (new_val), "m"(self->val), "0" (old_val)
	: "memory"
	);
    return result;
}


#endif /* !__ARCH__X86__ATOMIC_H__ */
