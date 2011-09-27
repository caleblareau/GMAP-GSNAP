static char rcsid[] = "$Id: iit-read.c,v 1.133 2010-07-27 01:10:41 twu Exp $";
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "iit-read.h"
#include "iitdef.h"

#ifdef WORDS_BIGENDIAN
#include "bigendian.h"
#else
#include "littleendian.h"
#endif

#include <stdlib.h>		/* For qsort */
#include <string.h>		/* For memset */
#include <strings.h>
#include <ctype.h>		/* For isspace */
#ifdef HAVE_UNISTD_H
#include <unistd.h>		/* For mmap on Linux */
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>		/* For open, fstat, and mmap */
#endif
/* Not sure why this was included
#include <sys/param.h>
*/
#ifdef HAVE_FCNTL_H
#include <fcntl.h>		/* For open */
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>		/* For open and fstat */
#endif
#include <sys/mman.h>		/* For mmap and madvise */
#include <math.h>		/* For qsort */
#include <errno.h>		/* For perror */
#include "assert.h"
#include "except.h"
#include "mem.h"
#include "access.h"
#include "fopen.h"
#include "uintlist.h"
#include "intlist.h"

/* Note: if sizeof(int) or sizeof(unsigned int) are not 4, then the below code is faulty */


/* Integer interval tree. */

/*
 * n intervals;
 *   specified by their indices e[1..n]
 *   and endpoint-access function:
 *                low  (e[i])
 *                high (e[i])
 *        is_contained (x, e[i])
 *   eg:
 *        interval e[i]          ... "[" low (e[i]) "," high (e[i]) ")"
 *        is_contained (x, e[i]) ... (    (low (e[i]) <= x
 *                                    and (x < high (e[i]))
 */

/*--------------------------------------------------------------------------*/

#ifdef DEBUG
#define debug(x) x
#else
#define debug(x)
#endif

/* Timing */
#ifdef DEBUG1
#define debug1(x) x
#else
#define debug1(x)
#endif

/* Flanking */
#ifdef DEBUG2
#define debug2(x) x
#else
#define debug2(x)
#endif

/* Fileio read() */
#ifdef DEBUG3
#define debug3(x) x
#else
#define debug3(x)
#endif


#define T IIT_T

static void
file_move_absolute (int fd, off_t offset, off_t objsize, unsigned int n) {
  off_t position = offset + n*objsize;

  if (lseek(fd,position,SEEK_SET) < 0) {
    perror("Error in gmap, file_move_label");
    exit(9);
  }
  return;
}


char *
IIT_name (T this) {
  return this->name;
}

int
IIT_version (T this) {
  return this->version;
}

int
IIT_total_nintervals (T this) {
  return this->total_nintervals;
}

int
IIT_nintervals (T this, int divno) {
  return this->nintervals[divno];
}


int
IIT_ntypes (T this) {
  return this->ntypes;
}

int
IIT_nfields (T this) {
  return this->nfields;
}


unsigned int
IIT_length (T this, int index) {
  Interval_T interval;

  interval = &(this->intervals[0][index-1]);
  return Interval_length(interval);
}


unsigned int
IIT_divlength (T this, char *divstring) {
  unsigned int max = 0U;
  Interval_T interval;
  int divno, i;

  divno = IIT_divint(this,divstring);
  for (i = 0; i < this->nintervals[divno]; i++) {
    interval = &(this->intervals[divno][i]);
    if (Interval_high(interval) > max) {
      max = Interval_high(interval);
    }
  }
  /* Convert from zero-based coordinate */
  return max+1U;
}


/* Assumes intervals are stored using universal coordinates */
unsigned int
IIT_totallength (T this) {
  unsigned int max = 0U;
  Interval_T interval;
  int divno, i;

  for (divno = 0; divno < this->ndivs; divno++) {
    for (i = 0; i < this->nintervals[divno]; i++) {
      interval = &(this->intervals[divno][i]);
      if (Interval_high(interval) > max) {
	max = Interval_high(interval);
      }
    }
  }
  /* Convert from zero-based coordinate */
  return max+1U;
}


Interval_T
IIT_interval (T this, int index) {
  assert(index <= this->total_nintervals);
  return &(this->intervals[0][index-1]); /* Convert to 0-based */
}

unsigned int
IIT_interval_low (T this, int index) {
  Interval_T interval;

  assert(index <= this->total_nintervals);
  interval = &(this->intervals[0][index-1]);
  return Interval_low(interval);
}

unsigned int
IIT_interval_high (T this, int index) {
  Interval_T interval;

  assert(index <= this->total_nintervals);
  interval = &(this->intervals[0][index-1]);
  return Interval_high(interval);
}

int
IIT_interval_sign (T this, int index) {
  Interval_T interval;

  assert(index <= this->total_nintervals);
  interval = &(this->intervals[0][index-1]);
  return Interval_sign(interval);
}


void
IIT_interval_bounds (unsigned int *low, unsigned int *high, T this, int index) {
  Interval_T interval;

  assert(index <= this->total_nintervals);
  interval = &(this->intervals[0][index-1]);
  *low = Interval_low(interval);
  *high = Interval_high(interval);
  return;
}

int
IIT_index (T this, int divno, int i) {
  return this->cum_nintervals[divno] + i + 1; /* 1-based */
}



int
IIT_ndivs (T this) {
  return this->ndivs;
}

/* The iit file has a '\0' after each string, so functions know where
   it ends */
char *
IIT_divstring (T this, int divno) {
  unsigned int start;

  start = this->divpointers[divno];
  return &(this->divstrings[start]);
}

int
IIT_divint (T this, char *divstring) {
  int i = 0;			/* Actually divstring for divno 0 is NULL */
  unsigned int start;

  if (divstring == NULL) {
    return 0;
  } else {
    while (i < this->ndivs) {
      start = this->divpointers[i];
      if (!strcmp(divstring,&(this->divstrings[start]))) {
	return i;
      }
      i++;
    }

    return -1;
  }
}

char *
IIT_divstring_from_index (T this, int index) {
  int divno = 1;
  unsigned int start;

  while (divno <= this->ndivs) {
    /* Checked on existing iit file to confirm we need >= and not > */
    if (this->cum_nintervals[divno] >= index) {
      start = this->divpointers[divno-1];
      return &(this->divstrings[start]);
    }
    divno++;
  }
  return (char *) NULL;
}


/* Maps from chromosome_iit chrnums to iit divints */
int *
IIT_divint_crosstable (T chromosome_iit, T iit) {
  int *crosstable;
  int chrnum;
  char *chr;
  unsigned int start;

  crosstable = (int *) CALLOC(chromosome_iit->total_nintervals+1,sizeof(int));

  for (chrnum = 0; chrnum < chromosome_iit->total_nintervals; chrnum++) {
#ifdef WORDS_BIGENDIAN
    /* chromosome_iit should be version 1 */
    start = Bigendian_convert_uint(chromosome_iit->labelpointers[chrnum]);
#else
    start = chromosome_iit->labelpointers[chrnum];
#endif
    chr = &(chromosome_iit->labels[start]);

    /* upon lookup, chrnum from IIT_get_one(chromosome_iit)
       is 1-based, so we need to store in chrnum+1 */
    crosstable[chrnum+1] = IIT_divint(iit,chr);	
    if (crosstable[chrnum+1] < 0) {
      /* fprintf(stderr,"Note: No splicesites are provided in chr %s\n",chr); */
    } else {
      /* fprintf(stderr,"chrnum %d maps to splicesite divint %d\n",chrnum,crosstable[chrnum]); */
    }
  }

  return crosstable;
}


/* The iit file has a '\0' after each string, so functions know where
   it ends */
char *
IIT_typestring (T this, int type) {
  unsigned int start;

  start = this->typepointers[type];
  return &(this->typestrings[start]);
}

int
IIT_typeint (T this, char *typestring) {
  int i = 0;
  unsigned int start;

  while (i < this->ntypes) {
    start = this->typepointers[i];
    if (!strcmp(typestring,&(this->typestrings[start]))) {
      return i;
    }
    i++;
  }

  return -1;
}

char *
IIT_fieldstring (T this, int fieldint) {
  unsigned int start;

  start = this->fieldpointers[fieldint];
  return &(this->fieldstrings[start]);
}

int
IIT_fieldint (T this, char *fieldstring) {
  int i = 0;
  unsigned int start;

  while (i < this->nfields) {
    start = this->fieldpointers[i];
    if (!strcmp(fieldstring,&(this->fieldstrings[start]))) {
      return i;
    }
    i++;
  }

  return -1;
}


char *
IIT_label (T this, int index, bool *allocp) {
  int recno;
#ifdef HAVE_64_BIT
  UINT8 start;
#else
  unsigned int start;
#endif

  recno = index - 1; /* Convert to 0-based */

#ifdef WORDS_BIGENDIAN
#ifdef HAVE_64_BIT
  if (this->version >= 4) {
    start = Bigendian_convert_uint8(this->labelpointers8[recno]);
  } else {
    start = Bigendian_convert_uint(this->labelpointers[recno]);
  }
#else
  start = Bigendian_convert_uint(this->labelpointers[recno]);
#endif
#else
#ifdef HAVE_64_BIT
  if (this->version >= 4) {
    start = this->labelpointers8[recno];
  } else {
    start = this->labelpointers[recno];
  }
#else
  start = this->labelpointers[recno];
#endif
#endif
  *allocp = false;
  return &(this->labels[start]);
}


/* The iit file has a '\0' after each string, so functions know where
   it ends */
char *
IIT_annotation (T this, int index, bool *allocp) {
  int recno;
#ifdef HAVE_64_BIT
  UINT8 start;
#else
  unsigned int start;
#endif

  recno = index - 1; /* Convert to 0-based */
#ifdef WORDS_BIGENDIAN
#ifdef HAVE_64_BIT
  if (this->version >= 4) {
    start = Bigendian_convert_uint8(this->annotpointers8[recno]);
  } else {
    start = Bigendian_convert_uint(this->annotpointers[recno]);
  }
#else
  start = Bigendian_convert_uint(this->annotpointers[recno]);
#endif
#else
#ifdef HAVE_64_BIT
  if (this->version >= 4) {
    start = this->annotpointers8[recno];
  } else {
    start = this->annotpointers[recno];
  }
#else
  start = this->annotpointers[recno];
#endif
#endif
  *allocp = false;
  return &(this->annotations[start]);
}

/* The iit file has a '\0' after each string, so functions know where
   it ends */
char
IIT_annotation_firstchar (T this, int index) {
  int recno;
#ifdef HAVE_64_BIT
  UINT8 start;
#else
  unsigned int start;
#endif

  recno = index - 1; /* Convert to 0-based */

#ifdef WORDS_BIGENDIAN
#ifdef HAVE_64_BIT
  if (this->version >= 4) {
    start = Bigendian_convert_uint8(this->annotpointers8[recno]);
  } else {
    start = Bigendian_convert_uint(this->annotpointers[recno]);
  }
#else
  start = Bigendian_convert_uint(this->annotpointers[recno]);
#endif
#else
#ifdef HAVE_64_BIT
  if (this->version >= 4) {
    start = this->annotpointers8[recno];
  } else {
    start = this->annotpointers[recno];
  }
#else
  start = this->annotpointers[recno];
#endif
#endif

  return this->annotations[start];
}

unsigned int
IIT_annotation_strlen (T this, int index) {
  int recno;
#ifdef HAVE_64_BIT
  UINT8 start, end;
#else
  unsigned int start, end;
#endif

  recno = index - 1; /* Convert to 0-based */

#ifdef WORDS_BIGENDIAN
#ifdef HAVE_64_BIT
  if (this->version >= 4) {
    start = Bigendian_convert_uint8(this->annotpointers8[recno]);
    end = Bigendian_convert_uint8(this->annotpointers8[recno+1]);
  } else {
    start = Bigendian_convert_uint(this->annotpointers[recno]);
    end = Bigendian_convert_uint(this->annotpointers[recno+1]);
  }
#else
  start = Bigendian_convert_uint(this->annotpointers[recno]);
  end = Bigendian_convert_uint(this->annotpointers[recno+1]);
#endif
#else
#ifdef HAVE_64_BIT
  if (this->version >= 4) {
    start = this->annotpointers8[recno];
    end = this->annotpointers8[recno+1];
  } else {
    start = this->annotpointers[recno];
    end = this->annotpointers[recno+1];
  }
#else
  start = this->annotpointers[recno];
  end = this->annotpointers[recno+1];
#endif
#endif

  /*
  if (strlen(&(this->annotations[start])) != (end - start - 1)) {
    printf("Problem with %s: %d != %u\n",
    &(this->labels[this->labelpointers[recno]]),strlen(&(this->annotations[start])),end-start-1);
    abort();
  } else {
    printf("Okay %s: %d == %u\n",
    &(this->labels[this->labelpointers[recno]]),strlen(&(this->annotations[start])),end-start-1);
  }
  */

  return (end - start - 1);	/* Subtract terminal '\0' */
}

/* Always allocated */
char *
IIT_fieldvalue (T this, int index, int fieldint) {
  char *fieldvalue, *annotation, *p, *q;
  int recno, fieldno = 0, fieldlen;
#ifdef HAVE_64_BIT
  UINT8 start;
#else
  unsigned int start;
#endif
  bool allocp;

  recno = index - 1; /* Convert to 0-based */
#ifdef WORDS_BIGENDIAN
#ifdef HAVE_64_BIT
  if (this->version >= 4) {
    start = Bigendian_convert_uint8(this->annotpointers8[recno]);
  } else {
    start = Bigendian_convert_uint(this->annotpointers[recno]);
  }
#else
  start = Bigendian_convert_uint(this->annotpointers[recno]);
#endif
#else
#ifdef HAVE_64_BIT
  if (this->version >= 4) {
    start = this->annotpointers8[recno];
  } else {
    start = this->annotpointers[recno];
  }
#else
  start = this->annotpointers[recno];
#endif
#endif
  annotation = &(this->annotations[start]);
  allocp = false;

  p = annotation;
  while (*p != '\0' && fieldno < fieldint) {
    if (*p == '\n') {
      fieldno++;
    }
    p++;
  }

  if (*p == '\0') {
    fieldvalue = (char *) CALLOC(1,sizeof(char));
    fieldvalue[0] = '\0';
  } else {
    q = p;
    while (*q != '\0' && *q != '\n') {
      q++;
    }
    fieldlen = (q - p)/sizeof(char);
    fieldvalue = (char *) CALLOC(fieldlen+1,sizeof(char));
    strncpy(fieldvalue,p,fieldlen);
  }

  if (allocp == true) {
    FREE(annotation);
  }

  return fieldvalue;
}


void
IIT_dump_typestrings (FILE *fp, T this) {
  int type;
  unsigned int start;

  for (type = 0; type < this->ntypes; type++) {
    start = this->typepointers[type];
    fprintf(fp,"%d\t%s\n",type,&(this->typestrings[start]));
  }
  return;
}

void
IIT_dump_fieldstrings (FILE *fp, T this) {
  int field;
  unsigned int start;

  for (field = 0; field < this->nfields; field++) {
    start = this->fieldpointers[field];
    fprintf(fp,"%d\t%s\n",field,&(this->fieldstrings[start]));
  }
  return;
}

void
IIT_dump_labels (FILE *fp, T this) {
  int i;
#ifdef HAVE_64_BIT
  UINT8 start;
#else
  unsigned int start;
#endif
  char *label;

  for (i = 0; i < this->total_nintervals; i++) {
#ifdef WORDS_BIGENDIAN
#ifdef HAVE_64_BIT
    if (this->version >= 4) {
      start = Bigendian_convert_uint8(this->labelpointers8[i]);
    } else {
      start = Bigendian_convert_uint(this->labelpointers[i]);
    }
#else
    start = Bigendian_convert_uint(this->labelpointers[i]);
#endif
#else
#ifdef HAVE_64_BIT
    if (this->version >= 4) {
      start = this->labelpointers8[i];
    } else {
      start = this->labelpointers[i];
    }
#else
    start = this->labelpointers[i];
#endif
#endif
    label = &(this->labels[start]);
    fprintf(fp,"%s ",label);
  }
  fprintf(fp,"\n");
  return;
}


void
IIT_dump (T this, bool annotationonlyp) {
  int divno, i;
  Interval_T interval;
  char *divstring;
#if 0
  bool allocp;
  char *label, *annotation;
  int index = 0;
#endif
  char *labelptr, *annotptr, c;

  labelptr = this->labels;
  annotptr = this->annotations;
  for (divno = 0; divno < this->ndivs; divno++) {
    divstring = IIT_divstring(this,divno);
    for (i = 0; i < this->nintervals[divno]; i++) {
      printf(">");
      while ((c = *labelptr++) != '\0') {
	printf("%c",c);
      }
      printf(" ");

      if (divno > 0) {
	/* zeroth divno has empty string */
	printf("%s:",divstring);
      }

      interval = &(this->intervals[divno][i]);
      if (Interval_sign(interval) < 0) {
	printf("%u..%u",Interval_high(interval),Interval_low(interval));
      } else {
	printf("%u..%u",Interval_low(interval),Interval_high(interval));
      }
      if (Interval_type(interval) > 0) {
	printf(" %s",IIT_typestring(this,Interval_type(interval)));
      }
      printf("\n");

      while ((c = *annotptr++) != '\0') {
	printf("%c",c);
      }
    }
  }

  return;

#if 0
  /* It looks like annot_length is incorrect */
  ptr = &(this->annotations[0]);
  end = &(this->annotations[this->annotpointers[this->total_nintervals]]);
  while (ptr < end) {
    if (*ptr != '\0') {
      printf("%c",*ptr++);
    }
  }
  exit(0);
#endif

#if 0
  /* Original version */
  for (divno = 0; divno < this->ndivs; divno++) {
    divstring = IIT_divstring(this,divno);
    for (i = 0; i < this->nintervals[divno]; i++) {
      if (annotationonlyp == false) {
	label = IIT_label(this,index+1,&allocp);
	printf(">%s ",label);
	if (allocp == true) {
	  FREE(label);
	}

	if (divno > 0) {
	  /* zeroth divno has empty string */
	  printf("%s:",divstring);
	}

	interval = &(this->intervals[divno][i]);
	if (Interval_sign(interval) < 0) {
	  printf("%u..%u",Interval_high(interval),Interval_low(interval));
	} else {
	  printf("%u..%u",Interval_low(interval),Interval_high(interval));
	}
	if (Interval_type(interval) > 0) {
	  printf(" %s",IIT_typestring(this,Interval_type(interval)));
	}
	printf("\n");
      }

      annotation = IIT_annotation(this,index+1,&allocp);
      if (strlen(annotation) == 0) {
	/* Don't print anything */
      } else if (annotation[strlen(annotation)-1] == '\n') {
	printf("%s",annotation);
      } else {
	printf("%s\n",annotation);
      }
      if (allocp == true) {
	FREE(annotation);
      }
      
      index++;
    }
  }

  return;
#endif

}


