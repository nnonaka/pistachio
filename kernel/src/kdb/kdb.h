/*********************************************************************
 *                
 * Copyright (C) 2002, 2005, 2007-2008,  Karlsruhe University
 *                
 * File path:     kdb/kdb.h
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
 * $Id: kdb.h,v 1.8 2005/04/11 14:24:56 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __KDB__KDB_H__
#define __KDB__KDB_H__

struct tcb_t;       typedef struct tcb_t tcb_t;
struct space_t;     typedef struct space_t space_t;
struct mdb_t;       typedef struct mdb_t mdb_t;
struct mdb_node_t;  typedef struct mdb_node_t mdb_node_t;
struct mdb_table_t; typedef struct mdb_table_t mdb_table_t;
struct vrt_t;       typedef struct vrt_t vrt_t;
struct vrt_table_t; typedef struct vrt_table_t vrt_table_t;
    
#include <kdb/cmd.h>

/* THE kernel debugger state.  Was a class whose methods have all become free
   functions; what remains is plain instance data. */
struct kdb_t {
    void *	kdb_param;
    tcb_t *	kdb_current;
    space_t *	last_space;
    word_t	last_dump;
};
typedef struct kdb_t kdb_t;
extern kdb_t kdb;

/* The debugger commands are free functions (they were static members, which is
   why cmd_func_t could always be a plain function pointer). */
#include <kdb_class_helper.h>

/* The kdb entry points: entry/init in kdb/generic/entry.c and init.c,
   pre/post in kdb/glue/v4-x86/prepost.c. */
BEGIN_DECLS
void kdb_entry (void * param);
void kdb_init (void);
bool kdb_pre (void);
void kdb_post (void);
END_DECLS


#endif /* !__KDB__KDB_H__ */
