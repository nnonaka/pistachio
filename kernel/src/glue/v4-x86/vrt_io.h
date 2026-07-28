/*********************************************************************
 *                
 * Copyright (C) 2005-2007,  Karlsruhe University
 *                
 * File path:     platform/pc99/vrt_io.h
 * Description:   VRT for IO ports specific declarations
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
 * $Id: vrt_io.h,v 1.4 2006/06/08 16:02:02 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __PLATFORM__PC99__VRT_IO_H__
#define __PLATFORM__PC99__VRT_IO_H__

#include <vrt.h>
#include <mdb.h>

#define VRT_IO_SIZES		{ 0, 1, 3, 8, 16 }
#define VRT_IO_NUMSIZES		4

struct space_t; typedef struct space_t space_t;

/*
 * was class vrt_io_t : public vrt_t.  The base is embedded as the first
 * member, which reproduces the C++ layout (vptr then vrt_t's data), so a
 * vrt_io_t * converts to its vrt_t * by address.
 */
struct vrt_io_t
{
    vrt_t	base;
    char	name[sizeof ("io<>  ") + sizeof (word_t) * 2];
    word_t	count;
    space_t *	space;
};
typedef struct vrt_io_t vrt_io_t;

/* was vrt_io_t::rights_e */
#define VRT_IO_RW		6
#define VRT_IO_FULLRIGHTS	6

BEGIN_DECLS
extern const vrt_ops_t vrt_io_ops;
extern word_t vrt_io_sizes[];
extern word_t vrt_io_num_sizes;

/* space management; operator new/delete become named functions */
vrt_io_t * vrt_io_alloc (void);
void	   vrt_io_free (vrt_io_t *v);
void	   vrt_io_init (vrt_io_t *self);
void	   vrt_io_populate_sigma0 (vrt_io_t *self);
END_DECLS


/**
 * Get access rights for object.  Always return full rights.
 *
 * @param object	object value
 *
 * @return access rights for object
 */
INLINE word_t vrt_io_get_rights (word_t object)
{
    (void) object;
    return VRT_IO_FULLRIGHTS;
}

/**
 * Set access rights for object.  Void operation.
 *
 * @param n		IO object node
 * @param rights	new access rights
 */
INLINE void vrt_io_set_rights (vrt_node_t *n, word_t rights)
{
    (void) n; (void) rights;
}

/**
 * Lookup the port number.  The port number is stored in the least
 * signigficant bits of the word.
 *
 * @param object	object value
 *
 * @return global thread number
 */
INLINE word_t vrt_io_get_port (word_t object)
{
    return object & 0xffff;
}

/**
 * Check whether we are really dealing with an IO space.  Checks
 * the name of the iospace, so only works if kernel debugger is
 * enabled.
 *
 * @return true if this looks like a thread space, false otherwise
 */
INLINE bool vrt_io_is_vrt_io_t (vrt_io_t *self)
{
    return self->name[0] == 'i' && self->name[1] == 'o' && self->name[2] == '<';
}

/**
 * Set the embedded space_t object. Needed for I/O bitmap manipulation
 *
 * @param s	space_t object reference
 */
INLINE void vrt_io_set_space (vrt_io_t *self, space_t *s)
{
    self->space = s;
}

/**
 * Get the embedded space_t object. Needed for I/O bitmap manipulation
 *
 * @return space_t object reference
 */
INLINE space_t * vrt_io_get_space (vrt_io_t *self)
{
    return self->space;
}


/*
 * We use the VRT for IO-ports as our IO space.
 */
typedef vrt_io_t	io_space_t;


#endif /* !__PLATFORM__PC99__VRT_IO_H__ */