/* For chromosome.iit file, which is stored in version 1 */
void
IIT_dump_simple (T this) {
  int index = 0, i;
  Interval_T interval;
  unsigned int startpos, endpos;
  char *label;
  bool allocp;

  for (i = 0; i < this->nintervals[0]; i++) {
    interval = &(this->intervals[0][i]);
    label = IIT_label(this,index+1,&allocp);
    printf("%s\t",label);
    if (allocp == true) {
      FREE(label);
    }
    startpos = Interval_low(interval);
    endpos = startpos + Interval_length(interval) - 1U;

    printf("%u..%u\t",startpos+1U,endpos+1U);

    printf("%u",Interval_length(interval));
    if (Interval_type(interval) > 0) {
      printf("\t%s",IIT_typestring(this,Interval_type(interval)));
    }
    printf("\n");

    index++;
  }

  return;
}


/* For chromosome.iit file, which is stored in version 1 */
void
IIT_dump_sam (T this) {
  int index = 0, i;
  Interval_T interval;
  unsigned int startpos;
  char *label;
  bool allocp;

  for (i = 0; i < this->nintervals[0]; i++) {
    interval = &(this->intervals[0][i]);
    label = IIT_label(this,index+1,&allocp);
    printf("@SQ\tSN:%s",label);
    if (allocp == true) {
      FREE(label);
    }
    startpos = Interval_low(interval);
    /* endpos = startpos + Interval_length(interval) - 1U; */

    printf("\tLN:%u",Interval_length(interval));
    printf("\n");

    index++;
  }

  return;
}



/* For contig.iit file, which is stored in version 1 */
void
IIT_dump_version1 (T this, T chromosome_iit, bool directionalp) {
  int index = 0, i, chromosome_index;
  Interval_T interval;
  unsigned int startpos, endpos, chrstart, chrend, chroffset;
  char *label, firstchar, *chrstring;
  bool allocp;

  for (i = 0; i < this->nintervals[0]; i++) {
    interval = &(this->intervals[0][i]);
    label = IIT_label(this,index+1,&allocp);
    printf("%s\t",label);
    if (allocp == true) {
      FREE(label);
    }
    startpos = Interval_low(interval);
    endpos = startpos + Interval_length(interval) - 1U;

    chromosome_index = IIT_get_one(chromosome_iit,/*divstring*/NULL,startpos,startpos);
    chroffset = Interval_low(IIT_interval(chromosome_iit,chromosome_index));
    chrstart = startpos - chroffset;
    chrend = endpos - chroffset;

    chrstring = IIT_label(chromosome_iit,chromosome_index,&allocp); 

    if (directionalp == false) {
      printf("%u..%u\t",startpos+1U,endpos+1U);
      printf("%s:%u..%u\t",chrstring,chrstart+1U,chrend+1U);

    } else if (this->version <= 1) {
      firstchar = IIT_annotation_firstchar(this,index+1);
      if (firstchar == '-') {
	printf("%u..%u\t",endpos+1U,startpos+1U);
	printf("%s:%u..%u\t",chrstring,chrend+1U,chrstart+1U);
      } else {
	printf("%u..%u\t",startpos+1U,endpos+1U);
	printf("%s:%u..%u\t",chrstring,chrstart+1U,chrend+1U);
      }
    } else {
      if (Interval_sign(interval) < 0) {
	printf("%u..%u\t",endpos+1U,startpos+1U);
	printf("%s:%u..%u\t",chrstring,chrend+1U,chrstart+1U);
      } else {
	printf("%u..%u\t",startpos+1U,endpos+1U);
	printf("%s:%u..%u\t",chrstring,chrstart+1U,chrend+1U);
      }
    }
    if (allocp == true) {
      FREE(chrstring);
    }
    
    printf("%u",Interval_length(interval));
    if (Interval_type(interval) > 0) {
      printf("\t%s",IIT_typestring(this,Interval_type(interval)));
    }
    printf("\n");
    
    index++;
  }

  return;
}


/* For higher version files, which are divided into divs */
void
IIT_dump_formatted (T this, bool directionalp) {
  int divno, index = 0, i;
  Interval_T interval;
  unsigned int startpos, endpos;
  char *label, *divstring, firstchar;
  bool allocp;

  for (divno = 0; divno < this->ndivs; divno++) {
    divstring = IIT_divstring(this,divno);
    for (i = 0; i < this->nintervals[divno]; i++) {
      interval = &(this->intervals[divno][i]);
      label = IIT_label(this,index+1,&allocp);
      printf("%s\t",label);
      if (allocp == true) {
	FREE(label);
      }
      startpos = Interval_low(interval);
      endpos = startpos + Interval_length(interval) - 1U;

      if (divno > 0) {
	printf("%s:",divstring);
      }
      if (directionalp == false) {
	printf("%u..%u\t",startpos+1U,endpos+1U);
      } else if (this->version <= 1) {
	firstchar = IIT_annotation_firstchar(this,index+1);
	if (firstchar == '-') {
	  printf("%u..%u\t",endpos+1U,startpos+1U);
	} else {
	  printf("%u..%u\t",startpos+1U,endpos+1U);
	}
      } else {
	if (Interval_sign(interval) < 0) {
	  printf("%u..%u\t",endpos+1U,startpos+1U);
	} else {
	  printf("%u..%u\t",startpos+1U,endpos+1U);
	}
      }

      printf("%u",Interval_length(interval));
      if (Interval_type(interval) > 0) {
	printf("\t%s",IIT_typestring(this,Interval_type(interval)));
      }
      printf("\n");

      index++;
    }
  }

  return;
}


#if 0
static int
uint_cmp (const void *x, const void *y) {
  unsigned int a = * (unsigned int *) x;
  unsigned int b = * (unsigned int *) y;

  if (a < b) {
    return -1;
  } else if (a > b) {
    return +1;
  } else {
    return 0;
  }
}

/* Need to work on */
unsigned int *
IIT_transitions (int **signs, int *nedges, T this) { 
  unsigned int *edges, *starts, *ends;
  int nintervals, i, j, k;
  Interval_T interval;
  Uintlist_T startlist = NULL, endlist = NULL;

  for (i = 0; i < this->nintervals; i++) {
    interval = &(this->intervals[i]);
    startlist = Uintlist_push(startlist,Interval_low(interval));
    endlist = Uintlist_push(endlist,Interval_high(interval));
  }

  if (Uintlist_length(startlist) == 0) {
    edges = (unsigned int *) NULL;
    *signs = (int *) NULL;
    *nedges = 0;
  } else {
    starts = Uintlist_to_array(&nintervals,startlist);
    ends = Uintlist_to_array(&nintervals,endlist);
    qsort(starts,nintervals,sizeof(unsigned int),uint_cmp);
    qsort(ends,nintervals,sizeof(unsigned int),uint_cmp);

    *nedges = nintervals+nintervals;
    *signs = (int *) CALLOC(*nedges,sizeof(int));
    edges = (unsigned int *) CALLOC(*nedges,sizeof(unsigned int));
    i = j = k = 0;
    while (i < nintervals && j < nintervals) {
      if (starts[i] <= ends[j]) {
	(*signs)[k] = +1;
	edges[k++] = starts[i++];
      } else {
	(*signs)[k] = -1;
	edges[k++] = ends[j++];
      }
    }
    while (i < nintervals) {
      (*signs)[k] = +1;
      edges[k++] = starts[i++];
    }
    while (j < nintervals) {
      (*signs)[k] = -1;
      edges[k++] = ends[j++];
    }

    FREE(ends);
    FREE(starts);
  }

  Uintlist_free(&endlist);
  Uintlist_free(&startlist);

  return edges;
}

unsigned int *
IIT_transitions_subset (int **signs, int *nedges, T this, int *indices, int nindices) { 
  unsigned int *edges, *starts, *ends;
  int nintervals, i, j, k;
  Interval_T interval;
  Uintlist_T startlist = NULL, endlist = NULL;

  for (k = 0; k < nindices; k++) {
    i = indices[k] - 1;
    interval = &(this->intervals[i]);
    startlist = Uintlist_push(startlist,Interval_low(interval));
    endlist = Uintlist_push(endlist,Interval_high(interval));
  }

  if (Uintlist_length(startlist) == 0) {
    edges = (unsigned int *) NULL;
    *signs = (int *) NULL;
    *nedges = 0;
  } else {
    starts = Uintlist_to_array(&nintervals,startlist);
    ends = Uintlist_to_array(&nintervals,endlist);
    qsort(starts,nintervals,sizeof(unsigned int),uint_cmp);
    qsort(ends,nintervals,sizeof(unsigned int),uint_cmp);

    *nedges = nintervals+nintervals;
    *signs = (int *) CALLOC(*nedges,sizeof(int));
    edges = (unsigned int *) CALLOC(*nedges,sizeof(unsigned int));
    i = j = k = 0;
    while (i < nintervals && j < nintervals) {
      if (starts[i] <= ends[j]) {
	(*signs)[k] = +1;
	edges[k++] = starts[i++];
      } else {
	(*signs)[k] = -1;
	edges[k++] = ends[j++];
      }
    }
    while (i < nintervals) {
      (*signs)[k] = +1;
      edges[k++] = starts[i++];
    }
    while (j < nintervals) {
      (*signs)[k] = -1;
      edges[k++] = ends[j++];
    }

    FREE(ends);
    FREE(starts);
  }

  Uintlist_free(&endlist);
  Uintlist_free(&startlist);

  return edges;
}
#endif


/* For IIT versions <= 2.  Previously sorted by Chrom_compare, but now
   we assume that chromosomes are represented by divs, which are
   pre-sorted by iit_store. */
#if 0
static int
string_compare (const void *x, const void *y) {
  char *a = (char *) x;
  char *b = (char *) y;

  return strcmp(a,b);
}

static int *
sort_matches_by_type (T this, int *matches, int nmatches, bool alphabetizep) {
  int *sorted;
  int type, index, i, j, k = 0, t;
  List_T *intervallists;
  Interval_T *intervals, interval;
  int *matches1, nmatches1, nintervals;
  char *typestring;
  char **strings;

  if (nmatches == 0) {
    return (int *) NULL;
  } else {
    sorted = (int *) CALLOC(nmatches,sizeof(int));
  }
  
  intervallists = (List_T *) CALLOC(this->ntypes,sizeof(List_T));
  for (i = 0; i < nmatches; i++) {
    index = matches[i];
    interval = &(this->intervals[0][index-1]);
    type = Interval_type(interval);
    intervallists[type] = List_push(intervallists[type],(void *) interval);
  }

  if (alphabetizep == true) {
    strings = (char **) CALLOC(this->ntypes,sizeof(char *));

    for (type = 0; type < this->ntypes; type++) {
      typestring = IIT_typestring(this,type);
      strings[type] = (char *) CALLOC(strlen(typestring)+1,sizeof(char));
      strcpy(strings[type],typestring);
    }
    qsort(strings,this->ntypes,sizeof(char *),string_compare);
  }

  for (t = 0; t < this->ntypes; t++) {
    if (alphabetizep == false) {
      type = t;
      typestring = IIT_typestring(this,type);
    } else {
      typestring = strings[t];
      type = IIT_typeint(this,typestring);
    }

    if ((nintervals = List_length(intervallists[type])) > 0) {
      intervals = (Interval_T *) List_to_array(intervallists[type],/*end*/NULL);
      qsort(intervals,nintervals,sizeof(Interval_T),Interval_cmp);

      i = 0;
      while (i < nintervals) {
	interval = intervals[i];
	matches1 = IIT_get_exact_multiple(&nmatches1,this,/*divstring*/NULL,Interval_low(interval),Interval_high(interval),type);
	if (matches1 != NULL) {
	  for (j = 0; j < nmatches1; j++) {
	    sorted[k++] = matches1[j];
	  }
	  i += nmatches1;
	  FREE(matches1);
	}
      }

      FREE(intervals);
      List_free(&(intervallists[type]));
    }

  }

  if (alphabetizep == true) {
    for (t = 0; t < this->ntypes; t++) {
      FREE(strings[t]);
    }
    FREE(strings);
  }

  FREE(intervallists);
  return sorted;
}
#endif


/* For IIT versions >= 3.  Assumes that matches are all in the same
   div */
static int *
sort_matches_by_position (T this, int *matches, int nmatches, char *divstring, int divno) {
  int *sorted, index, i;
  struct Interval_windex_T *intervals;

  if (nmatches == 0) {
    return (int *) NULL;
  } else {
    intervals = (struct Interval_windex_T *) CALLOC(nmatches,sizeof(struct Interval_windex_T));
    for (i = 0; i < nmatches; i++) {
      index = intervals[i].index = matches[i];
      intervals[i].interval = &(this->intervals[0][index-1]); /* Ignore divno here, because we have offset index */
    }
    qsort(intervals,nmatches,sizeof(struct Interval_windex_T),Interval_windex_cmp);

    sorted = (int *) CALLOC(nmatches,sizeof(int));
    for (i = 0; i < nmatches; i++) {
      sorted[i] = intervals[i].index;
    }

    FREE(intervals);
    return sorted;
  }
}


static int *
sort_matches_by_position_with_divno (T this, int *matches, int nmatches, int divno) {
  int *sorted, index, i;
  struct Interval_windex_T *intervals;

  if (nmatches == 0) {
    return (int *) NULL;
  } else {
    intervals = (struct Interval_windex_T *) CALLOC(nmatches,sizeof(struct Interval_windex_T));
    for (i = 0; i < nmatches; i++) {
      index = intervals[i].index = matches[i];
      intervals[i].interval = &(this->intervals[0][index-1]); /* Ignore divno here, because we have offset index */
    }
    qsort(intervals,nmatches,sizeof(struct Interval_windex_T),Interval_windex_cmp);

    sorted = (int *) CALLOC(nmatches,sizeof(int));
    for (i = 0; i < nmatches; i++) {
      sorted[i] = intervals[i].index;
    }

    FREE(intervals);
    return sorted;
  }
}


#if 0
/* Need to work on */
void
IIT_dump_counts (T this, bool alphabetizep) { 
  int type, divno, index, i, j, k, t;
  Interval_T interval;
  Uintlist_T *startlists, *endlists;
  int *matches, nmatches, nintervals;
  unsigned int *starts, *ends, edge;
  char *typestring;
  Chrom_T *chroms;

  startlists = (Uintlist_T *) CALLOC(this->ntypes,sizeof(Uintlist_T));
  endlists = (Uintlist_T *) CALLOC(this->ntypes,sizeof(Uintlist_T));
  for (i = 0; i < this->nintervals; i++) {
    interval = &(this->intervals[i]);
    type = Interval_type(interval);
    startlists[type] = Uintlist_push(startlists[type],Interval_low(interval));
    endlists[type] = Uintlist_push(endlists[type],Interval_high(interval));
  }

  if (alphabetizep == true) {
    chroms = (Chrom_T *) CALLOC(this->ntypes,sizeof(Chrom_T));

    for (type = 0; type < this->ntypes; type++) {
      typestring = IIT_typestring(this,type);
      chroms[type] = Chrom_from_string(typestring);
    }
    qsort(chroms,this->ntypes,sizeof(Chrom_T),Chrom_compare);
  }

  for (t = 0; t < this->ntypes; t++) {
    if (alphabetizep == false) {
      type = t;
      typestring = IIT_typestring(this,type);
    } else {
      typestring = Chrom_string(chroms[t]); /* Not allocated; do not free */
      type = IIT_typeint(this,typestring);
    }

    if (Uintlist_length(startlists[type]) > 0) {
      starts = Uintlist_to_array(&nintervals,startlists[type]);
      ends = Uintlist_to_array(&nintervals,endlists[type]);
      qsort(starts,nintervals,sizeof(unsigned int),uint_cmp);
      qsort(ends,nintervals,sizeof(unsigned int),uint_cmp);

      i = j = 0;
      while (i < nintervals || j < nintervals) {
	if (i >= nintervals && j >= nintervals) {
	  /* done */
	  matches = (int *) NULL;
	} else if (i >= nintervals) {
	  /* work on remaining ends */
	  edge = ends[j++];
	  matches = IIT_get_typed(&nmatches,this,edge,edge,type,/*sortp*/false);
	  printf("%s\t%u\tend\t%d",typestring,edge,nmatches);
	  while (j < nintervals && ends[j] == edge) {
	    j++;
	  }
	} else if (j >= nintervals) {
	  /* work on remaining starts */
	  edge = starts[i++];
	  matches = IIT_get_typed(&nmatches,this,edge,edge,type,/*sortp*/false);
	  printf("%s\t%u\tstart\t%d",typestring,edge,nmatches);
	  while (i < nintervals && starts[i] == edge) {
	    i++;
	  }
	} else if (starts[i] <= ends[j]) {
	  edge = starts[i++];
	  matches = IIT_get_typed(&nmatches,this,edge,edge,type,/*sortp*/false);
	  printf("%s\t%u\tstart\t%d",typestring,edge,nmatches);
	  while (i < nintervals && starts[i] == edge) {
	    i++;
	  }
	} else {
	  edge = ends[j++];
	  matches = IIT_get_typed(&nmatches,this,edge,edge,type,/*sortp*/false);
	  printf("%s\t%u\tend\t%d",typestring,edge,nmatches);
	  while (j < nintervals && ends[j] == edge) {
	    j++;
	  }
	}

	if (matches != NULL) {
	  index = matches[0];
	  label = IIT_label(this,index,&allocp);
	  printf("\t%s",label);
	  if (allocp == true) {
	    FREE(label);
	  }

	  for (k = 1; k < nmatches; k++) {
	    index = matches[k];
	    label = IIT_label(this,index,&allocp);
	    printf(",%s",label);
	    if (allocp == true) {
	      FREE(label);
	    }
	  }
	  printf("\n");
	  FREE(matches);
	}
      }

      Uintlist_free(&(endlists[type]));
      Uintlist_free(&(startlists[type]));
      FREE(ends);
      FREE(starts);
    }

  }

  if (alphabetizep == true) {
    for (t = 0; t < this->ntypes; t++) {
      Chrom_free(&(chroms[t]));
    }
    FREE(chroms);
  }

  FREE(endlists);
  FREE(startlists);

  return;
}
#endif


/************************************************************************
 * For file format, see iit-write.c
 ************************************************************************/

void
IIT_free (T *old) {
  int divno;

  if (*old != NULL) {
    if ((*old)->name != NULL) {
      FREE((*old)->name);
    }

    if ((*old)->access == MMAPPED) {
#ifdef HAVE_MMAP
      munmap((void *) (*old)->annot_mmap,(*old)->annot_length);
      munmap((void *) (*old)->annotpointers_mmap,(*old)->annotpointers_length);
      munmap((void *) (*old)->label_mmap,(*old)->label_length);
      munmap((void *) (*old)->labelpointers_mmap,(*old)->labelpointers_length);
      munmap((void *) (*old)->labelorder_mmap,(*old)->labelorder_length);
#endif
      close((*old)->fd);

    } else if ((*old)->access == FILEIO) {
      FREE((*old)->annotations);
      if ((*old)->version >= 4) {
	FREE((*old)->annotpointers8);
      } else {
	FREE((*old)->annotpointers);
      }
      FREE((*old)->labels);
      if ((*old)->version >= 4) {
	FREE((*old)->labelpointers8);
      } else {
	FREE((*old)->labelpointers);
      }
      FREE((*old)->labelorder);
      /* close((*old)->fd); -- closed in read_annotations */

    } else if ((*old)->access == ALLOCATED) {
      /* Nothing to close.  IIT must have been created by IIT_new. */

    } else {
      abort();
    }

    if ((*old)->fieldstrings != NULL) {
      FREE((*old)->fieldstrings);
    }
    FREE((*old)->fieldpointers);
    FREE((*old)->typestrings);
    FREE((*old)->typepointers);

    FREE((*old)->intervals[0]);
    FREE((*old)->intervals);

    for (divno = 0; divno < (*old)->ndivs; divno++) {
      /* Note: we are depending on Mem_free() to check that these are non-NULL */
      FREE((*old)->nodes[divno]);
      FREE((*old)->omegas[divno]);
      FREE((*old)->sigmas[divno]);
      if ((*old)->alphas != NULL) {
	FREE((*old)->betas[divno]);
	FREE((*old)->alphas[divno]);
      }
    }

    FREE((*old)->nodes);
    FREE((*old)->omegas);
    FREE((*old)->sigmas);
    if ((*old)->alphas != NULL) {
      FREE((*old)->betas);
      FREE((*old)->alphas);
    }

    FREE((*old)->divstrings);
    FREE((*old)->divpointers);
    FREE((*old)->cum_nnodes);
    FREE((*old)->nnodes);
    FREE((*old)->cum_nintervals);
    FREE((*old)->nintervals);

    FREE(*old);

  }

  return;
}


