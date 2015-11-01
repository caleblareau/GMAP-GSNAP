static char rcsid[] = "$Id: mem.c 145604 2014-08-20 17:43:03Z twu $";
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "mem.h"
#include <stdio.h>
#include <stdlib.h>
#include "assert.h"
#include "except.h"
#include "bool.h"

#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif

/* Prints out memory usage */
/* #define DEBUG 1 */
#ifdef DEBUG
#define debug(x) x
#else
#define debug(x)
#endif


/* #define TRAP 1 */
#ifdef TRAP
static void *trap_contents;
static void **trap_location;
static int startp = 0;

void
Mem_trap_start (void **location, const char *file, int line) {
  if (startp == 0) {
    trap_location = location;
    trap_contents = * (void **) location;
    startp = 1;
    printf("Initial value at location %p is %p from %s:%d\n",
	   trap_location,trap_contents,file,line);
    fflush(stdout);
  }
  return;
}

void
Mem_trap_check (const char *file, int line) {
  if (startp > 0 && *trap_location != trap_contents) {
      printf("Value changed at location %p.  Old value was %p.  New value is %p.  Observed during check at %s:%d\n",
	     trap_location,trap_contents,*trap_location,file,line);
      fflush(stdout);
      trap_contents = * (void **) trap_location;
  }
  return;
}
#endif


#ifdef MEMUSAGE

#ifdef HAVE_PTHREAD
static pthread_mutex_t memusage_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t key_memusage_std_heap; /* Standard pool: Memory that is used by a thread within a query */
static pthread_key_t key_memusage_std_max;
static pthread_key_t key_memusage_keep; /* Keep pool: Memory that is kept by a thread between queries  */
static pthread_key_t key_threadname;
#else
static char *threadname = "program";
#endif

static long int memusage_std_heap = 0;
static long int memusage_std_max = 0;
static long int memusage_in = 0; /* Input pool: Memory from inbuffer to threads */
static long int memusage_out = 0; /* Output pool: Memory from threads to outbuffer */

void
Mem_usage_init () {
#ifdef HAVE_PTHREAD
  pthread_key_create(&key_memusage_std_heap,NULL);
  pthread_key_create(&key_memusage_std_max,NULL);
  pthread_key_create(&key_memusage_keep,NULL);
  pthread_key_create(&key_threadname,NULL);
  pthread_setspecific(key_memusage_std_heap,(void *) 0);
  pthread_setspecific(key_memusage_std_max,(void *) 0);
#else
  memusage_std_heap = 0;
  memusage_std_max = 0;
#endif

  memusage_in = 0;
  memusage_out = 0;
  return;
}


void
Mem_usage_set_threadname (const char *threadname) {
#ifdef HAVE_PTHREAD
  pthread_setspecific(key_threadname,(void *) threadname);
#endif
  return;
}

void
Mem_usage_reset (long int x) {
#ifdef HAVE_PTHREAD
  char *threadname;
  long int memusage_std_heap;

  threadname = (char *) pthread_getspecific(key_threadname);
  memusage_std_heap = (long int) pthread_getspecific(key_memusage_std_heap);
  debug(printf("%ld %s: Reset memusage_std_heap to %ld\n",memusage_std_heap,threadname,x));
  pthread_setspecific(key_memusage_std_heap,(void *) x);
#else
  debug(printf("%ld: Reset memusage_std_heap to %ld\n",memusage_std_heap,x));
  memusage_std_heap = x;
#endif
}

void
Mem_usage_reset_max () {
#ifdef HAVE_PTHREAD
  char *threadname;
  long int memusage_std_max;

  threadname = (char *) pthread_getspecific(key_threadname);
  memusage_std_max = (long int) pthread_getspecific(key_memusage_std_max);
  pthread_setspecific(key_memusage_std_max,(void *) 0);
#else
  memusage_std_max = 0;
#endif
}

void
Mem_usage_std_heap_add (long int x) {
#ifdef HAVE_PTHREAD
  char *threadname;
  long int memusage_std_heap;

  threadname = (char *) pthread_getspecific(key_threadname);
  memusage_std_heap = (long int) pthread_getspecific(key_memusage_std);
  debug(printf("%ld %s: ",memusage_std_heap,threadname));
  memusage_std_heap += x;
  pthread_setspecific(key_memusage_std_heap,(void *) x);
  debug(printf("Reset memusage_std_heap to %ld\n",memusage_std_heap));
#else
  debug(printf("%ld: ",memusage_std_heap));
  memusage_std_heap += x;
  debug(printf("Reset memusage_std_heap to %ld\n",memusage_std_heap));
#endif
}

