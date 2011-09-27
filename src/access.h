/* $Id: access.h,v 1.5 2005/10/21 16:42:39 twu Exp $ */
#ifndef ACCESS_INCLUDED
#define ACCESS_INCLUDED

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>		/* For size_t, and for mmap on Linux, lseek, and getpagesize */
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>		/* For size_t, and for mmap and off_t */
#endif

#include "bool.h"

/* ALLOCATED implies bigendian conversion already done */
typedef enum {ALLOCATED, MMAPPED, FILEIO} Access_T;
#define MAX32BIT 4294967295U	/* 2^32 - 1 */

extern off_t
Access_filesize (char *filename);

extern int
Access_fileio (char *filename);

extern void *
Access_allocated (size_t *len, double *seconds, char *filename, size_t eltsize);

#ifdef HAVE_CADDR_T
extern caddr_t
#else
extern void *
#endif
Access_mmap (int *fd, size_t *len, char *filename, size_t eltsize, bool randomp);

#ifdef HAVE_CADDR_T
extern caddr_t
#else
extern void *
#endif
Access_mmap_rw (int *fd, size_t *len, char *filename, size_t eltsize, bool randomp);

#ifdef HAVE_CADDR_T
extern caddr_t
#else
extern void *
#endif
Access_mmap_and_preload (int *fd, size_t *len, int *npages, double *seconds,
			 char *filename, size_t eltsize);

#endif