#ifdef HAVE_FSEEKO

static void
move_relative (FILE *fp, off_t offset) {
  if (fseeko(fp,offset,SEEK_CUR) < 0) {
    fprintf(stderr,"Error in move_relative, seek\n");
    abort();
  }
  return;
}

#else

static void
move_relative (FILE *fp, long int offset) {
  if (fseek(fp,offset,SEEK_CUR) < 0) {
    fprintf(stderr,"Error in move_relative, seek\n");
    abort();
  }
  return;
}

#endif

static off_t
skip_trees (off_t offset, off_t filesize, FILE *fp, char *filename,
	    int skip_ndivs, int skip_nintervals, int skip_nnodes) {

  off_t skipsize;

  /* 4 is for alphas, betas, sigmas, and omegas */
  skipsize = (skip_nintervals + skip_ndivs) * 4 * sizeof(int);
  skipsize += skip_nnodes * sizeof(struct FNode_T);

  if ((offset += skipsize) > filesize) {
    fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after skip_trees %ld, filesize %ld)\n",
	    filename,offset,filesize);
    exit(9);
  } else {
    move_relative(fp,skipsize);
  }

  return offset;
}



static off_t
read_tree (off_t offset, off_t filesize, FILE *fp, char *filename, T new, int divno) {
  size_t items_read;
  int i;

  if (new->version < 2) {
#if 0
    /* Computing only if needed */
    compute_flanking(new);
#else
    new->alphas[divno] = new->betas[divno] = (int *) NULL;
#endif

  } else {
    if ((offset += sizeof(int)*(new->nintervals[divno]+1)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after alphas %ld, filesize %ld)\n",
	      filename,offset,filesize);
      exit(9);
    } else {
      new->alphas[divno] = (int *) CALLOC(new->nintervals[divno]+1,sizeof(int));
      if ((items_read = FREAD_INTS(new->alphas[divno],new->nintervals[divno]+1,fp)) != new->nintervals[divno] + 1) {
	fprintf(stderr,"IIT file %s appears to be truncated.  items_read = %lu\n",filename,items_read);
	exit(9);
      }
    }

    if ((offset += sizeof(int)*(new->nintervals[divno]+1)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after betas %ld, filesize %ld\n",
	      filename,offset,filesize);
      exit(9);
    } else {
      new->betas[divno] = (int *) CALLOC(new->nintervals[divno]+1,sizeof(int));
      if ((items_read = FREAD_INTS(new->betas[divno],new->nintervals[divno]+1,fp)) != new->nintervals[divno] + 1) {
	fprintf(stderr,"IIT file %s appears to be truncated.  items_read = %lu\n",filename,items_read);
	exit(9);
      }
#if 0
      debug(
	    printf("betas[%d]:",divno);
	    for (i = 0; i < new->nintervals[divno]+1; i++) {
	      printf(" %d",new->betas[divno][i]);
	    }
	    printf("\n");
	    );
#endif
    }
  }

  if ((offset += sizeof(int)*(new->nintervals[divno]+1)) > filesize) {
    fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after sigmas %ld, filesize %ld\n",
	    filename,offset,filesize);
    exit(9);
  } else {
    new->sigmas[divno] = (int *) CALLOC(new->nintervals[divno]+1,sizeof(int));
    if ((items_read = FREAD_INTS(new->sigmas[divno],new->nintervals[divno]+1,fp)) != new->nintervals[divno] + 1) {
      fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
      exit(9);
    }
#if 0
    debug(
	  printf("sigmas[%d]:",divno);
	  for (i = 0; i < new->nintervals[divno]+1; i++) {
	    printf(" %d",new->sigmas[divno][i]);
	  }
	  printf("\n");
	  );
#endif
  }

  if ((offset += sizeof(int)*(new->nintervals[divno]+1)) > filesize) {
    fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after omegas %ld, filesize %ld\n",
	    filename,offset,filesize);
    exit(9);
  } else {
    new->omegas[divno] = (int *) CALLOC(new->nintervals[divno]+1,sizeof(int));
    if ((items_read = FREAD_INTS(new->omegas[divno],new->nintervals[divno]+1,fp)) != new->nintervals[divno] + 1) {
      fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
      exit(9);
    }
#if 0
    debug(
	  printf("omegas[%d]:",divno);
	  for (i = 0; i < new->nintervals[divno]+1; i++) {
	    printf(" %d",new->omegas[divno][i]);
	  }
	  printf("\n");
	  );
#endif
  }

  debug(printf("nnodes[%d]: %d\n",divno,new->nnodes[divno]));
  if (new->nnodes[divno] == 0) {
    new->nodes[divno] = (struct FNode_T *) NULL;
  } else {
    new->nodes[divno] = (struct FNode_T *) CALLOC(new->nnodes[divno],sizeof(struct FNode_T));
#ifdef WORDS_BIGENDIAN
    for (i = 0; i < new->nnodes[divno]; i++) {
      Bigendian_fread_uint(&(new->nodes[divno][i].value),fp);
      Bigendian_fread_int(&(new->nodes[divno][i].a),fp);
      Bigendian_fread_int(&(new->nodes[divno][i].b),fp);
      Bigendian_fread_int(&(new->nodes[divno][i].leftindex),fp);
      Bigendian_fread_int(&(new->nodes[divno][i].rightindex),fp);
    }
    offset += (sizeof(unsigned int)+sizeof(int)+sizeof(int)+sizeof(int)+sizeof(int))*new->nnodes[divno];
#else
    if (sizeof(struct FNode_T) == sizeof(unsigned int)+sizeof(int)+sizeof(int)+sizeof(int)+sizeof(int)) {
      offset += sizeof(struct FNode_T)*fread(new->nodes[divno],sizeof(struct FNode_T),new->nnodes[divno],fp);
    } else {
      for (i = 0; i < new->nnodes[divno]; i++) {
	fread(&(new->nodes[divno][i].value),sizeof(unsigned int),1,fp);
	fread(&(new->nodes[divno][i].a),sizeof(int),1,fp);
	fread(&(new->nodes[divno][i].b),sizeof(int),1,fp);
	fread(&(new->nodes[divno][i].leftindex),sizeof(int),1,fp);
	fread(&(new->nodes[divno][i].rightindex),sizeof(int),1,fp);
      }
      offset += (sizeof(unsigned int)+sizeof(int)+sizeof(int)+sizeof(int)+sizeof(int))*new->nnodes[divno];
    }
#endif
    if (offset > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after nodes %ld, filesize %ld\n",
	      filename,offset,filesize);
      exit(9);
    }

#if 1
    debug(
	  for (i = 0; i < new->nnodes[divno]; i++) {
	    printf("Read node %d %d %d\n",new->nodes[divno][i].value,new->nodes[divno][i].a,new->nodes[divno][i].b);
	  }
	  );
#endif

  }
  debug(printf("\n"));

  return offset;
}


static off_t
skip_intervals (int *skip_nintervals, off_t offset, off_t filesize, FILE *fp, char *filename, T new, 
		int divstart, int divend) {
  int divno;
  off_t skipsize = 0;

  *skip_nintervals = 0;
  for (divno = divstart; divno <= divend; divno++) {
    *skip_nintervals += new->nintervals[divno];
  }
  if (new->version >= 2) {
    skipsize += (sizeof(unsigned int)+sizeof(unsigned int)+sizeof(int)+sizeof(int))*(*skip_nintervals);
  } else {
    skipsize += (sizeof(unsigned int)+sizeof(unsigned int)+sizeof(int))*(*skip_nintervals);
  }

  if ((offset += skipsize) > filesize) {
    fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after skip_intervals %ld, filesize %ld\n",
	    filename,offset,filesize);
    exit(9);
  } else {
    move_relative(fp,skipsize);
  }

  return offset;
}


static off_t
read_intervals (off_t offset, off_t filesize, FILE *fp, char *filename, T new, int divno) {
  int i;

#ifdef WORDS_BIGENDIAN
  for (i = 0; i < new->nintervals[divno]; i++) {
    Bigendian_fread_uint(&(new->intervals[divno][i].low),fp);
    Bigendian_fread_uint(&(new->intervals[divno][i].high),fp);
    if (new->version >= 2) {
      Bigendian_fread_int(&(new->intervals[divno][i].sign),fp);
    } else {
      new->intervals[divno][i].sign = +1;
    }
    Bigendian_fread_int(&(new->intervals[divno][i].type),fp);
  }
  if (new->version >= 2) {
    offset += (sizeof(unsigned int)+sizeof(unsigned int)+sizeof(int)+sizeof(int))*new->nintervals[divno];
  } else {
    offset += (sizeof(unsigned int)+sizeof(unsigned int)+sizeof(int))*new->nintervals[divno];
  }
#else
  if (new->version >= 2 && sizeof(struct Interval_T) == sizeof(unsigned int)+sizeof(unsigned int)+sizeof(int)+sizeof(int)) {
    offset += sizeof(struct Interval_T)*fread(new->intervals[divno],sizeof(struct Interval_T),new->nintervals[divno],fp);
  } else if (new->version <= 1 && sizeof(struct Interval_T) == sizeof(unsigned int)+sizeof(unsigned int)+sizeof(int)) {
    offset += sizeof(struct Interval_T)*fread(new->intervals[divno],sizeof(struct Interval_T),new->nintervals[divno],fp);
  } else {
    for (i = 0; i < new->nintervals[divno]; i++) {
      fread(&(new->intervals[divno][i].low),sizeof(unsigned int),1,fp);
      fread(&(new->intervals[divno][i].high),sizeof(unsigned int),1,fp);
      if (new->version >= 2) {
	fread(&(new->intervals[divno][i].sign),sizeof(int),1,fp);
      } else {
	new->intervals[divno][i].sign = +1;
      }
      fread(&(new->intervals[divno][i].type),sizeof(int),1,fp);
    }
    if (new->version >= 2) {
      offset += (sizeof(unsigned int)+sizeof(unsigned int)+sizeof(int)+sizeof(int))*new->nintervals[divno];
    } else {
      offset += (sizeof(unsigned int)+sizeof(unsigned int)+sizeof(int))*new->nintervals[divno];
    }
  }
#endif
  if (offset > filesize) {
    fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after intervals %ld, filesize %ld\n",
	    filename,offset,filesize);
    exit(9);
  }

  return offset;
}

static void
read_words (off_t offset, off_t filesize, FILE *fp, char *filename, T new) {
  off_t stringlen;
#ifdef HAVE_64_BIT
  UINT8 length8;
#endif
  unsigned int length;
#ifdef DEBUG
  int i;
#endif

  new->typepointers = (unsigned int *) CALLOC(new->ntypes+1,sizeof(unsigned int));
  offset += sizeof(int)*FREAD_UINTS(new->typepointers,new->ntypes+1,fp);
  debug(
	printf("typepointers:");
	for (i = 0; i < new->ntypes+1; i++) {
	  printf(" %u",new->typepointers[i]);
	}
	printf("\n");
	);

  stringlen = new->typepointers[new->ntypes];
  new->typestrings = (char *) CALLOC(stringlen,sizeof(char));
  offset += sizeof(char)*FREAD_CHARS(new->typestrings,stringlen,fp);
  debug(
	printf("typestrings:\n");
	for (i = 0; i < stringlen; i++) {
	  printf("%c",new->typestrings[i]);
	}
	printf("\n");
	);

  new->fieldpointers = (unsigned int *) CALLOC(new->nfields+1,sizeof(unsigned int));
  if (new->version < 2) {
    new->fieldpointers[0] = '\0';
  } else {
    offset += sizeof(int)*FREAD_UINTS(new->fieldpointers,new->nfields+1,fp);
  }
  stringlen = new->fieldpointers[new->nfields];
  if (stringlen == 0) {
    new->fieldstrings = (char *) NULL;
  } else {
    new->fieldstrings = (char *) CALLOC(stringlen,sizeof(char));
    offset += sizeof(char)*FREAD_CHARS(new->fieldstrings,stringlen,fp);
  }
  debug(
	printf("fieldstrings:\n");
	for (i = 0; i < stringlen; i++) {
	  printf("%c",new->fieldstrings[i]);
	}
	printf("\n");
	);

  debug1(printf("Starting read of labelorder offset/length\n"));
  new->labelorder_offset = offset;
  new->labelorder_length = (size_t) (new->total_nintervals*sizeof(int));
  /* fprintf(stderr,"Doing a move_relative for labelorder_length %lu\n",new->labelorder_length); */
  move_relative(fp,new->labelorder_length);
  offset += new->labelorder_length;

  debug1(printf("Starting read of labelpointer offset/length\n"));
  new->labelpointers_offset = offset;
#ifdef HAVE_64_BIT
  if (new->version >= 4) {
    new->labelpointers_length = (size_t) ((new->total_nintervals+1)*sizeof(UINT8));
    move_relative(fp,new->total_nintervals * sizeof(UINT8));
    FREAD_UINT8(&length8,fp);
    new->label_length = (size_t) length8;
  } else {
    new->labelpointers_length = (size_t) ((new->total_nintervals+1)*sizeof(unsigned int));
    /* fprintf(stderr,"Doing a move_relative for labelpointer %lu\n",new->total_nintervals * sizeof(unsigned int)); */
    move_relative(fp,new->total_nintervals * sizeof(unsigned int));
    FREAD_UINT(&length,fp);
    new->label_length = (size_t) length;
  }
#else
  new->labelpointers_length = (size_t) ((new->total_nintervals+1)*sizeof(unsigned int));
  move_relative(fp,new->total_nintervals * sizeof(unsigned int));
  FREAD_UINT(&length,fp);
  new->label_length = (size_t) length;
#endif
  offset += new->labelpointers_length;

  debug1(printf("Starting read of label offset/length\n"));
  new->label_offset = offset;
  /* new->label_length computed above */
  /* fprintf(stderr,"Doing a move_relative for label_length %lu\n",new->label_length); */
  move_relative(fp,new->label_length);
  offset += new->label_length;

  debug1(printf("Starting read of annotpointers offset/length\n"));
  new->annotpointers_offset = offset;
#ifdef HAVE_64_BIT
  if (new->version >= 4) {
    new->annotpointers_length = (size_t) ((new->total_nintervals+1)*sizeof(UINT8));
  } else {
    new->annotpointers_length = (size_t) ((new->total_nintervals+1)*sizeof(unsigned int));
  }
#else
  new->annotpointers_length = (size_t) ((new->total_nintervals+1)*sizeof(unsigned int));
#endif
  offset += new->annotpointers_length;

  new->annot_offset = offset;

#ifdef BAD_32BIT
  /* This fails if length > 4 GB */
  move_relative(fp,new->total_nintervals * sizeof(unsigned int));
  FREAD_UINT(&length,fp);
  new->annot_length = (size_t) length;
  fprintf(stderr,"Incorrect length: %u\n",length);
#else
  new->annot_length = filesize - new->annot_offset;
  /* fprintf(stderr,"annot_length: %lu\n",new->annot_length); */
#endif

#if 0
  /* To do this check, we need to get stringlen for annotation similarly to that for labels */
  last_offset = offset + sizeof(char)*stringlen;
  if (last_offset != filesize) {
    fprintf(stderr,"Problem with last_offset (%ld) not equal to filesize = (%ld)\n",last_offset,filesize);
    exit(9);
  }
#endif

  return;
}

static void
read_words_debug (off_t offset, off_t filesize, FILE *fp, char *filename, T new) {
  off_t stringlen;
#ifdef HAVE_64_BIT
  UINT8 length8;
#endif
  unsigned int length;
  int i;
#if 0
  off_t last_offset;
#endif

  new->typepointers = (unsigned int *) CALLOC(new->ntypes+1,sizeof(unsigned int));
  offset += sizeof(int)*FREAD_UINTS(new->typepointers,new->ntypes+1,fp);
  printf("typepointers:");
  for (i = 0; i < new->ntypes+1; i++) {
    printf(" %u",new->typepointers[i]);
  }
  printf("\n");

  stringlen = new->typepointers[new->ntypes];
  new->typestrings = (char *) CALLOC(stringlen,sizeof(char));
  offset += sizeof(char)*FREAD_CHARS(new->typestrings,stringlen,fp);
  printf("typestrings:\n");
  for (i = 0; i < stringlen; i++) {
    printf("%c",new->typestrings[i]);
  }
  printf("\n");

  new->fieldpointers = (unsigned int *) CALLOC(new->nfields+1,sizeof(unsigned int));
  if (new->version < 2) {
    new->fieldpointers[0] = '\0';
  } else {
    offset += sizeof(int)*FREAD_UINTS(new->fieldpointers,new->nfields+1,fp);
  }
  stringlen = new->fieldpointers[new->nfields];
  if (stringlen == 0) {
    new->fieldstrings = (char *) NULL;
  } else {
    new->fieldstrings = (char *) CALLOC(stringlen,sizeof(char));
    offset += sizeof(char)*FREAD_CHARS(new->fieldstrings,stringlen,fp);
  }
  printf("fieldstrings:\n");
  for (i = 0; i < stringlen; i++) {
    printf("%c",new->fieldstrings[i]);
  }
  printf("\n");

  debug1(printf("Starting read of labelorder offset/length\n"));
  new->labelorder_offset = offset;
  new->labelorder_length = (size_t) (new->total_nintervals*sizeof(int));
  move_relative(fp,new->labelorder_length);
  offset += new->labelorder_length;

  debug1(printf("Starting read of labelpointers offset/length\n"));
  new->labelpointers_offset = offset;
#ifdef HAVE_64_BIT
  if (new->version >= 4) {
    new->labelpointers_length = (size_t) ((new->total_nintervals+1)*sizeof(UINT8));
    move_relative(fp,new->total_nintervals * sizeof(UINT8));
    FREAD_UINT8(&length8,fp);
    new->label_length = (size_t) length8;
  } else {
    new->labelpointers_length = (size_t) ((new->total_nintervals+1)*sizeof(unsigned int));
    move_relative(fp,new->total_nintervals * sizeof(unsigned int));
    FREAD_UINT(&length,fp);
    new->label_length = (size_t) length;
  }
#else
  new->labelpointers_length = (size_t) ((new->total_nintervals+1)*sizeof(unsigned int));
  move_relative(fp,new->total_nintervals * sizeof(unsigned int));
  FREAD_UINT(&length,fp);
  new->label_length = (size_t) length;
#endif
  offset += new->labelpointers_length;

  fprintf(stderr,"label_length: %lu\n",new->label_length);
  debug1(printf("Starting read of label offset/length\n"));
  new->label_offset = offset;
  /* new->label_length computed above */
  move_relative(fp,new->label_length);
  offset += new->label_length;

  debug1(printf("Starting read of annotpointers offset/length\n"));
  new->annotpointers_offset = offset;
#ifdef HAVE_64_BIT
  if (new->version >= 4) {
    new->annotpointers_length = (size_t) ((new->total_nintervals+1)*sizeof(UINT8));
  } else {
    new->annotpointers_length = (size_t) ((new->total_nintervals+1)*sizeof(unsigned int));
  }
#else
  new->annotpointers_length = (size_t) ((new->total_nintervals+1)*sizeof(unsigned int));
#endif
  offset += new->annotpointers_length;

  new->annot_offset = offset;

#ifdef BAD_32BIT
  /* This fails if length > 4 GB */
  move_relative(fp,new->total_nintervals * sizeof(unsigned int));
  FREAD_UINT(&length,fp);
  new->annot_length = (size_t) length;
  fprintf(stderr,"Incorrect length: %u\n",length);
#else
  new->annot_length = filesize - new->annot_offset;
  fprintf(stderr,"annot_length: %lu\n",new->annot_length);
#endif

#if 0
  /* To do this check, we need to get stringlen for annotation similarly to that for labels */
  last_offset = offset + sizeof(char)*stringlen;
  if (last_offset != filesize) {
    fprintf(stderr,"Problem with last_offset (%ld) not equal to filesize = (%ld)\n",last_offset,filesize);
    exit(9);
  }
#endif

  return;
}