long int
Mem_usage_report_std_heap () {
#ifdef HAVE_PTHREAD
  return (long int) pthread_getspecific(key_memusage_std_heap);
#else
  return memusage_std_heap;
#endif
}

long int
Mem_usage_report_keep () {
#ifdef HAVE_PTHREAD
  return (long int) pthread_getspecific(key_memusage_keep);
#else
  return memusage_keep;
#endif
}

long int
Mem_usage_report_std_max () {
#ifdef HAVE_PTHREAD
  return (long int) pthread_getspecific(key_memusage_std_max);
#else
  return memusage_std_max;
#endif
}

long int
Mem_usage_report_in () {
  return memusage_in;
}

long int
Mem_usage_report_out () {
  return memusage_out;
}


#define hash(p, t) (((unsigned long)(p)>>3) & (sizeof (t)/sizeof ((t)[0])-1))
struct descriptor {
  struct descriptor *link;
  const void *ptr;
  long size;
};
static struct descriptor *htab[2048000];

/* Also removes element from linked list */
static struct descriptor *
find (const void *ptr) {
  struct descriptor *bp, **pp;

  pp = &(htab[hash(ptr, htab)]);
  while (*pp && (*pp)->ptr != ptr) {
    pp = &(*pp)->link;
  }
  if (*pp) {
    bp = *pp;
    *pp = bp->link;
    return bp;
  } else {
    return NULL;
  }
}
#endif



const Except_T Mem_Leak = { "Memory Leak" };
const Except_T Mem_Failed = { "Allocation Failed" };


void *
Mem_alloc (size_t nbytes, const char *file, int line) {
  void *ptr;
#ifdef MEMUSAGE
  static struct descriptor *bp;
  unsigned h;

#ifdef HAVE_PTHREAD
  pthread_mutex_lock(&memusage_mutex);
  long int memusage_std_heap, memusage_std_max;
  char *threadname;
#endif
#endif

  assert(nbytes > 0);
  ptr = malloc(nbytes);

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  threadname = (char *) pthread_getspecific(key_threadname);
  memusage_std_heap = (long int) pthread_getspecific(key_memusage_std_heap);
  memusage_std_heap += nbytes;
  pthread_setspecific(key_memusage_std_heap,(void *) memusage_std_heap);

  memusage_std_max = (long int) pthread_getspecific(key_memusage_std_max);
  if (memusage_std_heap > memusage_std_max) {
    pthread_setspecific(key_memusage_std_max,(void *) memusage_std_heap);
  }
#else
  memusage_std_heap += nbytes;
  if (memusage_std_heap > memusage_std_max) {
    memusage_std_max = memusage_std_heap;
  }
#endif
  h = hash(ptr,htab);
  bp = malloc(sizeof(*bp));
  bp->link = htab[h];
  bp->ptr = ptr;
  bp->size = nbytes;
  htab[h] = bp;
#endif

#ifdef MEMUSAGE
  debug(printf("%ld %s: Allocating %p to %p -- Malloc of %lu bytes in standard pool requested from %s:%d\n",
	       memusage_std_heap,threadname,ptr,(char *) ptr + nbytes-1,nbytes,file,line));
#else
  debug(printf("Allocating %p to %p -- Malloc of %lu bytes in standard pool requested from %s:%d\n",
	       ptr,(char *) ptr + nbytes-1,nbytes,file,line));
#endif


#ifdef TRAP
  if (ptr == trap_location) {
    printf("Trap: Alloc of location %p by %s:%d\n",ptr,file,line);
  }
  if (startp > 0 && *trap_location != trap_contents) {
      printf("Value changed at location %p.  Old value was %p.  New value is %p.  Observed during malloc at %s:%d\n",
	     trap_location,trap_contents,*trap_location,file,line);
      fflush(stdout);
      trap_contents = * (void **) trap_location;
  }
#endif

  if (ptr == NULL) {
    fprintf(stderr,"Failed attempt to alloc %lu bytes\n",nbytes);
    if (file == NULL) {
      RAISE(Mem_Failed);
    } else {
      Except_raise(&Mem_Failed, file, line);
    }
  }

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  pthread_mutex_unlock(&memusage_mutex);
#endif
#endif

  return ptr;
}



