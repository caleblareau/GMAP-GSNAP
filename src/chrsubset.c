static char rcsid[] = "$Id: chrsubset.c,v 1.5 2005/05/04 18:49:01 twu Exp $";

#include "chrsubset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>		/* For strlen */
#include <ctype.h>		/* For isspace */
#include "mem.h"


#ifdef DEBUG
#define debug(x) x
#else
#define debug(x)
#endif


#define T Chrsubset_T
struct T {
  char *name;
  bool *includep;
};


void
Chrsubset_print (T this) {
  if (this != NULL && this->name != NULL) {
    printf("    Chromosome subset: %s\n",this->name);
  }
  return;
}


bool
Chrsubset_includep (T this, Genomicpos_T position, IIT_T chromosome_iit) {
  int index;

  if (this == NULL) {
    return true;
  } else {
    index = IIT_get_one(chromosome_iit,position,position);
    return this->includep[index-1];
  }
}


void
Chrsubset_free (T *old) {
  if (*old != NULL) {
    if ((*old)->name != NULL) {
      FREE((*old)->name);
    }
    FREE((*old)->includep);
    FREE(*old);
  }
  return;
}

#define BUFSIZE 2048

static char *
read_header (FILE *fp, char *filename) {
  char *subsetname, Buffer[BUFSIZE], *p, *start, *end;

  Buffer[0] = '\0';

  while (Buffer[0] == '\0' || isspace(Buffer[0])) {
    /* Read past all empty lines */
    if (fgets(Buffer,BUFSIZE,fp) == NULL) {
      return NULL;
    }
  }
  if (Buffer[0] != '>') {
    fprintf(stderr,"In chromosome subset file %s, a '>' line was expected\n",filename);
    exit(9);
  } else {
    p = &(Buffer[1]);
    if (*p == '\0' || isspace(*p)) {
      fprintf(stderr,"The '>' line in chromosome subset file %s is invalid\n",filename);
      exit(9);
    } else {
      start = p;
      while (*p != '\0' && !isspace(*p)) {
	p++;
      }
      end = p;

      subsetname = (char *) CALLOC((size_t) (end - start +1),sizeof(char));
      strncpy(subsetname,start,(size_t) (end - start));
      debug(printf("Read header %s\n",subsetname));
      return subsetname;
    }
  }
}

static void
skip_list (FILE *fp, char *filename, char *subsetname) {
  char Buffer[BUFSIZE];

  if (fgets(Buffer,BUFSIZE,fp) == NULL) {
    fprintf(stderr,"In %s, expected a line after >%s\n",filename,subsetname);
    exit(9);
  }
  return;
}


static bool *
include_all (IIT_T chromosome_iit) {
  bool *includep;
  int nchromosomes, i;

  nchromosomes = IIT_nintervals(chromosome_iit);
  includep = (bool *) CALLOC(nchromosomes,sizeof(bool));
  for (i = 0; i < nchromosomes; i++) {
    includep[i] = true;
  }
  return includep;
}


static char *
get_next_token (char **string) {
  char *token, *start, *end, *p;

  p = *string;
  while (*p != '\0' && (isspace(*p) || *p == ',')) {
    p++;
  }
  if (*p == '\0') {
    *string = p;
    return NULL;
  } else {
    start = p;
  }

  while (*p != '\0' && !isspace(*p) && *p != ',') {
    p++;
  }
  end = p;

  token = (char *) CALLOC((size_t) (end - start + 1),sizeof(char));
  strncpy(token,start,(size_t) (end - start));
  *string = p;
  return token;
}



static bool *
process_inclusions (char *string, IIT_T chromosome_iit) {
  bool *includep;
  int nchromosomes, index, i;
  char *chrstring;

  /* Default is to exclude */
  nchromosomes = IIT_nintervals(chromosome_iit);
  includep = (bool *) CALLOC(nchromosomes,sizeof(bool));
  for (i = 0; i < nchromosomes; i++) {
    includep[i] = false;
  }

  while ((chrstring = get_next_token(&string)) != NULL) {
    debug(printf("Token is %s\n",chrstring));
    index = IIT_find_one(chromosome_iit,chrstring);
    includep[index-1] = true;
    FREE(chrstring);
  }
  return includep;
}