/* This function only assigns pointers.  Subsequent accesses to
   memory, other than char *, still need to be read correctly
   by bigendian machines */
#ifdef HAVE_MMAP
static bool
mmap_annotations (char *filename, T new, bool readonlyp) {
  int remainder;

  if (readonlyp == true) {
    if ((new->fd = open(filename,O_RDONLY,0764)) < 0) {
      fprintf(stderr,"Error: can't open file %s with open for reading\n",filename);
      exit(9);
    }

    new->labelorder_mmap = Access_mmap_offset(&remainder,new->fd,new->labelorder_offset,new->labelorder_length,
					      sizeof(char),/*randomp*/true);
    debug(fprintf(stderr,"labelorder_mmap is %p\n",new->labelorder_mmap));
    new->labelorder = (int *) &(new->labelorder_mmap[remainder]);
    new->labelorder_length += (size_t) remainder;

    new->labelpointers_mmap = Access_mmap_offset(&remainder,new->fd,new->labelpointers_offset,new->labelpointers_length,
						 sizeof(char),/*randomp*/true);
    debug(fprintf(stderr,"labelpointers_mmap is %p\n",new->labelpointers_mmap));
#ifdef HAVE_64_BIT
    if (new->version >= 4) {
      new->labelpointers8 = (UINT8 *) &(new->labelpointers_mmap[remainder]);
      new->labelpointers = (unsigned int *) NULL;
    } else {
      new->labelpointers8 = (UINT8 *) NULL;
      new->labelpointers = (unsigned int *) &(new->labelpointers_mmap[remainder]);
    }
#else
    new->labelpointers = (unsigned int *) &(new->labelpointers_mmap[remainder]);
#endif
    new->labelpointers_length += (size_t) remainder;

    new->label_mmap = Access_mmap_offset(&remainder,new->fd,new->label_offset,new->label_length,
					  sizeof(char),/*randomp*/true);
    debug(fprintf(stderr,"labels_mmap is %p\n",new->label_mmap));
    new->labels = (char *) &(new->label_mmap[remainder]);
    new->label_length += (size_t) remainder;

    new->annotpointers_mmap = Access_mmap_offset(&remainder,new->fd,new->annotpointers_offset,new->annotpointers_length,
						 sizeof(char),/*randomp*/true);
    debug(fprintf(stderr,"annotpointers_mmap is %p\n",new->annotpointers_mmap));
#ifdef HAVE_64_BIT
    if (new->version >= 4) {
      new->annotpointers8 = (UINT8 *) &(new->annotpointers_mmap[remainder]);
      new->annotpointers = (unsigned int *) NULL;
    } else {
      new->annotpointers8 = (UINT8 *) NULL;
      new->annotpointers = (unsigned int *) &(new->annotpointers_mmap[remainder]);
    }
#else
    new->annotpointers = (unsigned int *) &(new->annotpointers_mmap[remainder]);
#endif
    new->annotpointers_length += (size_t) remainder;

    new->annot_mmap = Access_mmap_offset(&remainder,new->fd,new->annot_offset,new->annot_length,
					 sizeof(char),/*randomp*/true);
    debug(fprintf(stderr,"annots_mmap is %p\n",new->annot_mmap));
    new->annotations = (char *) &(new->annot_mmap[remainder]);
    new->annot_length += (size_t) remainder;

  } else {
    if ((new->fd = open(filename,O_RDWR,0764)) < 0) {
      fprintf(stderr,"Error: can't open file %s with open for reading/writing\n",filename);
      exit(9);
    }

    new->labelorder_mmap = Access_mmap_offset_rw(&remainder,new->fd,new->labelorder_offset,new->labelorder_length,
						 sizeof(char),/*randomp*/true);
    new->labelorder = (int *) &(new->labelorder_mmap[remainder]);
    new->labelorder_length += (size_t) remainder;

    new->labelpointers_mmap = Access_mmap_offset_rw(&remainder,new->fd,new->labelpointers_offset,new->labelpointers_length,
						    sizeof(char),/*randomp*/true);
#ifdef HAVE_64_BIT
    if (new->version >= 4) {
      new->labelpointers8 = (UINT8 *) &(new->labelpointers_mmap[remainder]);
      new->labelpointers = (unsigned int *) NULL;
    } else {
      new->labelpointers8 = (UINT8 *) NULL;
      new->labelpointers = (unsigned int *) &(new->labelpointers_mmap[remainder]);
    }
#else
    new->labelpointers = (unsigned int *) &(new->labelpointers_mmap[remainder]);
#endif
    new->labelpointers_length += (size_t) remainder;

    new->label_mmap = Access_mmap_offset_rw(&remainder,new->fd,new->label_offset,new->label_length,
					    sizeof(char),/*randomp*/true);
    new->labels = (char *) &(new->label_mmap[remainder]);
    new->label_length += (size_t) remainder;

    new->annotpointers_mmap = Access_mmap_offset_rw(&remainder,new->fd,new->annotpointers_offset,new->annotpointers_length,
						    sizeof(char),/*randomp*/true);
#ifdef HAVE_64_BIT
    if (new->version >= 4) {
      new->annotpointers8 = (UINT8 *) &(new->annotpointers_mmap[remainder]);
      new->annotpointers = (unsigned int *) NULL;
    } else {
      new->annotpointers8 = (UINT8 *) NULL;
      new->annotpointers = (unsigned int *) &(new->annotpointers_mmap[remainder]);
    }
#else
    new->annotpointers = (unsigned int *) &(new->annotpointers_mmap[remainder]);
#endif
    new->annotpointers_length += (size_t) remainder;

    new->annot_mmap = Access_mmap_offset_rw(&remainder,new->fd,new->annot_offset,new->annot_length,
					    sizeof(char),/*randomp*/true);
    new->annotations = (char *) &(new->annot_mmap[remainder]);
    new->annot_length += (size_t) remainder;
  }

#ifdef HAVE_64_BIT
  if (new->version >= 4) {
    if (new->labelorder == NULL || new->labelpointers8 == NULL || new->labels == NULL || new->annotpointers8 == NULL || new->annotations == NULL) {
      fprintf(stderr,"Memory mapping failed in reading IIT file %s.  Using slow file IO instead.\n",filename);
      return false;
    } else {
      return true;
    }
  } else {
    if (new->labelorder == NULL || new->labelpointers == NULL || new->labels == NULL || new->annotpointers == NULL || new->annotations == NULL) {
      fprintf(stderr,"Memory mapping failed in reading IIT file %s.  Using slow file IO instead.\n",filename);
      return false;
    } else {
      return true;
    }
  }
#else
  if (new->labelorder == NULL || new->labelpointers == NULL || new->labels == NULL || new->annotpointers == NULL || new->annotations == NULL) {
    fprintf(stderr,"Memory mapping failed in reading IIT file %s.  Using slow file IO instead.\n",filename);
    return false;
  } else {
    return true;
  }
#endif

}
#endif


/* Used if access is FILEIO.  Subsequent accesses by bigendian
   machines to anything but (char *) will still need to convert. */
static void
read_annotations (int fd, T new) {

  file_move_absolute(new->fd,new->labelorder_offset,sizeof(int),/*n*/0);
  new->labelorder = (int *) CALLOC(new->total_nintervals,sizeof(int));
  read(new->fd,new->labelorder,new->total_nintervals*sizeof(int));

#ifdef HAVE_64_BIT
  if (new->version >= 4) {
    file_move_absolute(new->fd,new->labelpointers_offset,sizeof(UINT8),/*n*/0);
    new->labelpointers8 = (UINT8 *) CALLOC(new->total_nintervals+1,sizeof(UINT8));
    read(new->fd,new->labelpointers8,(new->total_nintervals+1)*sizeof(UINT8));
    new->labelpointers = (unsigned int *) NULL;
  } else {
    file_move_absolute(new->fd,new->labelpointers_offset,sizeof(unsigned int),/*n*/0);
    new->labelpointers = (unsigned int *) CALLOC(new->total_nintervals+1,sizeof(unsigned int));
    read(new->fd,new->labelpointers,(new->total_nintervals+1)*sizeof(unsigned int));
    new->labelpointers8 = (UINT8 *) NULL;
  }
#else
  file_move_absolute(new->fd,new->labelpointers_offset,sizeof(unsigned int),/*n*/0);
  new->labelpointers = (unsigned int *) CALLOC(new->total_nintervals+1,sizeof(unsigned int));
  read(new->fd,new->labelpointers,(new->total_nintervals+1)*sizeof(unsigned int));
#endif

  file_move_absolute(new->fd,new->label_offset,sizeof(char),/*n*/0);
  new->labels = (char *) CALLOC(new->label_length,sizeof(char));
  read(new->fd,new->labels,new->label_length*sizeof(char));

#ifdef HAVE_64_BIT
  if (new->version >= 4) {
    file_move_absolute(new->fd,new->annotpointers_offset,sizeof(UINT8),/*n*/0);
    new->annotpointers8 = (UINT8 *) CALLOC(new->total_nintervals+1,sizeof(UINT8));
    read(new->fd,new->annotpointers,(new->total_nintervals+1)*sizeof(UINT8));
    new->annotpointers = (unsigned int *) NULL;
  } else {
    file_move_absolute(new->fd,new->annotpointers_offset,sizeof(unsigned int),/*n*/0);
    new->annotpointers = (unsigned int *) CALLOC(new->total_nintervals+1,sizeof(unsigned int));
    read(new->fd,new->annotpointers,(new->total_nintervals+1)*sizeof(unsigned int));
    new->annotpointers8 = (UINT8 *) NULL;
  }
#else
  file_move_absolute(new->fd,new->annotpointers_offset,sizeof(unsigned int),/*n*/0);
  new->annotpointers = (unsigned int *) CALLOC(new->total_nintervals+1,sizeof(unsigned int));
  read(new->fd,new->annotpointers,(new->total_nintervals+1)*sizeof(unsigned int));
#endif

  file_move_absolute(new->fd,new->annot_offset,sizeof(char),/*n*/0);
  new->annotations = (char *) CALLOC(new->annot_length,sizeof(char));
  read(new->fd,new->annotations,new->annot_length*sizeof(char));

  return;
}


int
IIT_read_divint (char *filename, char *divstring, bool add_iit_p) {
  char *newfile = NULL;
  FILE *fp;
  int version;
  off_t offset, filesize, skipsize;
  int total_nintervals, ntypes, nfields, divsort;

  int i, ndivs;
  unsigned int *divpointers, stringlen, start;
  char *divstrings;

  if ((fp = FOPEN_READ_BINARY(filename)) == NULL) {
    if (add_iit_p == false) {
      /* fprintf(stderr,"Cannot open IIT file %s\n",filename); */
      return -1;
    } else {
      newfile = (char *) CALLOC(strlen(filename)+strlen(".iit")+1,sizeof(char));
      sprintf(newfile,"%s.iit",filename);
      if ((fp = FOPEN_READ_BINARY(newfile)) == NULL) {
	/* fprintf(stderr,"Cannot open IIT file %s or %s\n",filename,newfile); */
	return -1;
      } else {
	filename = newfile;
      }
    }
  }

  version = 1;

  filesize = Access_filesize(filename);
  offset = 0U;

  if (FREAD_INT(&total_nintervals,fp) < 1) {
    fprintf(stderr,"IIT file %s appears to be empty\n",filename);
    return -1;
  } else if ((offset += sizeof(int)) > filesize) {
    fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after first byte %ld, filesize %ld)\n",
	    filename,offset,filesize);
    return -1;
  }

  if (total_nintervals == 0) {
    /* New format */
    FREAD_INT(&version,fp);
    if (version > IIT_LATEST_VERSION) {
      fprintf(stderr,"This file is version %d, but this software can only read up to version %d\n",
	      version,IIT_LATEST_VERSION);
      return -1;
    } else if ((offset += sizeof(int)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after version %ld, filesize %ld)\n",
	      filename,offset,filesize);
      return -1;
    }
    if (FREAD_INT(&total_nintervals,fp) < 1) {
      fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
      return -1;
    } else if ((offset += sizeof(int)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after nintervals %ld, filesize %ld)\n",
	      filename,offset,filesize);
      return -1;
    }
  }
  if (total_nintervals < 0) {
    fprintf(stderr,"IIT file %s appears to have a negative number of intervals\n",filename);
    return -1;
  }

  debug(printf("version: %d\n",version));
  debug(printf("total_nintervals: %d\n",total_nintervals));


  if (FREAD_INT(&ntypes,fp) < 1) {
    fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
    return -1;
  } else if (ntypes < 0) {
    fprintf(stderr,"IIT file %s appears to have a negative number of types\n",filename);
    return -1;
  } else if ((offset += sizeof(int)) > filesize) {
    fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after ntypes %ld, filesize %ld)\n",
	    filename,offset,filesize);
    return -1;
  }
  debug(printf("ntypes: %d\n",ntypes));


  if (version < 2) {
    nfields = 0;
  } else {
    if (FREAD_INT(&nfields,fp) < 1) {
      fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
      return -1;
    } else if (nfields < 0) {
      fprintf(stderr,"IIT file %s appears to have a negative number of fields\n",filename);
      return -1;
    } else if ((offset += sizeof(int)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after nfields %ld, filesize %ld)\n",
	      filename,offset,filesize);
      return -1;
    }
  }
  debug(printf("nfields: %d\n",nfields));


  if (version <= 2) {
    return -1;

  } else {

    if (FREAD_INT(&ndivs,fp) < 1) {
      fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
      return -1;
    } else if (ndivs < 0) {
      fprintf(stderr,"IIT file %s appears to have a negative number of divs\n",filename);
      return -1;
    } else if ((offset += sizeof(int)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after ndivs %ld, filesize %ld)\n",
	      filename,offset,filesize);
      return -1;
    }
    debug(printf("ndivs: %d\n",ndivs));

    /* Skip nintervals */
    offset += skipsize = sizeof(int)*ndivs;
    move_relative(fp,skipsize);

    /* Skip cum_nintervals */
    offset += skipsize = sizeof(int)*(ndivs+1);
    move_relative(fp,skipsize);

    /* Skip nnodes */
    offset += skipsize = sizeof(int)*ndivs;
    move_relative(fp,skipsize);

    /* Skip cum_nnodes */
    offset += skipsize = sizeof(int)*(ndivs+1);
    move_relative(fp,skipsize);

    if (FREAD_INT(&divsort,fp) < 1) {
      fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
      return -1;
    } else if (divsort < 0) {
      fprintf(stderr,"IIT file %s appears to have a negative value for divsort\n",filename);
      return -1;
    } else if ((offset += sizeof(int)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after divsort %ld, filesize %ld)\n",
	      filename,offset,filesize);
      return -1;
    }
    debug(printf("divsort: %d\n",divsort));

    divpointers = (unsigned int *) CALLOC(ndivs+1,sizeof(unsigned int));
    offset += sizeof(int)*FREAD_UINTS(divpointers,ndivs+1,fp);
    debug(
	  printf("divpointers:");
	  for (i = 0; i < ndivs+1; i++) {
	    printf(" %u",divpointers[i]);
	  }
	  printf("\n");
	  );

    stringlen = divpointers[ndivs];
    divstrings = (char *) CALLOC(stringlen,sizeof(char));
    offset += sizeof(char)*FREAD_CHARS(divstrings,stringlen,fp);
    debug(
	  printf("divstrings:\n");
	  for (i = 0; i < stringlen; i++) {
	    if (divstrings[i] == '\0') {
	      printf("\n");
	    } else {
	      printf("%c",divstrings[i]);
	    }
	  }
	  printf("(end of divstrings)\n");
	  );

    i = 0;
    while (i < ndivs) {
      start = divpointers[i];
      if (!strcmp(divstring,&(divstrings[start]))) {
	fclose(fp);
	FREE(divstrings);
	FREE(divpointers);
	if (newfile != NULL) {
	  FREE(newfile);
	}
	return i;
      }
      i++;
    }
    
    fclose(fp);
    FREE(divstrings);
    FREE(divpointers);
    if (newfile != NULL) {
      FREE(newfile);
    }
    return -1;
  }
}