void *
Mem_alloc_keep (size_t nbytes, const char *file, int line) {
  void *ptr;
#ifdef MEMUSAGE
  static struct descriptor *bp;
  unsigned h;

#ifdef HAVE_PTHREAD
  pthread_mutex_lock(&memusage_mutex);
  long int memusage_keep;
  char *threadname;
#endif
#endif

  assert(nbytes > 0);
  ptr = malloc(nbytes);

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  threadname = (char *) pthread_getspecific(key_threadname);
  memusage_keep = (long int) pthread_getspecific(key_memusage_keep);
  memusage_keep += nbytes;
  pthread_setspecific(key_memusage_keep,(void *) memusage_keep);
#else
  memusage_keep += nbytes;
#endif
  h = hash(ptr,htab);
  bp = malloc(sizeof(*bp));
  bp->link = htab[h];
  bp->ptr = ptr;
  bp->size = nbytes;
  htab[h] = bp;
#endif

#ifdef MEMUSAGE
  debug(printf("%ld %s-keep: Allocating %p to %p -- Malloc of %lu bytes in keep pool requested from %s:%d\n",
	       memusage_keep,threadname,ptr,(char *) ptr + nbytes-1,nbytes,file,line));
#else
  debug(printf("Allocating %p to %p -- Malloc of %lu bytes in keep pool requested from %s:%d\n",
	       ptr,(char *) ptr + nbytes-1,nbytes,file,line));
#endif


#ifdef TRAP
  if (ptr == trap_location) {
    printf("Trap: Alloc of location %p by %s:%d\n",ptr,file,line);
  }
  if (startp > 0 && *trap_location != trap_contents) {
      printf("Value changed at location %p.  Old value was %p.  New value is %p.  Observed during malloc at %s:%d\n",
	     trap_location,trap_contents,*trap_location,file,line);
      fflush(stdout);
      trap_contents = * (void **) trap_location;
  }
#endif

  if (ptr == NULL) {
    fprintf(stderr,"Failed attempt to alloc %lu bytes\n",nbytes);
    if (file == NULL) {
      RAISE(Mem_Failed);
    } else {
      Except_raise(&Mem_Failed, file, line);
    }
  }

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  pthread_mutex_unlock(&memusage_mutex);
#endif
#endif

  return ptr;
}

void *
Mem_alloc_in (size_t nbytes, const char *file, int line) {
  void *ptr;
#ifdef MEMUSAGE
  static struct descriptor *bp;
  unsigned h;

#ifdef HAVE_PTHREAD
  pthread_mutex_lock(&memusage_mutex);
#endif
#endif

  assert(nbytes > 0);
  ptr = malloc(nbytes);

#ifdef MEMUSAGE
  memusage_in += nbytes;
  h = hash(ptr,htab);
  bp = malloc(sizeof(*bp));
  bp->link = htab[h];
  bp->ptr = ptr;
  bp->size = nbytes;
  htab[h] = bp;
#endif

#ifdef MEMUSAGE
  debug(printf("%ld IN: Allocating %p to %p -- Malloc of %lu bytes in input pool requested from %s:%d\n",
	       memusage_in,ptr,(char *) ptr + nbytes-1,nbytes,file,line));
#else
  debug(printf("Allocating %p to %p -- Malloc of %lu bytes in input pool requested from %s:%d\n",
	       ptr,(char *) ptr + nbytes-1,nbytes,file,line));
#endif


#ifdef TRAP
  if (ptr == trap_location) {
    printf("Trap: Alloc of location %p by %s:%d\n",ptr,file,line);
  }
  if (startp > 0 && *trap_location != trap_contents) {
      printf("Value changed at location %p.  Old value was %p.  New value is %p.  Observed during malloc at %s:%d\n",
	     trap_location,trap_contents,*trap_location,file,line);
      fflush(stdout);
      trap_contents = * (void **) trap_location;
  }
#endif

  if (ptr == NULL) {
    fprintf(stderr,"Failed attempt to alloc %lu bytes\n",nbytes);
    if (file == NULL) {
      RAISE(Mem_Failed);
    } else {
      Except_raise(&Mem_Failed, file, line);
    }
  }

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  pthread_mutex_unlock(&memusage_mutex);
#endif
#endif

  return ptr;
}

