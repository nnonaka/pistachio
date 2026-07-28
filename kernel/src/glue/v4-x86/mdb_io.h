/*********************************************************************
 *                
 * Copyright (C) 2005, 2007-2008,  Karlsruhe University
 *                
 * File path:     glue/v4-x86/mdb_io.h
 * Description:   MDB for IO ports specific declarations
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
 * $Id: mdb_io.h,v 1.3 2007/01/08 14:08:10 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __PLATFORM__PC99__MDB_IO_H__
#define __PLATFORM__PC99__MDB_IO_H__

#include <mdb.h>
#include INC_GLUE(mdb.h)
#include INC_GLUE(vrt_io.h)

#define MDB_IO_SIZES		VRT_IO_SIZES
#define MDB_IO_NUMSIZES		VRT_IO_NUMSIZES

/*
 * was class mdb_io_t : public mdb_t.  Like mdb_mem_t it adds no data, so the
 * instance is an mdb_t whose ops table is mdb_io_ops.
 */
BEGIN_DECLS
extern const mdb_ops_t mdb_io_ops;
extern mdb_t	     mdb_io;
extern mdb_node_t *  sigma0_ionode;
extern word_t	     mdb_io_sizes[];
extern word_t	     mdb_io_num_sizes;
END_DECLS

#endif /* !__PLATFORM__PC99__MDB_IO_H__ */