T
IIT_read (char *filename, char *name, bool readonlyp, Divread_T divread, char *divstring,
	  bool add_iit_p, bool labels_read_p) {
  T new;
  FILE *fp;
  char *newfile = NULL;
  off_t offset = 0, filesize, stringlen;
  int skip_nintervals, desired_divno, divno;
#ifdef DEBUG
  int i;
  Interval_T interval;
#endif

  /* printf("Reading IIT file %s\n",filename); */
  if ((fp = FOPEN_READ_BINARY(filename)) == NULL) {
    if (add_iit_p == false) {
      /* fprintf(stderr,"Cannot open IIT file %s\n",filename); */
      return NULL;
    } else {
      newfile = (char *) CALLOC(strlen(filename)+strlen(".iit")+1,sizeof(char));
      sprintf(newfile,"%s.iit",filename);
      if ((fp = FOPEN_READ_BINARY(newfile)) == NULL) {
	/* fprintf(stderr,"Cannot open IIT file %s or %s\n",filename,newfile); */
	return NULL;
      } else {
	filename = newfile;
      }
    }
  }

  new = (T) MALLOC(sizeof(*new));
  new->version = 1;

  filesize = Access_filesize(filename);

  if (name == NULL) {
    new->name = NULL;
  } else {
    new->name = (char *) CALLOC(strlen(name)+1,sizeof(char));
    strcpy(new->name,name);
  }

  if (FREAD_INT(&new->total_nintervals,fp) < 1) {
    fprintf(stderr,"IIT file %s appears to be empty\n",filename);
    return NULL;
  } else if ((offset += sizeof(int)) > filesize) {
    fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after first byte %ld, filesize %ld)\n",
	    filename,offset,filesize);
    return NULL;
  }

  if (new->total_nintervals == 0) {
    /* New format */
    FREAD_INT(&new->version,fp);
    if (new->version > IIT_LATEST_VERSION) {
      fprintf(stderr,"This file is version %d, but this software can only read up to version %d\n",
	      new->version,IIT_LATEST_VERSION);
      return NULL;
    } else if ((offset += sizeof(int)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after version %ld, filesize %ld)\n",
	      filename,offset,filesize);
      return NULL;
    }
    if (FREAD_INT(&new->total_nintervals,fp) < 1) {
      fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
      return NULL;
    } else if ((offset += sizeof(int)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after nintervals %ld, filesize %ld)\n",
	      filename,offset,filesize);
      return NULL;
    }
  }
  if (new->total_nintervals < 0) {
    fprintf(stderr,"IIT file %s appears to have a negative number of intervals\n",filename);
    return NULL;
  }

  debug(printf("version: %d\n",new->version));
  debug(printf("total_nintervals: %d\n",new->total_nintervals));


  if (FREAD_INT(&new->ntypes,fp) < 1) {
    fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
    return NULL;
  } else if (new->ntypes < 0) {
    fprintf(stderr,"IIT file %s appears to have a negative number of types\n",filename);
    return NULL;
  } else if ((offset += sizeof(int)) > filesize) {
    fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after ntypes %ld, filesize %ld)\n",
	    filename,offset,filesize);
    return NULL;
  }
  debug(printf("ntypes: %d\n",new->ntypes));


  if (new->version < 2) {
    new->nfields = 0;
  } else {
    if (FREAD_INT(&new->nfields,fp) < 1) {
      fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
      return NULL;
    } else if (new->nfields < 0) {
      fprintf(stderr,"IIT file %s appears to have a negative number of fields\n",filename);
      return NULL;
    } else if ((offset += sizeof(int)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after nfields %ld, filesize %ld)\n",
	      filename,offset,filesize);
      return NULL;
    }
  }
  debug(printf("nfields: %d\n",new->nfields));


  if (new->version <= 2) {
    new->ndivs = 1;

    new->nintervals = (int *) CALLOC(new->ndivs,sizeof(int));
    new->nintervals[0] = new->total_nintervals;
    new->cum_nintervals = (int *) CALLOC(new->ndivs+1,sizeof(int));
    new->cum_nintervals[0] = 0;
    new->cum_nintervals[1] = new->total_nintervals;

    new->nnodes = (int *) CALLOC(new->ndivs,sizeof(int));
    if (FREAD_INT(&(new->nnodes[0]),fp) < 1) {
      fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
      return NULL;
    } else if (new->nnodes[0] < 0) {
      fprintf(stderr,"IIT file %s appears to have a negative number of nodes\n",filename);
      return NULL;
    } else if ((offset += sizeof(int)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after nnodes %ld, filesize %ld)\n",
	      filename,offset,filesize);
      return NULL;
    }
    new->cum_nnodes = (int *) CALLOC(new->ndivs+1,sizeof(int));
    new->cum_nnodes[0] = 0;
    new->cum_nnodes[1] = new->nnodes[0];

    new->divsort = NO_SORT;

    new->divpointers = (unsigned int *) CALLOC(new->ndivs+1,sizeof(unsigned int));
    new->divpointers[0] = 0;
    new->divpointers[1] = 1;

    new->divstrings = (char *) CALLOC(1,sizeof(char));
    new->divstrings[0] = '\0';

  } else {

    if (FREAD_INT(&new->ndivs,fp) < 1) {
      fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
      return NULL;
    } else if (new->ndivs < 0) {
      fprintf(stderr,"IIT file %s appears to have a negative number of divs\n",filename);
      return NULL;
    } else if ((offset += sizeof(int)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after ndivs %ld, filesize %ld)\n",
	      filename,offset,filesize);
      return NULL;
    }
    debug(printf("ndivs: %d\n",new->ndivs));

    new->nintervals = (int *) CALLOC(new->ndivs,sizeof(int));
    offset += sizeof(int)*FREAD_INTS(new->nintervals,new->ndivs,fp);
    debug(
	  printf("nintervals:");
	  for (i = 0; i < new->ndivs; i++) {
	    printf(" %d",new->nintervals[i]);
	  }
	  printf("\n");
	  );

    new->cum_nintervals = (int *) CALLOC(new->ndivs+1,sizeof(int));
    offset += sizeof(int)*FREAD_INTS(new->cum_nintervals,new->ndivs+1,fp);
    debug(
	  printf("cum_nintervals:");
	  for (i = 0; i < new->ndivs; i++) {
	    printf(" %d",new->cum_nintervals[i]);
	  }
	  printf("\n");
	  );

    new->nnodes = (int *) CALLOC(new->ndivs,sizeof(int));
    offset += sizeof(int)*FREAD_INTS(new->nnodes,new->ndivs,fp);
    debug(
	  printf("nnodes:");
	  for (i = 0; i < new->ndivs; i++) {
	    printf(" %d",new->nnodes[i]);
	  }
	  printf("\n");
	  );

    new->cum_nnodes = (int *) CALLOC(new->ndivs+1,sizeof(int));
    offset += sizeof(int)*FREAD_INTS(new->cum_nnodes,new->ndivs+1,fp);
    debug(
	  printf("cum_nnodes:");
	  for (i = 0; i < new->ndivs; i++) {
	    printf(" %d",new->cum_nnodes[i]);
	  }
	  printf("\n");
	  );

    if (FREAD_INT(&new->divsort,fp) < 1) {
      fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
      return NULL;
    } else if (new->divsort < 0) {
      fprintf(stderr,"IIT file %s appears to have a negative value for divsort\n",filename);
      return NULL;
    } else if ((offset += sizeof(int)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after divsort %ld, filesize %ld)\n",
	      filename,offset,filesize);
      return NULL;
    }
    debug(printf("divsort: %d\n",new->divsort));

    new->divpointers = (unsigned int *) CALLOC(new->ndivs+1,sizeof(unsigned int));
    offset += sizeof(int)*FREAD_UINTS(new->divpointers,new->ndivs+1,fp);
    debug(
	  printf("divpointers:");
	  for (i = 0; i < new->ndivs+1; i++) {
	    printf(" %u",new->divpointers[i]);
	  }
	  printf("\n");
	  );

    stringlen = new->divpointers[new->ndivs];
    new->divstrings = (char *) CALLOC(stringlen,sizeof(char));
    offset += sizeof(char)*FREAD_CHARS(new->divstrings,stringlen,fp);
    debug(
	  printf("divstrings:\n");
	  for (i = 0; i < stringlen; i++) {
	    if (new->divstrings[i] == '\0') {
	      printf("\n");
	    } else {
	      printf("%c",new->divstrings[i]);
	    }
	  }
	  printf("(end of divstrings)\n");
	  );
  }

  new->alphas = (int **) CALLOC(new->ndivs,sizeof(int *));
  new->betas = (int **) CALLOC(new->ndivs,sizeof(int *));
  new->sigmas = (int **) CALLOC(new->ndivs,sizeof(int *));
  new->omegas = (int **) CALLOC(new->ndivs,sizeof(int *));
  new->nodes = (struct FNode_T **) CALLOC(new->ndivs,sizeof(struct FNode_T *));
  new->intervals = (struct Interval_T **) CALLOC(new->ndivs,sizeof(struct Interval_T *));

  if (divread == READ_ALL) {
    /* Read all divs */
    debug(printf("Reading all divs\n"));
    for (divno = 0; divno < new->ndivs; divno++) {
      debug(fprintf(stderr,"Starting read of div\n"));
      offset = read_tree(offset,filesize,fp,filename,new,divno);
      debug(fprintf(stderr,"Ending read of div\n"));
    }

    new->intervals[0] = (struct Interval_T *) CALLOC(new->total_nintervals,sizeof(struct Interval_T));
    offset = read_intervals(offset,filesize,fp,filename,new,/*divno*/0);
    for (divno = 1; divno < new->ndivs; divno++) {
      new->intervals[divno] = &(new->intervals[divno-1][new->nintervals[divno-1]]);
      offset = read_intervals(offset,filesize,fp,filename,new,divno);
    }

  } else if (divread == READ_NONE) {
    debug(printf("Reading no divs\n"));
    offset = skip_trees(offset,filesize,fp,filename,new->ndivs,
			new->cum_nintervals[new->ndivs],new->cum_nnodes[new->ndivs]);

    new->intervals[0] = (struct Interval_T *) CALLOC(new->total_nintervals,sizeof(struct Interval_T));
    offset = read_intervals(offset,filesize,fp,filename,new,/*divno*/0);
    for (divno = 1; divno < new->ndivs; divno++) {
      new->intervals[divno] = &(new->intervals[divno-1][new->nintervals[divno-1]]);
      offset = read_intervals(offset,filesize,fp,filename,new,divno);
    }

  } else if (divread == READ_ONE) {
    debug(printf("Reading only div %s\n",divstring));
    if ((desired_divno = IIT_divint(new,divstring)) < 0) {
      fprintf(stderr,"Cannot find div %s in IIT_read.  Ignoring div.\n",divstring);
      desired_divno = 0;
    }
    offset = skip_trees(offset,filesize,fp,filename,desired_divno,
			new->cum_nintervals[desired_divno],new->cum_nnodes[desired_divno]);
    debug1(fprintf(stderr,"Starting read of div\n"));
    offset = read_tree(offset,filesize,fp,filename,new,desired_divno);
    debug1(fprintf(stderr,"Ending read of div\n"));
    offset = skip_trees(offset,filesize,fp,filename,new->ndivs - (desired_divno + 1),
			new->cum_nintervals[new->ndivs] - new->cum_nintervals[desired_divno+1],
			new->cum_nnodes[new->ndivs] - new->cum_nnodes[desired_divno+1]);

    new->intervals[0] = (struct Interval_T *) CALLOC(new->total_nintervals,sizeof(struct Interval_T));
    offset = skip_intervals(&skip_nintervals,offset,filesize,fp,filename,new,0,desired_divno-1);
    debug1(fprintf(stderr,"Starting read of intervals\n"));
    new->intervals[desired_divno] = &(new->intervals[0][skip_nintervals]);
    offset = read_intervals(offset,filesize,fp,filename,new,desired_divno);
    debug1(fprintf(stderr,"Ending read of intervals\n"));
    offset = skip_intervals(&skip_nintervals,offset,filesize,fp,filename,new,desired_divno+1,new->ndivs-1);

    debug(
	  /*
	  printf("sigmas[%d]:\n",desired_divno);
	  for (i = 0; i < new->nintervals[desired_divno]+1; i++) {
	    interval = &(new->intervals[desired_divno][new->sigmas[desired_divno][i]]);
	    printf("%d %u..%u\n",new->sigmas[desired_divno][i],Interval_low(interval),Interval_high(interval));
	  }
	  printf("\n");
	  */

	  printf("alphas[%d]:\n",desired_divno);
	  for (i = 0; i < new->nintervals[desired_divno]+1; i++) {
	    interval = &(new->intervals[desired_divno][new->alphas[desired_divno][i]]);
	    printf("%d %u..%u\n",new->alphas[desired_divno][i],Interval_low(interval),Interval_high(interval));
	  }
	  printf("\n");
	  );


  } else {
    abort();
  }

  read_words(offset,filesize,fp,filename,new);
  fclose(fp);

#ifndef HAVE_MMAP
  debug1(printf("No mmap available.  Reading annotations\n"));
  new->access = FILEIO;
  new->fd = Access_fileio(filename);
  read_annotations(new->fd,new);
  close(new->fd);
  /* pthread_mutex_init(&new->read_mutex,NULL); */
#else
  debug1(printf("mmap available.  Setting up pointers to annotations\n"));
  new->access = MMAPPED;
  if (mmap_annotations(filename,new,readonlyp) == false) {
    debug1(printf("  Failed.  Reading annotations\n"));
    new->access = FILEIO;
    new->fd = Access_fileio(filename);
    read_annotations(new->fd,new);
    close(new->fd);
    /* pthread_mutex_init(&new->read_mutex,NULL); */
  }
#endif
    
  if (newfile != NULL) {
    FREE(newfile);
  }

  return new;
}


void
IIT_debug (char *filename) {
  T new;
  FILE *fp;
  char *newfile = NULL;
  off_t offset = 0, filesize, stringlen;
  int skip_nintervals, desired_divno, divno, i;
  Divread_T divread = READ_ALL;
  char *divstring = NULL;
  bool add_iit_p = false;
#ifdef DEBUG
  Interval_T interval;
#endif

  if ((fp = FOPEN_READ_BINARY(filename)) == NULL) {
    if (add_iit_p == false) {
      /* fprintf(stderr,"Cannot open IIT file %s\n",filename); */
      return;
    } else {
      newfile = (char *) CALLOC(strlen(filename)+strlen(".iit")+1,sizeof(char));
      sprintf(newfile,"%s.iit",filename);
      if ((fp = FOPEN_READ_BINARY(newfile)) == NULL) {
	/* fprintf(stderr,"Cannot open IIT file %s or %s\n",filename,newfile); */
	return;
      } else {
	filename = newfile;
      }
    }
  }

  new = (T) MALLOC(sizeof(*new));
  new->version = 1;

  filesize = Access_filesize(filename);

  new->name = NULL;

  if (FREAD_INT(&new->total_nintervals,fp) < 1) {
    fprintf(stderr,"IIT file %s appears to be empty\n",filename);
    return;
  } else if ((offset += sizeof(int)) > filesize) {
    fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after first byte %ld, filesize %ld)\n",
	    filename,offset,filesize);
    return;
  }

  if (new->total_nintervals == 0) {
    /* New format */
    FREAD_INT(&new->version,fp);
    if (new->version > IIT_LATEST_VERSION) {
      fprintf(stderr,"This file is version %d, but this software can only read up to version %d\n",
	      new->version,IIT_LATEST_VERSION);
      return;
    } else if ((offset += sizeof(int)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after version %ld, filesize %ld)\n",
	      filename,offset,filesize);
      return;
    }
    if (FREAD_INT(&new->total_nintervals,fp) < 1) {
      fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
      return;
    } else if ((offset += sizeof(int)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after nintervals %ld, filesize %ld)\n",
	      filename,offset,filesize);
      return;
    }
  }
  if (new->total_nintervals < 0) {
    fprintf(stderr,"IIT file %s appears to have a negative number of intervals\n",filename);
    return;
  }

  printf("version: %d\n",new->version);
  printf("total_nintervals: %d\n",new->total_nintervals);


  if (FREAD_INT(&new->ntypes,fp) < 1) {
    fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
    return;
  } else if (new->ntypes < 0) {
    fprintf(stderr,"IIT file %s appears to have a negative number of types\n",filename);
    return;
  } else if ((offset += sizeof(int)) > filesize) {
    fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after ntypes %ld, filesize %ld)\n",
	    filename,offset,filesize);
    return;
  }
  printf("ntypes: %d\n",new->ntypes);


  if (new->version < 2) {
    new->nfields = 0;
  } else {
    if (FREAD_INT(&new->nfields,fp) < 1) {
      fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
      return;
    } else if (new->nfields < 0) {
      fprintf(stderr,"IIT file %s appears to have a negative number of fields\n",filename);
      return;
    } else if ((offset += sizeof(int)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after nfields %ld, filesize %ld)\n",
	      filename,offset,filesize);
      return;
    }
  }
  printf("nfields: %d\n",new->nfields);


  if (new->version <= 2) {
    new->ndivs = 1;

    new->nintervals = (int *) CALLOC(new->ndivs,sizeof(int));
    new->nintervals[0] = new->total_nintervals;
    new->cum_nintervals = (int *) CALLOC(new->ndivs+1,sizeof(int));
    new->cum_nintervals[0] = 0;
    new->cum_nintervals[1] = new->total_nintervals;

    new->nnodes = (int *) CALLOC(new->ndivs,sizeof(int));
    if (FREAD_INT(&(new->nnodes[0]),fp) < 1) {
      fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
      return;
    } else if (new->nnodes[0] < 0) {
      fprintf(stderr,"IIT file %s appears to have a negative number of nodes\n",filename);
      return;
    } else if ((offset += sizeof(int)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after nnodes %ld, filesize %ld)\n",
	      filename,offset,filesize);
      return;
    }
    new->cum_nnodes = (int *) CALLOC(new->ndivs+1,sizeof(int));
    new->cum_nnodes[0] = 0;
    new->cum_nnodes[1] = new->nnodes[0];

    new->divsort = NO_SORT;

    new->divpointers = (unsigned int *) CALLOC(new->ndivs+1,sizeof(unsigned int));
    new->divpointers[0] = 0;
    new->divpointers[1] = 1;

    new->divstrings = (char *) CALLOC(1,sizeof(char));
    new->divstrings[0] = '\0';

  } else {

    if (FREAD_INT(&new->ndivs,fp) < 1) {
      fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
      return;
    } else if (new->ndivs < 0) {
      fprintf(stderr,"IIT file %s appears to have a negative number of divs\n",filename);
      return;
    } else if ((offset += sizeof(int)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after ndivs %ld, filesize %ld)\n",
	      filename,offset,filesize);
      return;
    }
    printf("ndivs: %d\n",new->ndivs);

    new->nintervals = (int *) CALLOC(new->ndivs,sizeof(int));
    offset += sizeof(int)*FREAD_INTS(new->nintervals,new->ndivs,fp);
    printf("nintervals:");
    for (i = 0; i < new->ndivs; i++) {
      printf(" %d",new->nintervals[i]);
    }
    printf("\n");

    new->cum_nintervals = (int *) CALLOC(new->ndivs+1,sizeof(int));
    offset += sizeof(int)*FREAD_INTS(new->cum_nintervals,new->ndivs+1,fp);
    printf("cum_nintervals:");
    for (i = 0; i <= new->ndivs; i++) {
      printf(" %d",new->cum_nintervals[i]);
    }
    printf("\n");

    new->nnodes = (int *) CALLOC(new->ndivs,sizeof(int));
    offset += sizeof(int)*FREAD_INTS(new->nnodes,new->ndivs,fp);
    printf("nnodes:");
    for (i = 0; i < new->ndivs; i++) {
      printf(" %d",new->nnodes[i]);
    }
    printf("\n");

    new->cum_nnodes = (int *) CALLOC(new->ndivs+1,sizeof(int));
    offset += sizeof(int)*FREAD_INTS(new->cum_nnodes,new->ndivs+1,fp);
    printf("cum_nnodes:");
    for (i = 0; i <= new->ndivs; i++) {
      printf(" %d",new->cum_nnodes[i]);
    }
    printf("\n");

    if (FREAD_INT(&new->divsort,fp) < 1) {
      fprintf(stderr,"IIT file %s appears to be truncated\n",filename);
      return;
    } else if (new->divsort < 0) {
      fprintf(stderr,"IIT file %s appears to have a negative value for divsort\n",filename);
      return;
    } else if ((offset += sizeof(int)) > filesize) {
      fprintf(stderr,"IIT file %s appears to have an offset that is too large (offset after divsort %ld, filesize %ld)\n",
	      filename,offset,filesize);
      return;
    }
    printf("divsort: %d\n",new->divsort);

    new->divpointers = (unsigned int *) CALLOC(new->ndivs+1,sizeof(unsigned int));
    offset += sizeof(int)*FREAD_UINTS(new->divpointers,new->ndivs+1,fp);
    printf("divpointers:");
    for (i = 0; i < new->ndivs+1; i++) {
      printf(" %u",new->divpointers[i]);
    }
    printf("\n");

    stringlen = new->divpointers[new->ndivs];
    new->divstrings = (char *) CALLOC(stringlen,sizeof(char));
    offset += sizeof(char)*FREAD_CHARS(new->divstrings,stringlen,fp);
    printf("divstrings:\n");
    for (i = 0; i < stringlen; i++) {
      if (new->divstrings[i] == '\0') {
	printf("\n");
      } else {
	printf("%c",new->divstrings[i]);
      }
    }
  }

  new->alphas = (int **) CALLOC(new->ndivs,sizeof(int *));
  new->betas = (int **) CALLOC(new->ndivs,sizeof(int *));
  new->sigmas = (int **) CALLOC(new->ndivs,sizeof(int *));
  new->omegas = (int **) CALLOC(new->ndivs,sizeof(int *));
  new->nodes = (struct FNode_T **) CALLOC(new->ndivs,sizeof(struct FNode_T *));
  new->intervals = (struct Interval_T **) CALLOC(new->ndivs,sizeof(struct Interval_T *));

  if (divread == READ_ALL) {
    /* Read all divs */
    debug(printf("Reading all divs\n"));
    for (divno = 0; divno < new->ndivs; divno++) {
      debug(printf("Div %d tree\n",divno));
      offset = read_tree(offset,filesize,fp,filename,new,divno);
    }

    debug(printf("Div 0 intervals\n"));
    new->intervals[0] = (struct Interval_T *) CALLOC(new->total_nintervals,sizeof(struct Interval_T));
    offset = read_intervals(offset,filesize,fp,filename,new,/*divno*/0);
    for (divno = 1; divno < new->ndivs; divno++) {
      debug(printf("Div %d intervals\n",divno));
      new->intervals[divno] = &(new->intervals[divno-1][new->nintervals[divno-1]]);
      offset = read_intervals(offset,filesize,fp,filename,new,divno);
    }

  } else if (divread == READ_NONE) {
    debug(printf("Reading no divs\n"));
    offset = skip_trees(offset,filesize,fp,filename,new->ndivs,
			new->cum_nintervals[new->ndivs],new->cum_nnodes[new->ndivs]);

    new->intervals[0] = (struct Interval_T *) CALLOC(new->total_nintervals,sizeof(struct Interval_T));
    offset = read_intervals(offset,filesize,fp,filename,new,/*divno*/0);
    for (divno = 1; divno < new->ndivs; divno++) {
      new->intervals[divno] = &(new->intervals[divno-1][new->nintervals[divno-1]]);
      offset = read_intervals(offset,filesize,fp,filename,new,divno);
    }

  } else if (divread == READ_ONE) {
    debug(printf("Reading only div %s\n",divstring));
    if ((desired_divno = IIT_divint(new,divstring)) < 0) {
      fprintf(stderr,"Cannot find div %s in IIT_read.  Ignoring div.\n",divstring);
      desired_divno = 0;
    }
    offset = skip_trees(offset,filesize,fp,filename,desired_divno,
			new->cum_nintervals[desired_divno],new->cum_nnodes[desired_divno]);
    debug1(fprintf(stderr,"Starting read of div\n"));
    offset = read_tree(offset,filesize,fp,filename,new,desired_divno);
    debug1(fprintf(stderr,"Ending read of div\n"));
    offset = skip_trees(offset,filesize,fp,filename,new->ndivs - (desired_divno + 1),
			new->cum_nintervals[new->ndivs] - new->cum_nintervals[desired_divno+1],
			new->cum_nnodes[new->ndivs] - new->cum_nnodes[desired_divno+1]);

    new->intervals[0] = (struct Interval_T *) CALLOC(new->total_nintervals,sizeof(struct Interval_T));
    offset = skip_intervals(&skip_nintervals,offset,filesize,fp,filename,new,0,desired_divno-1);
    debug1(fprintf(stderr,"Starting read of intervals\n"));
    new->intervals[desired_divno] = &(new->intervals[0][skip_nintervals]);
    offset = read_intervals(offset,filesize,fp,filename,new,desired_divno);
    debug1(fprintf(stderr,"Ending read of intervals\n"));
    offset = skip_intervals(&skip_nintervals,offset,filesize,fp,filename,new,desired_divno+1,new->ndivs-1);

    debug(
	  /*
	  printf("sigmas[%d]:\n",desired_divno);
	  for (i = 0; i < new->nintervals[desired_divno]+1; i++) {
	    interval = &(new->intervals[desired_divno][new->sigmas[desired_divno][i]]);
	    printf("%d %u..%u\n",new->sigmas[desired_divno][i],Interval_low(interval),Interval_high(interval));
	  }
	  printf("\n");
	  */

	  printf("alphas[%d]:\n",desired_divno);
	  for (i = 0; i < new->nintervals[desired_divno]+1; i++) {
	    interval = &(new->intervals[desired_divno][new->alphas[desired_divno][i]]);
	    printf("%d %u..%u\n",new->alphas[desired_divno][i],Interval_low(interval),Interval_high(interval));
	  }
	  printf("\n");
	  );


  } else {
    abort();
  }

  read_words_debug(offset,filesize,fp,filename,new);
  fclose(fp);