void *
Mem_alloc_out (size_t nbytes, const char *file, int line) {
  void *ptr;
#ifdef MEMUSAGE
  static struct descriptor *bp;
  unsigned h;

#ifdef HAVE_PTHREAD
  pthread_mutex_lock(&memusage_mutex);
#endif
#endif

  assert(nbytes > 0);
  ptr = malloc(nbytes);

#ifdef MEMUSAGE
  memusage_out += nbytes;
  h = hash(ptr,htab);
  bp = malloc(sizeof(*bp));
  bp->link = htab[h];
  bp->ptr = ptr;
  bp->size = nbytes;
  htab[h] = bp;
#endif

#ifdef MEMUSAGE
  debug(printf("%ld OUT: Allocating %p to %p -- Malloc of %lu bytes in output pool requested from %s:%d\n",
	       memusage_out,ptr,(char *) ptr + nbytes-1,nbytes,file,line));
#else
  debug(printf("Allocating %p to %p -- Malloc of %lu bytes in output pool requested from %s:%d\n",
	       ptr,(char *) ptr + nbytes-1,nbytes,file,line));
#endif


#ifdef TRAP
  if (ptr == trap_location) {
    printf("Trap: Alloc of location %p by %s:%d\n",ptr,file,line);
  }
  if (startp > 0 && *trap_location != trap_contents) {
      printf("Value changed at location %p.  Old value was %p.  New value is %p.  Observed during malloc at %s:%d\n",
	     trap_location,trap_contents,*trap_location,file,line);
      fflush(stdout);
      trap_contents = * (void **) trap_location;
  }
#endif

  if (ptr == NULL) {
    fprintf(stderr,"Failed attempt to alloc %lu bytes\n",nbytes);
    if (file == NULL) {
      RAISE(Mem_Failed);
    } else {
      Except_raise(&Mem_Failed, file, line);
    }
  }

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  pthread_mutex_unlock(&memusage_mutex);
#endif
#endif

  return ptr;
}

void *
Mem_alloc_no_exception (size_t nbytes, const char *file, int line) {
  void *ptr;
  assert(nbytes > 0);
  ptr = malloc(nbytes);
  return ptr;
}

void *
Mem_calloc (size_t count, size_t nbytes, const char *file, int line) {
  void *ptr;
#ifdef MEMUSAGE
  static struct descriptor *bp;
  unsigned h;

#ifdef HAVE_PTHREAD
  pthread_mutex_lock(&memusage_mutex);
  long int memusage_std_heap, memusage_std_max;
  char *threadname;
#endif
#endif

  if (count <= 0) {
    fprintf(stderr,"Failed attempt to calloc %lu x %lu bytes\n",count,nbytes);
    if (file == NULL) {
      RAISE(Mem_Failed);
    } else {
      Except_raise(&Mem_Failed, file, line);
    }
  }
  assert(nbytes > 0);

  ptr = calloc(count,nbytes);

#ifdef TRAP
  if (ptr == trap_location) {
    printf("Trap: Calloc of location %p by %s:%d\n",ptr,file,line);
  }

  if (startp > 0 && *trap_location != trap_contents) {
      printf("Value changed at location %p.  Old value is %p.  New value is %p.  Observed during calloc at %s:%d\n",
	     trap_location,trap_contents,*trap_location,file,line);
      fflush(stdout);
      trap_contents = * (void **) trap_location;
  }
#endif

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  threadname = (char *) pthread_getspecific(key_threadname);
  memusage_std_heap = (long int) pthread_getspecific(key_memusage_std_heap);
  memusage_std_heap += count*nbytes;
  pthread_setspecific(key_memusage_std_heap,(void *) memusage_std_heap);

  memusage_std_max = (long int) pthread_getspecific(key_memusage_std_max);
  if (memusage_std_heap > memusage_std_max) {
    pthread_setspecific(key_memusage_std_max,(void *) memusage_std_heap);
  }
#else
  memusage_std_heap += count*nbytes;
  if (memusage_std_heap > memusage_std_max) {
    memusage_std_max = memusage_std_heap;
  }
#endif
  h = hash(ptr,htab);
  bp = malloc(sizeof(*bp));
  bp->link = htab[h];
  bp->ptr = ptr;
  bp->size = count*nbytes;
  htab[h] = bp;
#endif

#ifdef MEMUSAGE
  debug(printf("%ld %s: Allocating %p to %p -- Calloc of %lu x %lu = %lu bytes in standard pool requested from %s:%d\n",
	       memusage_std_heap,threadname,ptr,(char *) ptr + count*nbytes-1,count,nbytes,count*nbytes,file,line));
#else
  debug(printf("Allocating %p to %p -- Calloc of %lu x %lu = %lu bytes in standard pool requested from %s:%d\n",
	       ptr,(char *) ptr + count*nbytes-1,count,nbytes,count*nbytes,file,line));
#endif

  if (ptr == NULL) {
    fprintf(stderr,"Failed attempt to calloc %lu x %lu bytes\n",count,nbytes);
    if (file == NULL) {
      RAISE(Mem_Failed);
    } else {
      Except_raise(&Mem_Failed, file, line);
    }
  }

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  pthread_mutex_unlock(&memusage_mutex);
#endif
#endif

  return ptr;
}


