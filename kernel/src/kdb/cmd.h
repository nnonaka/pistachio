/*********************************************************************
 *                
 * Copyright (C) 2002, 2003, 2007,  Karlsruhe University
 *                
 * File path:     kdb/cmd.h
 * Description:   Kernel debugger commands and command groups.
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
 * $Id: cmd.h,v 1.12 2003/09/24 19:04:54 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __CMD_H__
#define __CMD_H__

#include <kdb/linker_set.h>


/**
 * cmd_mode_t: Mode of operation for the kernel debugger.
 */
typedef enum {
    CMD_KEYMODE,
    CMD_LINEMODE
} cmd_mode_t;


/* Defined in kdb/generic/init.c. */
BEGIN_DECLS
extern cmd_mode_t kdb_cmd_mode;
END_DECLS



/*
 * Unprintable keystrokes.
 */

#define KEY_RETURN		'\r'
#define KEY_TAB			'\t'
#define KEY_ESC			0x1b
#define KEY_BS			0x8

#if defined(__cplusplus)
class cmd_t;
class cmd_group_t;
#else
struct cmd_t;
typedef struct cmd_t cmd_t;
struct cmd_group_t;
typedef struct cmd_group_t cmd_group_t;
#endif



/**
 * cmd_ret_t: Value returned from every command.  NOQUIT means that
 * more commands should be executed from the command group, ABORT
 * means that one should back up to the previous command group, and
 * QUIT means that one should back up to the root and exit the kernel
 * debugger.
 */
typedef enum {
    CMD_NOQUIT,
    CMD_ABORT,
    CMD_QUIT
} cmd_ret_t;


/**
 * cmd_func_t: Function type for all kernel debugger commands.  The
 * function takes the current command group as an argument and returns
 * a cmd_ret_t value.
 */
typedef cmd_ret_t (*cmd_func_t)(cmd_group_t *);



/**
 * cmd_t: Descriptor for kernel debugger command.
 */
struct cmd_t
{
    char	key;
    const char	*command;
    const char	*description;
    cmd_func_t	function;
};


/**
 * cmd_group_t: Descriptor for kernel debugger command group.
 */
struct cmd_group_t
{
    linker_set_t	*cmd_set;
    cmd_group_t		*parent;
    const char 		*name;

#if defined(__cplusplus)
    cmd_ret_t interact (cmd_group_t * myparent, const char * myname) __asm__ ("cmd_group_interact");
    void reset (void) { cmd_set->reset (); }
    cmd_t * next (void) { return (cmd_t *) cmd_set->next (); }
#endif
};

#if !defined(__cplusplus)
INLINE void cmd_group_reset (cmd_group_t *self) { linker_set_reset (self->cmd_set); }
INLINE cmd_t * cmd_group_next (cmd_group_t *self) { return (cmd_t *) linker_set_next (self->cmd_set); }
#endif


/**
 * DECLARE_CMD_GROUP: Declares a new command group.  Initializes group
 * with a help, abort and mode switch command.
 */
BEGIN_DECLS
cmd_ret_t cmd_group_interact (cmd_group_t *self, cmd_group_t *myparent, const char *myname);
END_DECLS

#define DECLARE_CMD_GROUP(group)					\
    DECLARE_SET(__kdb_group_##group);					\
    cmd_group_t group = { &__kdb_group_##group, NULL, NULL };		\
    DECLARE_CMD (cmd__help, group, '?', "help", "this help message");   \
    DECLARE_CMD (cmd__abort, group, KEY_BS, "up", "back up to previous menu"); \
    DECLARE_CMD (cmd__prior, group, KEY_ESC, "prior", "back to previous menu")

/**
 * DECLARE_CMD: Declares a new command in a command group and
 * initializes the command descriptor.
 */
#define DECLARE_CMD(func, group, key, cmd, desc)			\
    static cmd_t __kdb_##group##_##func = { key, cmd, desc, func };	\
    PUT_SET(__kdb_group_##group, __kdb_##group##_##func)
    

#define CMD(func, param) \
cmd_ret_t SECTION(".kdebug") func(cmd_group_t *param)

#endif /* !__CMD_H__ */