#ifndef HAVE_MMAP
  debug1(printf("No mmap available.  Reading annotations\n"));
  new->access = FILEIO;
  new->fd = Access_fileio(filename);
  read_annotations(new->fd,new);
  close(new->fd);
  /* pthread_mutex_init(&new->read_mutex,NULL); */
#else
  debug1(printf("mmap available.  Setting up pointers to annotations\n"));
  new->access = MMAPPED;
  if (mmap_annotations(filename,new,/*readonlyp*/true) == false) {
    debug1(printf("  Failed.  Reading annotations\n"));
    new->access = FILEIO;
    new->fd = Access_fileio(filename);
    read_annotations(new->fd,new);
    close(new->fd);
    /* pthread_mutex_init(&new->read_mutex,NULL); */
  }
#endif

  if (newfile != NULL) {
    FREE(newfile);
  }

  return;
}


/************************************************************************/

static void 
fnode_query_aux (int *min, int *max, T this, int divno, int nodeindex, unsigned int x) {
  int lambda;
  FNode_T node;

  if (nodeindex == -1) {
    return;
  }

  node = &(this->nodes[divno][nodeindex]);
  if (x == node->value) {
    debug(printf("%uD:\n",node->value));
    if (node->a < *min) {
      *min = node->a;
    }
    if (node->b > *max) {
      *max = node->b;
    }
    return;
  } else if (x < node->value) {
    fnode_query_aux(&(*min),&(*max),this,divno,node->leftindex,x);
    debug(printf("%uL:\n",node->value));
    if (node->a < *min) {
      *min = node->a;
    }
    for (lambda = node->a; lambda <= node->b; lambda++) {
      debug(printf("Looking at lambda %d, segment %d\n",
		   lambda,this->sigmas[divno][lambda]));
      if (Interval_is_contained(x,this->intervals[divno],this->sigmas[divno][lambda]) == true) {
	if (lambda > *max) {
	  *max = lambda;
	}
      } else {
	return;
      }
    }
    return;
  } else { 
    /* (node->value < x) */
    fnode_query_aux(&(*min),&(*max),this,divno,node->rightindex,x);
    debug(printf("%uR:\n", node->value));
    if (node->b > *max) {
      *max = node->b;
    }
    for (lambda = node->b; lambda >= node->a; lambda--) {
      debug(printf("Looking at lambda %d, segment %d\n",
		   lambda,this->omegas[divno][lambda]));
      if (Interval_is_contained(x,this->intervals[divno],this->omegas[divno][lambda]) == true) {
	if (lambda < *min) {
	  *min = lambda;
	}
      } else {
	return;
      }
    }
    return;
  }
}

/************************************************************************/

int *
IIT_find (int *nmatches, T this, char *label) {
  int *matches = NULL, j;
  int low, middle, high, recno;
  bool foundp = false;
  int cmp;

  low = 0;
  high = this->total_nintervals;
  *nmatches = 0;

#ifdef DEBUG
#ifndef WORDS_BIGENDIAN
#ifdef HAVE_64_BIT
  for (middle = low; middle < high; middle++) {
    printf("%d:%d:%s, ",middle,this->labelorder[middle],
	   &(this->labels[this->labelpointers8[this->labelorder[middle]]]));
  }
  printf("\n");
#endif
#endif
#endif

  while (!foundp && low < high) {
    middle = (low+high)/2;
#ifdef DEBUG
#ifndef WORDS_BIGENDIAN
#ifdef HAVE_64_BIT
    printf("low %d:%d:%s. middle %d:%d:%s high %d:%d:%s\n",
	   low,this->labelorder[low],
	   &(this->labels[this->labelpointers8[this->labelorder[low]]]),
	   middle,this->labelorder[middle],
	   &(this->labels[this->labelpointers8[this->labelorder[middle]]]),
	   high,this->labelorder[high],
	   &(this->labels[this->labelpointers8[this->labelorder[high]]]));
#endif
#endif
#endif

#ifdef WORDS_BIGENDIAN
#ifdef HAVE_64_BIT
    if (this->version >= 4) {
      cmp = strcmp(label,&(this->labels[Bigendian_convert_uint8(this->labelpointers8[Bigendian_convert_int(this->labelorder[middle])])]));
    } else {
      cmp = strcmp(label,&(this->labels[Bigendian_convert_uint(this->labelpointers[Bigendian_convert_int(this->labelorder[middle])])]));
    }
#else
    cmp = strcmp(label,&(this->labels[Bigendian_convert_uint(this->labelpointers[Bigendian_convert_int(this->labelorder[middle])])]));
#endif
#else
#ifdef HAVE_64_BIT
    if (this->version >= 4) {
      cmp = strcmp(label,&(this->labels[this->labelpointers8[this->labelorder[middle]]]));
    } else {
      cmp = strcmp(label,&(this->labels[this->labelpointers[this->labelorder[middle]]]));
    }
#else
    cmp = strcmp(label,&(this->labels[this->labelpointers[this->labelorder[middle]]]));
#endif
#endif

    if (cmp < 0) {
      high = middle;
    } else if (cmp > 0) {
      low = middle + 1;
    } else {
      foundp = true;
    }
  }

  if (foundp == true) {
    low = middle;
#ifdef WORDS_BIGENDIAN
#ifdef HAVE_64_BIT
    if (this->version >= 4) {
      while (low-1 >= 0 && 
	     !strcmp(label,&(this->labels[Bigendian_convert_uint8(this->labelpointers8[Bigendian_convert_int(this->labelorder[low-1])])]))) {
	low--;
      }
    } else {
      while (low-1 >= 0 && 
	     !strcmp(label,&(this->labels[Bigendian_convert_uint(this->labelpointers[Bigendian_convert_int(this->labelorder[low-1])])]))) {
	low--;
      }
    }
#else
    while (low-1 >= 0 && 
	   !strcmp(label,&(this->labels[Bigendian_convert_uint(this->labelpointers[Bigendian_convert_int(this->labelorder[low-1])])]))) {
      low--;
    }
#endif
#else
#ifdef HAVE_64_BIT
    if (this->version >= 4) {
      while (low-1 >= 0 && 
	     !strcmp(label,&(this->labels[this->labelpointers8[this->labelorder[low-1]]]))) {
	low--;
      }
    } else {
      while (low-1 >= 0 && 
	     !strcmp(label,&(this->labels[this->labelpointers[this->labelorder[low-1]]]))) {
	low--;
      }
    }
#else
    while (low-1 >= 0 && 
	   !strcmp(label,&(this->labels[this->labelpointers[this->labelorder[low-1]]]))) {
      low--;
    }
#endif
#endif
    
    high = middle;
#ifdef WORDS_BIGENDIAN
#ifdef HAVE_64_BIT
    if (this->version >= 4) {
      while (high+1 < this->total_nintervals && 
	     !strcmp(label,&(this->labels[Bigendian_convert_uint8(this->labelpointers8[Bigendian_convert_int(this->labelorder[high+1])])]))) {
	high++;
      }
    } else {
      while (high+1 < this->total_nintervals && 
	     !strcmp(label,&(this->labels[Bigendian_convert_uint(this->labelpointers[Bigendian_convert_int(this->labelorder[high+1])])]))) {
	high++;
      }
    }
#else
    while (high+1 < this->total_nintervals && 
	   !strcmp(label,&(this->labels[Bigendian_convert_uint(this->labelpointers[Bigendian_convert_int(this->labelorder[high+1])])]))) {
      high++;
    }
#endif
#else
#ifdef HAVE_64_BIT
    if (this->version >= 4) {
      while (high+1 < this->total_nintervals && 
	     !strcmp(label,&(this->labels[this->labelpointers8[this->labelorder[high+1]]]))) {
	high++;
      }
    } else {
      while (high+1 < this->total_nintervals && 
	     !strcmp(label,&(this->labels[this->labelpointers[this->labelorder[high+1]]]))) {
	high++;
      }
    }
#else
    while (high+1 < this->total_nintervals && 
	   !strcmp(label,&(this->labels[this->labelpointers[this->labelorder[high+1]]]))) {
      high++;
    }
#endif
#endif

    
    *nmatches = high - low + 1;
    if (*nmatches > 0) {
      matches = (int *) CALLOC(*nmatches,sizeof(int));
      j = 0;
      for (recno = low; recno <= high; recno++) {
#ifdef WORDS_BIGENDIAN
#ifdef DEBUG
	printf("Pushing %d:%d\n",recno,Bigendian_convert_int(this->labelorder[recno]));
#endif
	matches[j++] = Bigendian_convert_int(this->labelorder[recno])+1;
	
#else
#ifdef DEBUG
	printf("Pushing %d:%d\n",recno,this->labelorder[recno]);
#endif
	matches[j++] = this->labelorder[recno]+1;
#endif
      }
    }
  }

  return matches;
}

/* Slow.  Used before binary search method above. */
int
IIT_find_linear (T this, char *label) {
  int i;
  char *p;

  for (i = 0; i < this->total_nintervals; i++) {
#ifdef WORDS_BIGENDIAN
#ifdef HAVE_64_BIT
    if (this->version >= 4) {
      p = &(this->labels[Bigendian_convert_uint8(this->labelpointers8[i])]);
    } else {
      p = &(this->labels[Bigendian_convert_uint(this->labelpointers[i])]);
    }
#else
    p = &(this->labels[Bigendian_convert_uint(this->labelpointers[i])]);
#endif
#else
#ifdef HAVE_64_BIT
    if (this->version >= 4) {
      p = &(this->labels[this->labelpointers8[i]]);
    } else {
      p = &(this->labels[this->labelpointers[i]]);
    }
#else
    p = &(this->labels[this->labelpointers[i]]);
#endif
#endif
    while (isspace((int) *p)) {
      p++;
    }
    if (!strcmp(label,p)) {
      return i + 1;
    }
  }

  return -1;
}

int
IIT_find_one (T this, char *label) {
  int index;
  int *matches, nmatches;

  matches = IIT_find(&nmatches,this,label);
  if (nmatches == 0) {
    /*
    fprintf(stderr,"Expected one match for %s, but got 0\n",
	    label);
    */
    index = -1;
  } else {
    if (nmatches > 1) {
      fprintf(stderr,"Expected one match for %s, but got %d\n",
	      label,nmatches);
    }
    index = matches[0];
    FREE(matches);
  }

  return index;
}    


/************************************************************************/


static int
int_compare (const void *a, const void *b) {
  int x = * (int *) a;
  int y = * (int *) b;

  if (x < y) {
    return -1;
  } else if (y < x) {
    return 1;
  } else {
    return 0;
  }
}


int *
IIT_get (int *nmatches, T this, char *divstring, unsigned int x, unsigned int y, bool sortp) {
  int *sorted, *matches = NULL, matchstart, *uniq, neval, nuniq, i;
  int lambda, prev;
  int divno;
  int min1, max1 = 0, min2, max2 = 0;

  divno = IIT_divint(this,divstring);
#if 1
  /* Usually don't need to check, unless crossing between iits,
     because divstring comes from same iit */
  if (divno < 0) {
    /* fprintf(stderr,"No div %s found in iit file\n",divstring); */
    *nmatches = 0;
    return (int *) NULL;
  }
#endif
  min1 = min2 = this->nintervals[divno] + 1;

  debug(printf("Entering IIT_get with query %u %u\n",x,y));
  fnode_query_aux(&min1,&max1,this,divno,0,x);
  fnode_query_aux(&min2,&max2,this,divno,0,y);
  debug(printf("min1=%d max1=%d  min2=%d max2=%d\n",min1,max1,min2,max2));

  *nmatches = 0;
  if (max2 >= min1) {
    neval = (max2 - min1 + 1) + (max2 - min1 + 1);
    matches = (int *) CALLOC(neval,sizeof(int));
    uniq = (int *) CALLOC(neval,sizeof(int));

    i = 0;
    for (lambda = min1; lambda <= max2; lambda++) {
      matches[i++] = this->sigmas[divno][lambda];
      matches[i++] = this->omegas[divno][lambda];
    }

    /* Eliminate duplicates */
    qsort(matches,neval,sizeof(int),int_compare);
    nuniq = 0;
    prev = 0;
    debug(printf("unique segments in lambda %d to %d:",min1,max2));
    for (i = 0; i < neval; i++) {
      if (matches[i] != prev) {
	debug(printf(" %d",matches[i]));
	uniq[nuniq++] = matches[i];
	prev = matches[i];
      }
    }
    debug(printf("\n"));

    for (i = 0; i < nuniq; i++) {
      if (Interval_overlap_p(x,y,this->intervals[divno],uniq[i]) == true) {
	matches[(*nmatches)++] = uniq[i];
	debug(printf("Pushing overlapping segment %d (%u..%u)\n",uniq[i],
		     Interval_low(&(this->intervals[divno][uniq[i]-1])),
		     Interval_high(&(this->intervals[divno][uniq[i]-1]))));
      } else {
	debug(printf("Not pushing non-overlapping segment %d (%u..%u)\n",uniq[i],
		     Interval_low(&(this->intervals[divno][uniq[i]-1])),
		     Interval_high(&(this->intervals[divno][uniq[i]-1]))));
      }
    }

    FREE(uniq);
  }

  /* Convert to universal indices */
  matchstart = this->cum_nintervals[divno];
  for (i = 0; i < *nmatches; i++) {
    matches[i] += matchstart;
  }

  if (sortp == false) {
    return matches;
#if 0
  } else if (this->version <= 2) {
    sorted = sort_matches_by_type(this,matches,*nmatches,/*alphabetizep*/true);
    FREE(matches);
    return sorted;
#endif
  } else {
    sorted = sort_matches_by_position(this,matches,*nmatches,divstring,divno);
    FREE(matches);
    return sorted;
  }
}