void *
Mem_calloc_keep (size_t count, size_t nbytes, const char *file, int line) {
  void *ptr;
#ifdef MEMUSAGE
  static struct descriptor *bp;
  unsigned h;

#ifdef HAVE_PTHREAD
  pthread_mutex_lock(&memusage_mutex);
  long int memusage_keep;
  char *threadname;
#endif
#endif

  if (count <= 0) {
    fprintf(stderr,"Failed attempt to calloc %lu x %lu bytes\n",count,nbytes);
    if (file == NULL) {
      RAISE(Mem_Failed);
    } else {
      Except_raise(&Mem_Failed, file, line);
    }
  }
  assert(nbytes > 0);

  ptr = calloc(count,nbytes);

#ifdef TRAP
  if (ptr == trap_location) {
    printf("Trap: Calloc of location %p by %s:%d\n",ptr,file,line);
  }

  if (startp > 0 && *trap_location != trap_contents) {
      printf("Value changed at location %p.  Old value is %p.  New value is %p.  Observed during calloc at %s:%d\n",
	     trap_location,trap_contents,*trap_location,file,line);
      fflush(stdout);
      trap_contents = * (void **) trap_location;
  }
#endif

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  threadname = (char *) pthread_getspecific(key_threadname);
  memusage_keep = (long int) pthread_getspecific(key_memusage_keep);
  memusage_keep += count*nbytes;
  pthread_setspecific(key_memusage_keep,(void *) memusage_keep);
#else
  memusage_keep += count*nbytes;
#endif
  h = hash(ptr,htab);
  bp = malloc(sizeof(*bp));
  bp->link = htab[h];
  bp->ptr = ptr;
  bp->size = count*nbytes;
  htab[h] = bp;
#endif

#ifdef MEMUSAGE
  debug(printf("%ld %s-keep: Allocating %p to %p -- Calloc of %lu x %lu = %lu bytes in keep pool requested from %s:%d\n",
	       memusage_keep,threadname,ptr,(char *) ptr + count*nbytes-1,count,nbytes,count*nbytes,file,line));
#else
  debug(printf("Allocating %p to %p -- Calloc of %lu x %lu = %lu bytes in keep pool requested from %s:%d\n",
	       ptr,(char *) ptr + count*nbytes-1,count,nbytes,count*nbytes,file,line));
#endif

  if (ptr == NULL) {
    fprintf(stderr,"Failed attempt to calloc %lu x %lu bytes\n",count,nbytes);
    if (file == NULL) {
      RAISE(Mem_Failed);
    } else {
      Except_raise(&Mem_Failed, file, line);
    }
  }

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  pthread_mutex_unlock(&memusage_mutex);
#endif
#endif

  return ptr;
}


