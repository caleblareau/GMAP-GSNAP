/* $Id: mem.h 145604 2014-08-20 17:43:03Z twu $ */
#ifndef MEM_INCLUDED
#define MEM_INCLUDED

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stddef.h>
#include "except.h"

#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif


extern const Except_T Mem_Failed;

extern void
Mem_trap_start (void **location, const char *file, int line);
extern void
Mem_trap_check (const char *file, int line);

/* #define MEMUSAGE 1 */
#ifdef MEMUSAGE

typedef enum {MAIN_STORAGE, INPUT_USAGE, OUTPUT_USAGE, WORKER_STORAGE} Memusage_Class_T;

extern void
Mem_usage_init ();
extern void
Mem_usage_set_threadname (const char *threadname);
extern void
Mem_usage_reset (long int x);
extern void
Mem_usage_reset_max ();
extern void
Mem_usage_std_heap_add (long int x);
extern long int
Mem_usage_report_std_heap ();
extern long int
Mem_usage_report_std_max ();
extern long int
Mem_usage_report_keep ();
extern long int
Mem_usage_report_in ();
extern long int
Mem_usage_report_out ();
#endif


extern void *Mem_alloc (size_t nbytes, const char *file, int line);
extern void *Mem_alloc_keep (size_t nbytes, const char *file, int line);
extern void *Mem_alloc_in (size_t nbytes, const char *file, int line);
extern void *Mem_alloc_out (size_t nbytes, const char *file, int line);
extern void *Mem_alloc_no_exception (size_t nbytes,
				     const char *file, int line);
extern void *Mem_calloc (size_t count, size_t nbytes, const char *file, int line);
extern void *Mem_calloc_keep (size_t count, size_t nbytes, const char *file, int line);
extern void *Mem_calloc_in (size_t count, size_t nbytes, const char *file, int line);
extern void *Mem_calloc_out (size_t count, size_t nbytes, const char *file, int line);
extern void *Mem_calloc_no_exception (size_t count, size_t nbytes,
				      const char *file, int line);
extern void Mem_free (void *ptr, const char *file, int line);
extern void Mem_free_keep (void *ptr, const char *file, int line);
extern void Mem_free_in (void *ptr, const char *file, int line);
extern void Mem_free_out (void *ptr, const char *file, int line);
extern void *Mem_resize (void *ptr, size_t nbytes,
			 const char *file, int line);

#define MTRAP(location) Mem_trap_start((location), __FILE__, __LINE__)
#define MCHECK() Mem_trap_check(__FILE__, __LINE__)

#ifdef MEMUSAGE
#define MALLOC(nbytes) Mem_alloc((nbytes), __FILE__, __LINE__)
#define MALLOC_KEEP(nbytes) Mem_alloc_keep((nbytes), __FILE__, __LINE__)
#define MALLOC_IN(nbytes) Mem_alloc_in((nbytes), __FILE__, __LINE__)
#define MALLOC_OUT(nbytes) Mem_alloc_out((nbytes), __FILE__, __LINE__)

#define CALLOC(count, nbytes) Mem_calloc((count), (nbytes), __FILE__, __LINE__)
#define CALLOC_KEEP(count, nbytes) Mem_calloc_keep((count), (nbytes), __FILE__, __LINE__)
#define CALLOC_IN(count, nbytes) Mem_calloc_in((count), (nbytes), __FILE__, __LINE__)
#define CALLOC_OUT(count, nbytes) Mem_calloc_out((count), (nbytes), __FILE__, __LINE__)

#define FREE(ptr) ((void)(Mem_free((ptr),__FILE__, __LINE__), (ptr) = 0))
#define FREE_KEEP(ptr) ((void)(Mem_free_keep((ptr),__FILE__, __LINE__), (ptr) = 0))
#define FREE_IN(ptr) ((void)(Mem_free_in((ptr),__FILE__, __LINE__), (ptr) = 0))
#define FREE_OUT(ptr) ((void)(Mem_free_out((ptr),__FILE__, __LINE__), (ptr) = 0))

#else

#define MALLOC(nbytes) Mem_alloc((nbytes), __FILE__, __LINE__)
#define MALLOC_KEEP(nbytes) Mem_alloc((nbytes), __FILE__, __LINE__)
#define MALLOC_IN(nbytes) Mem_alloc((nbytes), __FILE__, __LINE__)
#define MALLOC_OUT(nbytes) Mem_alloc((nbytes), __FILE__, __LINE__)

#define CALLOC(count, nbytes) Mem_calloc((count), (nbytes), __FILE__, __LINE__)
#define CALLOC_KEEP(count, nbytes) Mem_calloc((count), (nbytes), __FILE__, __LINE__)
#define CALLOC_IN(count, nbytes) Mem_calloc((count), (nbytes), __FILE__, __LINE__)
#define CALLOC_OUT(count, nbytes) Mem_calloc((count), (nbytes), __FILE__, __LINE__)

#define FREE(ptr) ((void)(Mem_free((ptr),__FILE__, __LINE__), (ptr) = 0))
#define FREE_KEEP(ptr) ((void)(Mem_free((ptr),__FILE__, __LINE__), (ptr) = 0))
#define FREE_IN(ptr) ((void)(Mem_free((ptr),__FILE__, __LINE__), (ptr) = 0))
#define FREE_OUT(ptr) ((void)(Mem_free((ptr),__FILE__, __LINE__), (ptr) = 0))

#endif

#ifdef HAVE_ALLOCA
/* Cannot have a function containing alloca, since the memory will be freed when the function returns */
#define ALLOCA(nbytes) alloca((nbytes))
#define FREEA(ptr)
#endif


#define MALLOC_NO_EXCEPTION(nbytes) \
	Mem_alloc_no_exception((nbytes), __FILE__, __LINE__)
#define CALLOC_NO_EXCEPTION(count, nbytes) \
	Mem_calloc((count), (nbytes), __FILE__, __LINE__)
#define  NEW(p) ((p) = MALLOC(sizeof *(p)))
#define NEW0(p) ((p) = CALLOC(1, sizeof *(p)))
#define RESIZE(ptr, nbytes) 	((ptr) = Mem_resize((ptr), \
	(nbytes), __FILE__, __LINE__))
#endif