int *
IIT_get_with_divno (int *nmatches, T this, int divno, unsigned int x, unsigned int y, bool sortp) {
  int *sorted, *matches = NULL, matchstart, *uniq, neval, nuniq, i;
  int lambda, prev;
  int min1, max1 = 0, min2, max2 = 0;

  if (divno < 0) {
    /* fprintf(stderr,"No div %s found in iit file\n",divstring); */
    *nmatches = 0;
    return (int *) NULL;
  }
  min1 = min2 = this->nintervals[divno] + 1;

  debug(printf("Entering IIT_get_with_divno with divno %d and query %u %u\n",divno,x,y));
  fnode_query_aux(&min1,&max1,this,divno,0,x);
  fnode_query_aux(&min2,&max2,this,divno,0,y);
  debug(printf("min1=%d max1=%d  min2=%d max2=%d\n",min1,max1,min2,max2));

  *nmatches = 0;
  if (max2 >= min1) {
    neval = (max2 - min1 + 1) + (max2 - min1 + 1);
    matches = (int *) CALLOC(neval,sizeof(int));
    uniq = (int *) CALLOC(neval,sizeof(int));

    i = 0;
    for (lambda = min1; lambda <= max2; lambda++) {
      matches[i++] = this->sigmas[divno][lambda];
      matches[i++] = this->omegas[divno][lambda];
    }

    /* Eliminate duplicates */
    qsort(matches,neval,sizeof(int),int_compare);
    nuniq = 0;
    prev = 0;
    debug(printf("unique segments in lambda %d to %d:",min1,max2));
    for (i = 0; i < neval; i++) {
      if (matches[i] != prev) {
	debug(printf(" %d",matches[i]));
	uniq[nuniq++] = matches[i];
	prev = matches[i];
      }
    }
    debug(printf("\n"));

    for (i = 0; i < nuniq; i++) {
      if (Interval_overlap_p(x,y,this->intervals[divno],uniq[i]) == true) {
	matches[(*nmatches)++] = uniq[i];
	debug(printf("Pushing overlapping segment %d (%u..%u)\n",uniq[i],
		     Interval_low(&(this->intervals[divno][uniq[i]-1])),
		     Interval_high(&(this->intervals[divno][uniq[i]-1]))));
      } else {
	debug(printf("Not pushing non-overlapping segment %d (%u..%u)\n",uniq[i],
		     Interval_low(&(this->intervals[divno][uniq[i]-1])),
		     Interval_high(&(this->intervals[divno][uniq[i]-1]))));
      }
    }

    FREE(uniq);
  }

  /* Convert to universal indices */
  matchstart = this->cum_nintervals[divno];
  for (i = 0; i < *nmatches; i++) {
    matches[i] += matchstart;
  }

  if (sortp == false) {
    return matches;
#if 0
  } else if (this->version <= 2) {
    sorted = sort_matches_by_type(this,matches,*nmatches,/*alphabetizep*/true);
    FREE(matches);
    return sorted;
#endif
  } else {
    sorted = sort_matches_by_position_with_divno(this,matches,*nmatches,divno);
    FREE(matches);
    return sorted;
  }
}

int *
IIT_get_signed_with_divno (int *nmatches, T this, int divno, unsigned int x, unsigned int y, bool sortp,
			   int sign) {
  int *sorted, *matches = NULL, matchstart, *uniq, neval, nuniq, i;
  int lambda, prev;
  int min1, max1 = 0, min2, max2 = 0;
  int index;

  if (divno < 0) {
    /* fprintf(stderr,"No div %s found in iit file\n",divstring); */
    *nmatches = 0;
    return (int *) NULL;
  }
  min1 = min2 = this->nintervals[divno] + 1;

  debug(printf("Entering IIT_get_with_divno with divno %d and query %u %u\n",divno,x,y));
  fnode_query_aux(&min1,&max1,this,divno,0,x);
  fnode_query_aux(&min2,&max2,this,divno,0,y);
  debug(printf("min1=%d max1=%d  min2=%d max2=%d\n",min1,max1,min2,max2));

  *nmatches = 0;
  if (max2 >= min1) {
    neval = (max2 - min1 + 1) + (max2 - min1 + 1);
    matches = (int *) CALLOC(neval,sizeof(int));
    uniq = (int *) CALLOC(neval,sizeof(int));

    i = 0;
    for (lambda = min1; lambda <= max2; lambda++) {
      index = this->sigmas[divno][lambda];
      if (sign == 0 || Interval_sign(&(this->intervals[divno][index-1])) == sign) {
	matches[i++] = index;
      }
      index = this->omegas[divno][lambda];
      if (sign == 0 || Interval_sign(&(this->intervals[divno][index-1])) == sign) {
	matches[i++] = index;
      }
    }

    /* Eliminate duplicates */
    qsort(matches,neval,sizeof(int),int_compare);
    nuniq = 0;
    prev = 0;
    debug(printf("unique segments in lambda %d to %d:",min1,max2));
    for (i = 0; i < neval; i++) {
      if (matches[i] != prev) {
	debug(printf(" %d",matches[i]));
	uniq[nuniq++] = matches[i];
	prev = matches[i];
      }
    }
    debug(printf("\n"));

    for (i = 0; i < nuniq; i++) {
      if (Interval_overlap_p(x,y,this->intervals[divno],uniq[i]) == true) {
	matches[(*nmatches)++] = uniq[i];
	debug(printf("Pushing overlapping segment %d (%u..%u)\n",uniq[i],
		     Interval_low(&(this->intervals[divno][uniq[i]-1])),
		     Interval_high(&(this->intervals[divno][uniq[i]-1]))));
      } else {
	debug(printf("Not pushing non-overlapping segment %d (%u..%u)\n",uniq[i],
		     Interval_low(&(this->intervals[divno][uniq[i]-1])),
		     Interval_high(&(this->intervals[divno][uniq[i]-1]))));
      }
    }

    FREE(uniq);
  }

  /* Convert to universal indices */
  matchstart = this->cum_nintervals[divno];
  for (i = 0; i < *nmatches; i++) {
    matches[i] += matchstart;
  }

  if (sortp == false) {
    return matches;
#if 0
  } else if (this->version <= 2) {
    sorted = sort_matches_by_type(this,matches,*nmatches,/*alphabetizep*/true);
    FREE(matches);
    return sorted;
#endif
  } else {
    sorted = sort_matches_by_position_with_divno(this,matches,*nmatches,divno);
    FREE(matches);
    return sorted;
  }
}


static int
coord_search_low (T this, int divno, unsigned int x) {
  int low, middle, high;
  bool foundp = false;
  unsigned int middlevalue;

  low = 0;
  high = this->nintervals[divno];

  while (!foundp && low < high) {
    middle = (low+high)/2;
    middlevalue = Interval_low(&(this->intervals[divno][this->alphas[divno][middle]-1]));
    if (x < middlevalue) {
      high = middle;
    } else if (x > middlevalue) {
      low = middle + 1;
    } else {
      foundp = true;
    }
  }

  if (foundp == true) {
    return middle;
  } else {
    return low;
  }
}

static int
coord_search_high (T this, int divno, unsigned int x) {
  int low, middle, high;
  bool foundp = false;
  unsigned int middlevalue;

  low = 0;
  high = this->nintervals[divno];

  while (!foundp && low < high) {
    middle = (low+high)/2;
    middlevalue = Interval_high(&(this->intervals[divno][this->betas[divno][middle]-1]));
    if (x < middlevalue) {
      high = middle;
    } else if (x > middlevalue) {
      low = middle + 1;
    } else {
      foundp = true;
    }
  }

  if (foundp == true) {
    return middle;
  } else {
    return high;
  }
}


void
IIT_get_flanking (int **leftflanks, int *nleftflanks, int **rightflanks, int *nrightflanks,
		  T this, char *divstring, unsigned int x, unsigned int y, int nflanking, int sign) {
  int lambda, matchstart, i;
  Interval_T interval;
  bool stopp;
  int divno;

  divno = IIT_divint(this,divstring);

  debug2(printf("Entering IIT_get_flanking with divno %d, query %u %u, nflanking = %d, sign %d\n",divno,x,y,nflanking,sign));

  if (this->alphas[divno] == NULL) {
#if 0
    compute_flanking(this);
#else
    fprintf(stderr,"Flanking hits not supported on version %d of iit files.  Please use iit_update to update your file\n",
	    this->version);
    exit(9);
#endif
  }

  /* Look at alphas for right flank */
  lambda = coord_search_low(this,divno,y);
  debug2(printf("coord_search_low lambda = %d\n",lambda));

  *rightflanks = (int *) CALLOC(nflanking,sizeof(int));
  *nrightflanks = 0;
  stopp = false;
  while (lambda <= this->nintervals[divno] && stopp == false) {
    interval = &(this->intervals[divno][this->alphas[divno][lambda]-1]);
    if (Interval_low(interval) <= y) {
      debug2(printf("Advancing because interval_low %u <= %u\n",Interval_low(interval),y));
      lambda++;
    } else if (sign != 0 && Interval_sign(interval) != sign) {
      debug2(printf("Advancing because sign != 0 && interval_sign %d != %d\n",sign,Interval_sign(interval),sign));
      lambda++;
    } else {
      (*rightflanks)[(*nrightflanks)++] = this->alphas[divno][lambda];
      debug2(printf("Storing right flank %d\n",this->alphas[divno][lambda]));
      if (*nrightflanks < nflanking) {
	debug2(printf("Advancing because need more\n"));
	lambda++;
      } else {
	stopp = true;
      }
    }
  }

  /* Look at betas for left flank */
  lambda = coord_search_high(this,divno,x);

  *leftflanks = (int *) CALLOC(nflanking,sizeof(int));
  *nleftflanks = 0;
  stopp = false;
  while (lambda >= 1 && stopp == false) {
    interval = &(this->intervals[divno][this->betas[divno][lambda]-1]);
    if (Interval_high(interval) >= x) {
      lambda--;
    } else if (sign != 0 && Interval_sign(interval) != sign) {
      lambda--;
    } else {
      (*leftflanks)[(*nleftflanks)++] = this->betas[divno][lambda];
      if (*nleftflanks < nflanking) {
	lambda--;
      } else {
	stopp = true;
      }
    }
  }

  /* Convert to universal indices */
  matchstart = this->cum_nintervals[divno];
  for (i = 0; i < *nrightflanks; i++) {
    (*rightflanks)[i] += matchstart;
  }
  for (i = 0; i < *nleftflanks; i++) {
    (*leftflanks)[i] += matchstart;
  }

  return;
}

void
IIT_get_flanking_with_divno (int **leftflanks, int *nleftflanks, int **rightflanks, int *nrightflanks,
			     T this, int divno, unsigned int x, unsigned int y, int nflanking, int sign) {
  int lambda, matchstart, i;
  Interval_T interval;
  bool stopp;

  debug2(printf("Entering IIT_get_flanking_with_divno with divno %d, query %u %u, nflanking = %d, sign %d\n",divno,x,y,nflanking,sign));

  if (this->alphas[divno] == NULL) {
#if 0
    compute_flanking(this);
#else
    fprintf(stderr,"Flanking hits not supported on version %d of iit files.  Please use iit_update to update your file\n",
	    this->version);
    exit(9);
#endif
  }

  /* Look at alphas for right flank */
  lambda = coord_search_low(this,divno,y);
  debug2(printf("coord_search_low lambda = %d\n",lambda));

  *rightflanks = (int *) CALLOC(nflanking,sizeof(int));
  *nrightflanks = 0;
  stopp = false;
  while (lambda <= this->nintervals[divno] && stopp == false) {
    interval = &(this->intervals[divno][this->alphas[divno][lambda]-1]);
    if (Interval_low(interval) <= y) {
      debug2(printf("Advancing because interval_low %u <= %u\n",Interval_low(interval),y));
      lambda++;
    } else if (sign != 0 && Interval_sign(interval) != sign) {
      debug2(printf("Advancing because sign != 0 && interval_sign %d != %d\n",sign,Interval_sign(interval),sign));
      lambda++;
    } else {
      (*rightflanks)[(*nrightflanks)++] = this->alphas[divno][lambda];
      debug2(printf("Storing right flank %d\n",this->alphas[divno][lambda]));
      if (*nrightflanks < nflanking) {
	debug2(printf("Advancing because need more\n"));
	lambda++;
      } else {
	stopp = true;
      }
    }
  }

  /* Look at betas for left flank */
  lambda = coord_search_high(this,divno,x);

  *leftflanks = (int *) CALLOC(nflanking,sizeof(int));
  *nleftflanks = 0;
  stopp = false;
  while (lambda >= 1 && stopp == false) {
    interval = &(this->intervals[divno][this->betas[divno][lambda]-1]);
    if (Interval_high(interval) >= x) {
      lambda--;
    } else if (sign != 0 && Interval_sign(interval) != sign) {
      lambda--;
    } else {
      (*leftflanks)[(*nleftflanks)++] = this->betas[divno][lambda];
      if (*nleftflanks < nflanking) {
	lambda--;
      } else {
	stopp = true;
      }
    }
  }

  /* Convert to universal indices */
  matchstart = this->cum_nintervals[divno];
  for (i = 0; i < *nrightflanks; i++) {
    (*rightflanks)[i] += matchstart;
  }
  for (i = 0; i < *nleftflanks; i++) {
    (*leftflanks)[i] += matchstart;
  }

  return;
}

void
IIT_get_flanking_typed (int **leftflanks, int *nleftflanks, int **rightflanks, int *nrightflanks,
			T this, char *divstring, unsigned int x, unsigned int y, int nflanking, int type) {
  int lambda, matchstart, i;
  Interval_T interval;
  bool stopp;
  int divno;

  divno = IIT_divint(this,divstring);

  debug(printf("Entering IIT_get_flanking_typed with query %u %u\n",x,y));

  if (this->alphas[divno] == NULL) {
#if 0
    IIT_compute_flanking(this);
#else
    fprintf(stderr,"Flanking hits not supported on version %d of iit files.  Please use iit_update to update your file\n",
	    this->version);
    exit(9);
#endif
  }

  /* Look at alphas for right flank */
  lambda = coord_search_low(this,divno,y);

  *rightflanks = (int *) CALLOC(nflanking,sizeof(int));
  *nrightflanks = 0;
  stopp = false;
  while (lambda <= this->nintervals[divno] && stopp == false) {
    interval = &(this->intervals[divno][this->alphas[divno][lambda]-1]);
    if (Interval_low(interval) <= y) {
      lambda++;
    } else if (Interval_type(interval) != type) {
      lambda++;
    } else {
      (*rightflanks)[(*nrightflanks)++] = this->alphas[divno][lambda];
      if (*nrightflanks < nflanking) {
	lambda++;
      } else {
	stopp = true;
      }
    }
  }

  /* Look at betas for left flank */
  lambda = coord_search_high(this,divno,x);

  *leftflanks = (int *) CALLOC(nflanking,sizeof(int));
  *nleftflanks = 0;
  stopp = false;
  while (lambda >= 1 && stopp == false) {
    interval = &(this->intervals[divno][this->betas[divno][lambda]-1]);
    if (Interval_high(interval) >= x) {
      lambda--;
    } else if (Interval_type(interval) != type) {
      lambda--;
    } else {
      (*leftflanks)[(*nleftflanks)++] = this->betas[divno][lambda];
      if (*nleftflanks < nflanking) {
	lambda--;
      } else {
	stopp = true;
      }
    }
  }

  /* Convert to universal indices */
  matchstart = this->cum_nintervals[divno];
  for (i = 0; i < *nrightflanks; i++) {
    (*rightflanks)[i] += matchstart;
  }
  for (i = 0; i < *nleftflanks; i++) {
    (*leftflanks)[i] += matchstart;
  }

  return;
}

void
IIT_get_flanking_multiple_typed (int **leftflanks, int *nleftflanks, int **rightflanks, int *nrightflanks,
				 T this, char *divstring, unsigned int x, unsigned int y, int nflanking, int *types, int ntypes) {
  int k, i;
  int lambda, matchstart;
  Interval_T interval;
  bool stopp;
  int divno;

  divno = IIT_divint(this,divstring);

  debug(printf("Entering IIT_get_flanking_multiple_typed with query %u %u\n",x,y));

  if (this->alphas[divno] == NULL) {
#if 0
    IIT_compute_flanking(this);
#else
    fprintf(stderr,"Flanking hits not supported on version %d of iit files.  Please use iit_update to update your file\n",
	    this->version);
    exit(9);
#endif
  }

  /* Look at alphas for right flank */
  lambda = coord_search_low(this,divno,y);

  *rightflanks = (int *) CALLOC(nflanking,sizeof(int));
  *nrightflanks = 0;
  stopp = false;
  while (lambda <= this->nintervals[divno] && stopp == false) {
    interval = &(this->intervals[divno][this->alphas[divno][lambda]-1]);
    if (Interval_low(interval) <= y) {
      lambda++;
    } else {
      k = 0;
      while (k < ntypes && Interval_type(interval) != types[k]) {
	k++;
      }
      if (k >= ntypes) {
	lambda++;
      } else {
	(*rightflanks)[(*nrightflanks)++] = this->alphas[divno][lambda];
	if (*nrightflanks < nflanking) {
	  lambda++;
	} else {
	  stopp = true;
	}
      }
    }
  }


  /* Look at betas for left flank */
  lambda = coord_search_high(this,divno,x);

  *leftflanks = (int *) CALLOC(nflanking,sizeof(int));
  *nleftflanks = 0;
  stopp = false;
  while (lambda >= 1 && stopp == false) {
    interval = &(this->intervals[divno][this->betas[divno][lambda]-1]);
    if (Interval_high(interval) >= x) {
      lambda--;
    } else {
      k = 0;
      while (k < ntypes && Interval_type(interval) != types[k]) {
	k++;
      }
      if (k >= ntypes) {
	lambda--;
      } else {
	(*leftflanks)[(*nleftflanks)++] = this->betas[divno][lambda];
	if (*nleftflanks < nflanking) {
	  lambda--;
	} else {
	  stopp = true;
	}
      }
    }
  }

  /* Convert to universal indices */
  matchstart = this->cum_nintervals[divno];
  for (i = 0; i < *nrightflanks; i++) {
    (*rightflanks)[i] += matchstart;
  }
  for (i = 0; i < *nleftflanks; i++) {
    (*leftflanks)[i] += matchstart;
  }

  return;
}


static const Except_T iit_error = { "IIT problem" };

int
IIT_get_one (T this, char *divstring, unsigned int x, unsigned int y) {
  int lambda;
  int min1, max1 = 0, min2, max2 = 0;
  int divno;

  divno = IIT_divint(this,divstring);
  min1 = min2 = this->nintervals[divno] + 1;

  debug(printf("Entering IIT_get_one with query %u %u\n",x,y));
  fnode_query_aux(&min1,&max1,this,divno,0,x);
  fnode_query_aux(&min2,&max2,this,divno,0,y);
  debug(printf("min1=%d max1=%d  min2=%d max2=%d\n",min1,max1,min2,max2));

  if (max2 >= min1) {
    for (lambda = min1; lambda <= max2; lambda++) {
      if (Interval_overlap_p(x,y,this->intervals[divno],this->sigmas[divno][lambda]) == true) {
	return this->sigmas[divno][lambda];
      }
    }
    for (lambda = min1; lambda <= max2; lambda++) {
      if (Interval_overlap_p(x,y,this->intervals[divno],this->omegas[divno][lambda]) == true) {
	return this->omegas[divno][lambda];
      }
    }
  }

  /* fprintf(stderr,"Expected one match for %u--%u, but got none\n",x,y); */
  return this->nintervals[divno]; /* shouldn't we return 0 or -1 for failure? */
}