void *
Mem_calloc_in (size_t count, size_t nbytes, const char *file, int line) {
  void *ptr;
#ifdef MEMUSAGE
  static struct descriptor *bp;
  unsigned h;

#ifdef HAVE_PTHREAD
  pthread_mutex_lock(&memusage_mutex);
#endif
#endif

  if (count <= 0) {
    fprintf(stderr,"Failed attempt to calloc %lu x %lu bytes\n",count,nbytes);
    if (file == NULL) {
      RAISE(Mem_Failed);
    } else {
      Except_raise(&Mem_Failed, file, line);
    }
  }
  assert(nbytes > 0);

  ptr = calloc(count,nbytes);

#ifdef TRAP
  if (ptr == trap_location) {
    printf("Trap: Calloc of location %p by %s:%d\n",ptr,file,line);
  }

  if (startp > 0 && *trap_location != trap_contents) {
      printf("Value changed at location %p.  Old value is %p.  New value is %p.  Observed during calloc at %s:%d\n",
	     trap_location,trap_contents,*trap_location,file,line);
      fflush(stdout);
      trap_contents = * (void **) trap_location;
  }
#endif

#ifdef MEMUSAGE
  memusage_in += count*nbytes;
  h = hash(ptr,htab);
  bp = malloc(sizeof(*bp));
  bp->link = htab[h];
  bp->ptr = ptr;
  bp->size = count*nbytes;
  htab[h] = bp;
#endif

#ifdef MEMUSAGE
  debug(printf("%ld IN: Allocating %p to %p -- Calloc of %lu x %lu = %lu bytes in input pool requested from %s:%d\n",
	       memusage_in,ptr,(char *) ptr + count*nbytes-1,count,nbytes,count*nbytes,file,line));
#else
  debug(printf("Allocating %p to %p -- Calloc of %lu x %lu = %lu bytes in input pool requested from %s:%d\n",
	       ptr,(char *) ptr + count*nbytes-1,count,nbytes,count*nbytes,file,line));
#endif

  if (ptr == NULL) {
    fprintf(stderr,"Failed attempt to calloc %lu x %lu bytes\n",count,nbytes);
    if (file == NULL) {
      RAISE(Mem_Failed);
    } else {
      Except_raise(&Mem_Failed, file, line);
    }
  }

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  pthread_mutex_unlock(&memusage_mutex);
#endif
#endif

  return ptr;
}

void *
Mem_calloc_out (size_t count, size_t nbytes, const char *file, int line) {
  void *ptr;
#ifdef MEMUSAGE
  static struct descriptor *bp;
  unsigned h;

#ifdef HAVE_PTHREAD
  pthread_mutex_lock(&memusage_mutex);
#endif
#endif

  if (count <= 0) {
    fprintf(stderr,"Failed attempt to calloc %lu x %lu bytes\n",count,nbytes);
    if (file == NULL) {
      RAISE(Mem_Failed);
    } else {
      Except_raise(&Mem_Failed, file, line);
    }
  }
  assert(nbytes > 0);

  ptr = calloc(count,nbytes);

#ifdef TRAP
  if (ptr == trap_location) {
    printf("Trap: Calloc of location %p by %s:%d\n",ptr,file,line);
  }

  if (startp > 0 && *trap_location != trap_contents) {
      printf("Value changed at location %p.  Old value is %p.  New value is %p.  Observed during calloc at %s:%d\n",
	     trap_location,trap_contents,*trap_location,file,line);
      fflush(stdout);
      trap_contents = * (void **) trap_location;
  }
#endif

#ifdef MEMUSAGE
  memusage_out += count*nbytes;
  h = hash(ptr,htab);
  bp = malloc(sizeof(*bp));
  bp->link = htab[h];
  bp->ptr = ptr;
  bp->size = count*nbytes;
  htab[h] = bp;
#endif

#ifdef MEMUSAGE
  debug(printf("%ld OUT: Allocating %p to %p -- Calloc of %lu x %lu = %lu bytes in output pool requested from %s:%d\n",
	       memusage_out,ptr,(char *) ptr + count*nbytes-1,count,nbytes,count*nbytes,file,line));
#else
  debug(printf("Allocating %p to %p -- Calloc of %lu x %lu = %lu bytes in output pool requested from %s:%d\n",
	       ptr,(char *) ptr + count*nbytes-1,count,nbytes,count*nbytes,file,line));
#endif

  if (ptr == NULL) {
    fprintf(stderr,"Failed attempt to calloc %lu x %lu bytes\n",count,nbytes);
    if (file == NULL) {
      RAISE(Mem_Failed);
    } else {
      Except_raise(&Mem_Failed, file, line);
    }
  }

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  pthread_mutex_unlock(&memusage_mutex);
#endif
#endif

  return ptr;
}