static bool *
process_exclusions (char *string, IIT_T chromosome_iit) {
  bool *includep;
  int nchromosomes, index, i;
  char *chrstring;

  /* Default is to include */
  nchromosomes = IIT_nintervals(chromosome_iit);
  includep = (bool *) CALLOC(nchromosomes,sizeof(bool));
  for (i = 0; i < nchromosomes; i++) {
    includep[i] = true;
  }

  while ((chrstring = get_next_token(&string)) != NULL) {
    debug(printf("Token is %s\n",chrstring));
    index = IIT_find_one(chromosome_iit,chrstring);
    includep[index-1] = false;
    FREE(chrstring);
  }
  return includep;
}


static bool *
process_list (FILE *fp, char *filename, char *subsetname, IIT_T chromosome_iit) {
  bool *includep;
  char Buffer[BUFSIZE], *p;

  if (fgets(Buffer,BUFSIZE,fp) == NULL) {
    fprintf(stderr,"In %s, expected a line after >%s\n",filename,subsetname);
    exit(9);
  } else {
    p = Buffer;
    while (*p != '\0' && isspace(*p)) {
      p++;
    }
    if (*p == '\0') {
      /* Blank line.  Interpret to mean inclusion of everything */
      return (bool *) NULL;

    } else if (*p == '+') {
      /* Skip blanks */
      p++;
      return process_inclusions(p,chromosome_iit);

    } else if (*p == '-') {
      /* Skip blanks */
      p++;
      while (*p != '\0' && !isspace(*p)) {
	p++;
      }
      return process_exclusions(p,chromosome_iit);

    } else {
      fprintf(stderr,"In %s, don't know how to interpret this line:\n%s\n",
	      filename,Buffer);
      exit(9);
    }
  }
}



T
Chrsubset_read (char *user_chrsubsetfile, char *genomesubdir, char *fileroot, 
		char *user_chrsubsetname, IIT_T chromosome_iit) {
  T new = NULL;
  FILE *fp;
  char *filename, *subsetname;
  bool *includep;
#ifdef DEBUG
  int i;
#endif

  if (user_chrsubsetfile != NULL) {
    filename = (char *) CALLOC(strlen(user_chrsubsetfile)+1,sizeof(char));
    strcpy(filename,user_chrsubsetfile);
    fp = fopen(filename,"r");
    if (fp == NULL) {
      fprintf(stderr,"The provided file chromosome subset file %s could not be read\n",
	      filename);
      exit(9);
    }

  } else {
    filename = (char *) CALLOC(strlen(genomesubdir)+strlen("/")+strlen(fileroot)+
			       strlen(".chrsubset")+1,sizeof(char));
    sprintf(filename,"%s/%s.chrsubset",genomesubdir,fileroot);
    fp = fopen(filename,"r");
  }

  if (fp == NULL) {
    debug(printf("Standard file doesn't exist.  Including all chromosomes\n"));
    new = (T) NULL;

  } else if (user_chrsubsetname == NULL) {
    if ((subsetname = read_header(fp,filename)) == NULL) {
      fprintf(stderr,"The chromosome subset file %s is empty\n",filename);
      exit(9);
    }

    if ((includep = process_list(fp,filename,subsetname,chromosome_iit)) == NULL) {
      new = (T) NULL;
      FREE(subsetname);
    } else {
      new = (T) MALLOC(sizeof(*new));
      new->name = subsetname;
      new->includep = includep;
    }

    fclose(fp);
    debug(printf("User didn't specify a subset.  Using first list: %s\n",subsetname));
    
  } else {
    debug(printf("User specified subset %s\n",user_chrsubsetname));
    while ((subsetname = read_header(fp,filename)) != NULL &&
	   strcmp(subsetname,user_chrsubsetname)) {
      debug(printf("Skipping %s\n",subsetname));
      skip_list(fp,filename,subsetname);
      FREE(subsetname);
    }
    if (subsetname == NULL) {
      fprintf(stderr,"Unable to find subset %s in chromosome subset file %s\n",
	      user_chrsubsetname,filename);
      exit(9);
    } else if ((includep = process_list(fp,filename,subsetname,chromosome_iit)) == NULL) {
      new = (T) NULL;
      FREE(subsetname);
    } else {
      new = (T) MALLOC(sizeof(*new));
      new->name = subsetname;
      new->includep = includep;
    }
    fclose(fp);
  }
  FREE(filename);

  debug(
	if (new != NULL) {
	  for (i = 0; i < IIT_nintervals(chromosome_iit); i++) {
	    printf(" %d: %d\n",i,new->includep[i]);
	  }
	}
	);

  return new;
}