/* Generally called where intervals don't overlap, like chromosomes,
   and where x == y. */
/*
int
IIT_get_one_safe (T this, unsigned int x, unsigned int y) {
  int index;
  int *matches, nmatches;

  matches = IIT_get(&nmatches,this,x,y,sortp);
  if (nmatches != 1) {
    fprintf(stderr,"Expected one match for %u--%u, but got %d\n",
	    x,y,nmatches);
    abort();
  }
  index = matches[0];
  FREE(matches);
  return index;
}
*/

int *
IIT_get_typed (int *ntypematches, T this, char *divstring, unsigned int x, unsigned int y, int type, bool sortp) {
  int *sorted;
  int index, divno;
  int *typematches = NULL, *matches, nmatches, i, j;
  Interval_T interval;

  *ntypematches = 0;
  matches = IIT_get(&nmatches,this,divstring,x,y,/*sortp*/false);
  for (i = 0; i < nmatches; i++) {
    index = matches[i];
    interval = &(this->intervals[0][index-1]);
    if (Interval_type(interval) == type) {
      (*ntypematches)++;
    }
  }

  if (*ntypematches > 0) {
    typematches = (int *) CALLOC(*ntypematches,sizeof(int));
    j = 0;
    for (i = 0; i < nmatches; i++) {
      index = matches[i];
      interval = &(this->intervals[0][index-1]);
      if (Interval_type(interval) == type) {
	typematches[j++] = index;
      }
    }
  }
  
  if (matches != NULL) {
    FREE(matches);
  }

  if (sortp == false) {
    return typematches;
#if 0
  } else if (this->version <= 2) {
    sorted = sort_matches_by_type(this,typematches,*ntypematches,/*alphabetizep*/false);
    FREE(typematches);
    return sorted;
#endif
  } else {
    divno = IIT_divint(this,divstring);
    sorted = sort_matches_by_position(this,typematches,*ntypematches,divstring,divno);
    FREE(typematches);
    return sorted;
  }
}

int *
IIT_get_typed_with_divno (int *ntypematches, T this, int divno, unsigned int x, unsigned int y, int type, bool sortp) {
  int *sorted;
  int index;
  int *typematches = NULL, *matches, nmatches, i, j;
  Interval_T interval;

  if (divno < 0) {
    /* fprintf(stderr,"No div %s found in iit file\n",divstring); */
    *ntypematches = 0;
    return (int *) NULL;
  }

  *ntypematches = 0;
  matches = IIT_get_with_divno(&nmatches,this,divno,x,y,/*sortp*/false);
  for (i = 0; i < nmatches; i++) {
    index = matches[i];
    interval = &(this->intervals[0][index-1]);
    if (Interval_type(interval) == type) {
      (*ntypematches)++;
    }
  }

  if (*ntypematches > 0) {
    typematches = (int *) CALLOC(*ntypematches,sizeof(int));
    j = 0;
    for (i = 0; i < nmatches; i++) {
      index = matches[i];
      interval = &(this->intervals[0][index-1]);
      if (Interval_type(interval) == type) {
	typematches[j++] = index;
      }
    }
  }
  
  if (matches != NULL) {
    FREE(matches);
  }

  if (sortp == false) {
    return typematches;
#if 0
  } else if (this->version <= 2) {
    sorted = sort_matches_by_type(this,typematches,*ntypematches,/*alphabetizep*/false);
    FREE(typematches);
    return sorted;
#endif
  } else {
    sorted = sort_matches_by_position_with_divno(this,typematches,*ntypematches,divno);
    FREE(typematches);
    return sorted;
  }
}


int *
IIT_get_typed_signed_with_divno (int *ntypematches, T this, int divno, unsigned int x, unsigned int y, 
				 int type, int sign, bool sortp) {
  int *sorted;
  int index;
  int *typematches = NULL, *matches, nmatches, i, j;
  Interval_T interval;

  if (divno < 0) {
    /* fprintf(stderr,"No div %s found in iit file\n",divstring); */
    *ntypematches = 0;
    return (int *) NULL;
  }

  *ntypematches = 0;
  matches = IIT_get_with_divno(&nmatches,this,divno,x,y,/*sortp*/false);
  for (i = 0; i < nmatches; i++) {
    index = matches[i];
    interval = &(this->intervals[0][index-1]);
    if (Interval_type(interval) == type && Interval_sign(interval) == sign) {
      (*ntypematches)++;
    }
  }

  if (*ntypematches > 0) {
    typematches = (int *) CALLOC(*ntypematches,sizeof(int));
    j = 0;
    for (i = 0; i < nmatches; i++) {
      index = matches[i];
      interval = &(this->intervals[0][index-1]);
      if (Interval_type(interval) == type && Interval_sign(interval) == sign) {
	typematches[j++] = index;
      }
    }
  }
  
  if (matches != NULL) {
    FREE(matches);
  }

  if (sortp == false) {
    return typematches;
#if 0
  } else if (this->version <= 2) {
    sorted = sort_matches_by_type(this,typematches,*ntypematches,/*alphabetizep*/false);
    FREE(typematches);
    return sorted;
#endif
  } else {
    sorted = sort_matches_by_position_with_divno(this,typematches,*ntypematches,divno);
    FREE(typematches);
    return sorted;
  }
}


int *
IIT_get_multiple_typed (int *ntypematches, T this, char *divstring, unsigned int x, unsigned int y, 
			int *types, int ntypes, bool sortp) {
  int *sorted;
  int index, divno;
  int *typematches = NULL, *matches, nmatches, i, j, k;
  Interval_T interval;

  *ntypematches = 0;
  matches = IIT_get(&nmatches,this,divstring,x,y,/*sortp*/false);
  for (i = 0; i < nmatches; i++) {
    index = matches[i];
    interval = &(this->intervals[0][index-1]);
    k = 0;
    while (k < ntypes && Interval_type(interval) != types[k]) {
      k++;
    }
    if (k < ntypes) {
      (*ntypematches)++;
    }
  }

  if (*ntypematches > 0) {
    typematches = (int *) CALLOC(*ntypematches,sizeof(int));
    j = 0;
    for (i = 0; i < nmatches; i++) {
      index = matches[i];
      interval = &(this->intervals[0][index-1]);
      k = 0;
      while (k < ntypes && Interval_type(interval) != types[k]) {
	k++;
      }
      if (k < ntypes) {
	typematches[j++] = index;
      }
    }
  }
  
  if (matches != NULL) {
    FREE(matches);
  }

  if (sortp == false || this->version >= 3) {
    return typematches;
#if 0
  } else if (this->version <= 2) {
    sorted = sort_matches_by_type(this,typematches,*ntypematches,/*alphabetizep*/true);
    FREE(typematches);
    return sorted;
#endif
  } else {
    divno = IIT_divint(this,divstring);
    sorted = sort_matches_by_position(this,typematches,*ntypematches,divstring,divno);
    FREE(typematches);
    return sorted;
  }
}

int
IIT_get_exact (T this, char *divstring, unsigned int x, unsigned int y, int type) {
  int index;
  int *matches, nmatches, i;
  Interval_T interval;

  matches = IIT_get(&nmatches,this,divstring,x,y,/*sortp*/false);
  for (i = 0; i < nmatches; i++) {
    index = matches[i];
    interval = &(this->intervals[0][index-1]);
    if (Interval_low(interval) == x && Interval_high(interval) == y &&
	Interval_type(interval) == type) {
      FREE(matches);
      return index;
    }
  }

  fprintf(stderr,"IIT_get_exact failed for %u %u %d\n",x,y,type);
  exit(9);

  FREE(matches);
  return -1;
}

int *
IIT_get_exact_multiple (int *nexactmatches, T this, char *divstring, unsigned int x, unsigned int y, int type) {
  int *exactmatches;
  int index;
  int *matches, nmatches, i, j;
  Interval_T interval;

  *nexactmatches = 0;
  matches = IIT_get(&nmatches,this,divstring,x,y,/*sortp*/false);
  for (i = 0; i < nmatches; i++) {
    index = matches[i];
    interval = &(this->intervals[0][index-1]);
    if (Interval_low(interval) == x && Interval_high(interval) == y &&
	Interval_type(interval) == type) {
      (*nexactmatches)++;
    }
  }

  if (*nexactmatches == 0) {
    fprintf(stderr,"IIT_get_exact_matches failed for %u %u %d\n",x,y,type);
    exit(9);
  } else {
    exactmatches = (int *) CALLOC(*nexactmatches,sizeof(int));
    j = 0;
    for (i = 0; i < nmatches; i++) {
      index = matches[i];
      interval = &(this->intervals[0][index-1]);
      if (Interval_low(interval) == x && Interval_high(interval) == y &&
	  Interval_type(interval) == type) {
	exactmatches[j++] = index;
      }
    }
    FREE(matches);
    return exactmatches;
  }
}

int *
IIT_get_exact_multiple_with_divno (int *nexactmatches, T this, int divno, unsigned int x, unsigned int y, int type) {
  int *exactmatches;
  int index;
  int *matches, nmatches, i, j;
  Interval_T interval;

  *nexactmatches = 0;
  matches = IIT_get_with_divno(&nmatches,this,divno,x,y,/*sortp*/false);
  for (i = 0; i < nmatches; i++) {
    index = matches[i];
    interval = &(this->intervals[0][index-1]);
    if (Interval_low(interval) == x && Interval_high(interval) == y &&
	Interval_type(interval) == type) {
      (*nexactmatches)++;
    }
  }

  if (*nexactmatches == 0) {
    fprintf(stderr,"IIT_get_exact_matches failed for %u %u %d\n",x,y,type);
    exit(9);
  } else {
    exactmatches = (int *) CALLOC(*nexactmatches,sizeof(int));
    j = 0;
    for (i = 0; i < nmatches; i++) {
      index = matches[i];
      interval = &(this->intervals[0][index-1]);
      if (Interval_low(interval) == x && Interval_high(interval) == y &&
	  Interval_type(interval) == type) {
	exactmatches[j++] = index;
      }
    }
    FREE(matches);
    return exactmatches;
  }
}


/************************************************************************/

#if 0
/* Need to work on */
/* Retrieves intervals from an IIT where type > 0.  Used by gmapindex to 
   construct altstrain_iit.  Here, the iit is a contig_iit.  */
List_T
IIT_intervallist_typed (List_T *labellist, Uintlist_T *seglength_list, T this) {
  List_T intervallist = NULL;
  Interval_T interval;
  char *label, *annotation, firstchar;
  bool allocp;
  int i;
  unsigned int seglength;

  *labellist = NULL;
  *seglength_list = NULL;
  for (i = 0; i < this->nintervals; i++) {
    interval = &(this->intervals[i]);
    if (Interval_type(interval) > 0) {
      intervallist = List_push(intervallist,Interval_copy(interval));
      label = IIT_label(this,i+1,&allocp);
      *labellist = List_push(*labellist,label);

      if (this->version <= 1) {
	/* Annotation may be negative to indicate contig is reverse complement */
	annotation = IIT_annotation(this,i+1,&allocp);
	firstchar = annotation[0];
	if (firstchar == '-') {
	  seglength = (unsigned int) strtoul(&(annotation[1]),NULL,10);
	} else {
	  seglength = (unsigned int) strtoul(annotation,NULL,10);
	  *seglength_list = Uintlist_push(*seglength_list,seglength);
	  if (allocp == true) {
	    FREE(annotation);
	  }
	}
      } else {
	seglength = (unsigned int) strtoul(annotation,NULL,10);
	*seglength_list = Uintlist_push(*seglength_list,seglength);
      }
    }
  }
  *labellist = List_reverse(*labellist);
  *seglength_list = Uintlist_reverse(*seglength_list);
  return List_reverse(intervallist);
}
#endif

List_T
IIT_typelist (T this) {
  List_T typelist = NULL;
  int i;
  char *typestring, *copy;

  for (i = 0; i < this->ntypes; i++) {
    typestring = IIT_typestring(this,i);
    copy = (char *) CALLOC(strlen(typestring)+1,sizeof(char));
    strcpy(copy,typestring);
    typelist = List_push(typelist,copy);
  }
  return List_reverse(typelist);
}
					  

/************************************************************************/

/* Note: Procedure call from get-genome.c needed to subtract 1 from
   position and then add 1 to chrpos */
char *
IIT_string_from_position (unsigned int *chrpos, unsigned int position, 
			  T chromosome_iit) {
  char *string, *chrstring;
  int index;
  bool allocp;

  index = IIT_get_one(chromosome_iit,/*divstring*/NULL,position,position);
  *chrpos = position - Interval_low(IIT_interval(chromosome_iit,index));
  chrstring = IIT_label(chromosome_iit,index,&allocp); 
  if (allocp == true) {
    return chrstring;
  } else {
    string = (char *) CALLOC(strlen(chrstring)+1,sizeof(char));
    strcpy(string,chrstring);
    return string;
  }
}


#if 0
/* Not used anymore */
/* Assume 0-based index */
static void
print_record (T this, int recno, bool map_bothstrands_p, char *chr, int level,
	      bool relativep, unsigned int left) {
#ifdef HAVE_64_BIT
  UINT8 start, end;
#else
  unsigned int start, end;
#endif
  unsigned int chrpos1, chrpos2;
  char *string;
  Interval_T interval;
  bool allocp;
#if 0
  int typeint;
#endif

#ifdef WORDS_BIGENDIAN
#ifdef HAVE_64_BIT
  if (this->version >= 4) {
    start = Bigendian_convert_uint8(this->annotpointers8[recno]);
    end = Bigendian_convert_uint8(this->annotpointers8[recno+1]);
  } else {
    start = Bigendian_convert_uint(this->annotpointers[recno]);
    end = Bigendian_convert_uint(this->annotpointers[recno+1]);
  }
#else
  start = Bigendian_convert_uint(this->annotpointers[recno]);
  end = Bigendian_convert_uint(this->annotpointers[recno+1]);
#endif
#else
#ifdef HAVE_64_BIT
  if (this->version >= 4) {
    start = this->annotpointers8[recno];
    end = this->annotpointers8[recno+1];
  } else {
    start = this->annotpointers[recno];
    end = this->annotpointers[recno+1];
  }
#else
  start = this->annotpointers[recno];
  end = this->annotpointers[recno+1];
#endif
#endif

  if (end <= start + 1U) {
    /* No annotation; use label */
    string = IIT_label(this,recno+1,&allocp);
  } else {
    string = IIT_annotation(this,recno+1,&allocp);
  }

  printf(">%s",this->name);
  if (level >= 0) {
    printf("\tlevel:%d",level);
  }

  interval = &(this->intervals[0][recno]);
  if (relativep == true) {
    if (Interval_sign(interval) >= 0) {
      printf("\t%u..%u",Interval_low(interval)-left,Interval_high(interval)-left);
    } else {
      printf("\t%u..%u",Interval_high(interval)-left,Interval_low(interval)-left);
    }
  } else {
    if (Interval_sign(interval) >= 0) {
      printf("\t%s:%u..%u",chr,Interval_low(interval),Interval_high(interval));
    } else {
      printf("\t%s:%u..%u",chr,Interval_high(interval),Interval_low(interval));
    }
  }

#if 0
  if (map_bothstrands_p == true) {
    if ((typeint = Interval_type(interval)) <= 0) {
      printf("\t\t%s",string);
    } else {
      printf("\t%s\t%s",IIT_typestring(this,typeint),string);
    }
  } else {
#endif
    printf("\t%s",string);
#if 0
  }
#endif
  if (string[strlen(string)-1] != '\n') {
    printf("\n");
  }

  if (allocp == true) {
    FREE(string);
  }

  return;
}
#endif


#if 0
/* Not used anymore */
void
IIT_print (T this, int *matches, int nmatches, bool map_bothstrands_p,
	   char *chr, int *levels, bool reversep, bool relativep, unsigned int left) {
  int recno, i;

  if (levels == NULL) {
    if (reversep == true) {
      for (i = nmatches-1; i >= 0; i--) {
	recno = matches[i] - 1;	/* Convert to 0-based */
	print_record(this,recno,map_bothstrands_p,chr,/*level*/-1,relativep,left);
      }
    } else {
      for (i = 0; i < nmatches; i++) {
	recno = matches[i] - 1;	/* Convert to 0-based */
	print_record(this,recno,map_bothstrands_p,chr,/*level*/-1,relativep,left);
      }
    }
  } else {
    if (reversep == true) {
      for (i = nmatches-1; i >= 0; i--) {
	recno = matches[i] - 1;	/* Convert to 0-based */
	print_record(this,recno,map_bothstrands_p,chr,levels[i],relativep,left);
      }
    } else {
      for (i = 0; i < nmatches; i++) {
	recno = matches[i] - 1;	/* Convert to 0-based */
	print_record(this,recno,map_bothstrands_p,chr,levels[i],relativep,left);
      }
    }
  }

  return;
}
#endif



/* Assume 0-based index */
static void
print_header (T this, int recno, char *chr, bool map_bothstrands_p,
	      bool relativep, unsigned int left) {
  unsigned int chrpos1, chrpos2;
  char *string, *chrstring1;
  Interval_T interval;
  bool allocp;
#if 0
  int typeint;
#endif

  string = IIT_label(this,recno+1,&allocp);

  printf("\t%s",this->name);

  interval = &(this->intervals[0][recno]);
  if (relativep == true) {
    if (Interval_sign(interval) >= 0) {
      printf("\t%u..%u",Interval_low(interval)-left,Interval_high(interval)-left);
    } else {
      printf("\t%u..%u",Interval_high(interval)-left,Interval_low(interval)-left);
    }
  } else {
    if (Interval_sign(interval) >= 0) {
      printf("\t%s:%u..%u",chr,Interval_low(interval),Interval_high(interval));
    } else {
      printf("\t%s:%u..%u",chr,Interval_high(interval),Interval_low(interval));
    }

    /* FREE(chrstring1); */
  }

#if 0
  if (map_bothstrands_p == true) {
    if ((typeint = Interval_type(interval)) <= 0) {
      printf("\t\t%s",string);
    } else {
      printf("\t%s\t%s",IIT_typestring(this,typeint),string);
    }
  } else {
#endif
    printf("\t%s",string);
#if 0
  }
#endif
  if (string[strlen(string)-1] != '\n') {
    printf("\n");
  }

  if (allocp == true) {
    FREE(string);
  }

  return;
}


void
IIT_print_header (T this, int *matches, int nmatches, bool map_bothstrands_p,
		  char *chr, bool reversep, bool relativep, unsigned int left) {
  int recno, i;

  if (reversep == true) {
    for (i = nmatches-1; i >= 0; i--) {
      recno = matches[i] - 1;	/* Convert to 0-based */
      print_header(this,recno,chr,map_bothstrands_p,relativep,left);
    }
  } else {
    for (i = 0; i < nmatches; i++) {
      recno = matches[i] - 1;	/* Convert to 0-based */
      print_header(this,recno,chr,map_bothstrands_p,relativep,left);
    }
  }

  return;
}