void *
Mem_calloc_no_exception (size_t count, size_t nbytes, const char *file, int line) {
  void *ptr;
#ifdef MEMUSAGE
  static struct descriptor *bp;
  unsigned h;

#ifdef HAVE_PTHREAD
  pthread_mutex_lock(&memusage_mutex);
  long int memusage_std_heap;
  char *threadname;
#endif
#endif

  if (count <= 0) {
    fprintf(stderr,"Failed attempt to allocate %lu x %lu bytes\n",count,nbytes);
    if (file == NULL) {
      RAISE(Mem_Failed);
    } else {
      Except_raise(&Mem_Failed, file, line);
    }
  }
  assert(nbytes > 0);

  ptr = calloc(count, nbytes);

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  threadname = (char *) pthread_getspecific(key_threadname);
  memusage_std_heap = (long int) pthread_getspecific(key_memusage_std_heap);
  memusage_std_heap += count*nbytes;
  pthread_setspecific(key_memusage_std_heap,(void *) memusage_std_heap);
#else
  memusage_std_heap += count*nbytes;
#endif
  h = hash(ptr,htab);
  bp = malloc(sizeof(*bp));
  bp->link = htab[h];
  bp->ptr = ptr;
  bp->size = count*nbytes;
  htab[h] = bp;
#endif

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  pthread_mutex_unlock(&memusage_mutex);
#endif
#endif

  return ptr;
}

void 
Mem_free (void *ptr, const char *file, int line) {
#ifdef MEMUSAGE
  struct descriptor *bp;
  size_t nbytes;

#ifdef HAVE_PTHREAD
  pthread_mutex_lock(&memusage_mutex);
  long int memusage_std_heap;
  char *threadname;
#endif
#endif

#ifdef TRAP
  if (ptr == trap_location) {
    printf("Trap: Location %p freed at %s:%d\n",ptr,file,line);
  }
#endif

  if (ptr) {
#ifdef MEMUSAGE
    if ((bp = find(ptr)) == NULL) {
      Except_raise(&Mem_Failed, file, line);
    } else {
      nbytes = bp->size;
#ifdef HAVE_PTHREAD
      threadname = (char *) pthread_getspecific(key_threadname);
      memusage_std_heap = (long int) pthread_getspecific(key_memusage_std_heap);
      memusage_std_heap -= nbytes;
      pthread_setspecific(key_memusage_std_heap,(void *) memusage_std_heap);
#else
      memusage_std_heap -= nbytes;
#endif
      free(bp);
    }
#endif

#ifdef MEMUSAGE
    debug(printf("%ld %s: Freeing %p in standard pool at %s:%d (%ld bytes)\n",
		 memusage_std_heap,threadname,ptr,file,line,nbytes));
#else
    debug(printf("Freeing %p in standard pool at %s:%d\n",ptr,file,line));
#endif
    free(ptr);
  }

#ifdef TRAP
  if (startp > 0 && *trap_location != trap_contents) {
      printf("Value changed at location %p.  Old value was %p.  New value is %p.  Observed during free at %s:%d\n",
	     trap_location,trap_contents,*trap_location,file,line);
      fflush(stdout);
      trap_contents = * (void **) trap_location;
  }
#endif

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  pthread_mutex_unlock(&memusage_mutex);
#endif
#endif

  return;
}


void 
Mem_free_keep (void *ptr, const char *file, int line) {
#ifdef MEMUSAGE
  struct descriptor *bp;
  size_t nbytes;

#ifdef HAVE_PTHREAD
  pthread_mutex_lock(&memusage_mutex);
  long int memusage_keep;
  char *threadname;
#endif
#endif

#ifdef TRAP
  if (ptr == trap_location) {
    printf("Trap: Location %p freed at %s:%d\n",ptr,file,line);
  }
#endif

  if (ptr) {
#ifdef MEMUSAGE
    if ((bp = find(ptr)) == NULL) {
      Except_raise(&Mem_Failed, file, line);
    } else {
      nbytes = bp->size;
#ifdef HAVE_PTHREAD
      threadname = (char *) pthread_getspecific(key_threadname);
      memusage_keep = (long int) pthread_getspecific(key_memusage_keep);
      memusage_keep -= nbytes;
      pthread_setspecific(key_memusage_keep,(void *) memusage_keep);
#else
      memusage_keep -= nbytes;
#endif
      free(bp);
    }
#endif

#ifdef MEMUSAGE
    debug(printf("%ld %s-keep: Freeing %p in keep pool at %s:%d (%ld bytes)\n",
		 memusage_keep,threadname,ptr,file,line,nbytes));
#else
    debug(printf("Freeing %p in keep pool at %s:%d\n",ptr,file,line));
#endif
    free(ptr);
  }

#ifdef TRAP
  if (startp > 0 && *trap_location != trap_contents) {
      printf("Value changed at location %p.  Old value was %p.  New value is %p.  Observed during free at %s:%d\n",
	     trap_location,trap_contents,*trap_location,file,line);
      fflush(stdout);
      trap_contents = * (void **) trap_location;
  }
#endif

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  pthread_mutex_unlock(&memusage_mutex);
#endif
#endif

  return;
}


void 
Mem_free_in (void *ptr, const char *file, int line) {
#ifdef MEMUSAGE
  struct descriptor *bp;
  size_t nbytes;

#ifdef HAVE_PTHREAD
  pthread_mutex_lock(&memusage_mutex);
#endif
#endif

#ifdef TRAP
  if (ptr == trap_location) {
    printf("Trap: Location %p freed at %s:%d\n",ptr,file,line);
  }
#endif

  if (ptr) {
#ifdef MEMUSAGE
    if ((bp = find(ptr)) == NULL) {
      Except_raise(&Mem_Failed, file, line);
    } else {
      nbytes = bp->size;
      memusage_in -= nbytes;
      free(bp);
    }
#endif

#ifdef MEMUSAGE
    debug(printf("%ld IN: Freeing %p in input pool at %s:%d (%ld bytes)\n",
		 memusage_in,ptr,file,line,nbytes));
#else
    debug(printf("Freeing %p in input pool at %s:%d\n",ptr,file,line));
#endif
    free(ptr);
  }

#ifdef TRAP
  if (startp > 0 && *trap_location != trap_contents) {
      printf("Value changed at location %p.  Old value was %p.  New value is %p.  Observed during free at %s:%d\n",
	     trap_location,trap_contents,*trap_location,file,line);
      fflush(stdout);
      trap_contents = * (void **) trap_location;
  }
#endif

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  pthread_mutex_unlock(&memusage_mutex);
#endif
#endif

  return;
}

void 
Mem_free_out (void *ptr, const char *file, int line) {
#ifdef MEMUSAGE
  struct descriptor *bp;
  size_t nbytes;

#ifdef HAVE_PTHREAD
  pthread_mutex_lock(&memusage_mutex);
#endif
#endif

#ifdef TRAP
  if (ptr == trap_location) {
    printf("Trap: Location %p freed at %s:%d\n",ptr,file,line);
  }
#endif

  if (ptr) {
#ifdef MEMUSAGE
    if ((bp = find(ptr)) == NULL) {
      Except_raise(&Mem_Failed, file, line);
    } else {
      nbytes = bp->size;
      memusage_out -= nbytes;
      free(bp);
    }
#endif

#ifdef MEMUSAGE
    debug(printf("%ld OUT: Freeing %p in output pool at %s:%d (%ld bytes)\n",
		 memusage_out,ptr,file,line,nbytes));
#else
    debug(printf("Freeing %p in output pool at %s:%d\n",ptr,file,line));
#endif
    free(ptr);
  }

#ifdef TRAP
  if (startp > 0 && *trap_location != trap_contents) {
      printf("Value changed at location %p.  Old value was %p.  New value is %p.  Observed during free at %s:%d\n",
	     trap_location,trap_contents,*trap_location,file,line);
      fflush(stdout);
      trap_contents = * (void **) trap_location;
  }
#endif

#ifdef MEMUSAGE
#ifdef HAVE_PTHREAD
  pthread_mutex_unlock(&memusage_mutex);
#endif
#endif

  return;
}


void *
Mem_resize (void *ptr, size_t nbytes, const char *file, int line) {
  assert(ptr);
  assert(nbytes > 0);
  ptr = realloc(ptr, nbytes);
  if (ptr == NULL) {
    fprintf(stderr,"Failed attempt to realloc %lu bytes\n",nbytes);
    if (file == NULL) {
      RAISE(Mem_Failed);
    } else {
      Except_raise(&Mem_Failed, file, line);
    }
  }
  return ptr;
}
