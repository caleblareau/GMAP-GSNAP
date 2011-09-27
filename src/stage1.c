static char rcsid[] = "$Id: stage1.c,v 1.208 2010-07-10 15:23:23 twu Exp $";
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "stage1.h"
#include <stdio.h>
#include <stdlib.h>
#include "bool.h"
#include "mem.h"
#include "reader.h"
#include "block.h"
#ifdef PMAP
#include "oligop.h"
#else
#include "oligo.h"
#endif
#include "indexdb.h"
#include "indexdb_hr.h"
#include "intlist.h"
#include "listdef.h"
#include "matchdef.h"
#include "match.h"
#include "chrsubset.h"
#include "gregion.h"


#define MAX_INDELS 15
#define MIN_REPEAT 6
#define MAXENTRIES 100
#define MIN_MATCH_WEIGHT 0.05	/* For connectablep */

#define SUFFICIENT_FIRST_WEIGHT 0.50

#define PCT_MAX_SUPPORT 0.70
#define DIFF_MAX_SUPPORT 200
#define BOUNDARY_SUPPORT 666 	/* DIFF_MAXSUPPORT/(1-PCT_MAX_SUPPORT) */

#ifdef PMAP
#define VERYSHORTSEQLEN 30
#define SHORTSEQLEN 90
#define SINGLEEXONLENGTH 30	/* per nt */
#define SLOPE 2400		/* genomic bp per aa */
#define NOEXTENDLEN 6		/* in aa */
#else
#define VERYSHORTSEQLEN 60
#define SHORTSEQLEN 90
#define SINGLEEXONLENGTH 90	/* per nt */
#define SLOPE 800		/* genomic bp per nt */
#define NOEXTENDLEN 6		/* in nt */
#endif

#define HIGHINTRONLENGTH 100000U /* Used in find_best_path */

#define INTRONLEN_PENALTY 10000	/* Used in find_best_path on diagonal segments.  1 point for each length */

#define MAX_ATTEMPTS_UNIT 100 /* This parameter is used to decide when to switch from 24-mers to 18-mers */

#define MAX_FILL_IN 200
#define MAX_DANGLING_PCT 0.33

#define SAMPLERUN 50

#define COLLAPSE_DISTANCE 12
#define SUBOPTIMAL 36
#define NBEST 100

#define MAX_GREGIONS_PRE_UNIQUE 1000
#define MAX_GREGIONS_POST_UNIQUE 100

#define BINARY_FOLDDIFF 8

/* Once a match at a genomic location has PROMISCUOUS matches locally,
   it is unlikely that further matches will help define that candidate
   segment.  Allowing PROMISCUOUS to be greater than 1 allows more
   candidate segments to be passed to stage 2.  However, if
   PROMISCUOUS is set too high, then a single pair of matches may make
   the algorithm miss the correct spot.  For example, PROMISCUOUS of 9
   or greater will miss the spliced form of BM994949, on chr4, and get
   a pseudogene that contains a poly-T on chr15. */

#define PROMISCUOUS 4

/* Debugging of scanning for 24-mers */
#ifdef DEBUG
#define debug(x) x
static IIT_T global_chromosome_iit;
static Genome_T global_genome;
static char *queryuc_ptr;
#else
#define debug(x)
#endif

/* show positions.  Should also specify DEBUG */
#ifdef DEBUG1
#define debug1(x) x
#else
#define debug1(x)
#endif

/* Debugging of 12-mer extension at ends */ 
#ifdef DEBUG2
#define debug2(x) x
#else
#define debug2(x)
#endif

/* Detailed view of scanning for 24-mers */
#ifdef DEBUG3
#define debug3(x) x
#else
#define debug3(x)
#endif

/* Double/triple matching, including binary search */
#ifdef DEBUG4
#define debug4(x) x
#else
#define debug4(x)
#endif

/* connectable_p */
#ifdef DEBUG5
#define debug5(x) x
#else
#define debug5(x)
#endif


/* collapse_diagonals and find_segments */
#ifdef DEBUG6
#define debug6(x) x
#else
#define debug6(x)
#endif

/* find_segments_heap */
#ifdef DEBUG7
#define debug7(x) x
#else
#define debug7(x)
#endif

/* compute_paths */
#ifdef DEBUG8
#define debug8(x) x
#else
#define debug8(x)
#endif

/* find_good_paths */
#ifdef DEBUG9
#define debug9(x) x
#else
#define debug9(x)
#endif



#define T Stage1_T
struct T {
  int trimstart;
  int trimend;
  int trimlength;
  int maxtotallen;
  int querylength;
  int maxentries;

  int oligosize;

  Block_T block5;
  Block_T block3;
  List_T matches5;
  List_T matches3;
  Genomicpos_T **plus_positions;
  Genomicpos_T **minus_positions;
  int *plus_npositions;
  int *minus_npositions;
  bool *plus_matchedp;		/* For identify_matches */
  bool *minus_matchedp;
  bool *processedp;		/* For Block_process_oligo */
  bool *validp;			/* For Block_process_oligo */
  int querystart;		/* Changes during computation */
  int queryend;			/* Changes during computation */

#ifdef PMAP
  Storedoligomer_T *oligos;
#else
  Storedoligomer_T *forward_oligos;
  Storedoligomer_T *revcomp_oligos;
#endif
};


#ifdef CHECK
static void
Stage1_check (T this) {
  int i, npositions, j;
  
  for (i = 0; i < this->querylength; i++) {
    npositions = this->plus_npositions[i];
    if (npositions == 0 && this->plus_positions[i] != NULL) {
      fprintf(stderr,"npositions = 0, this->plus_positions[i] != NULL\n");
      abort();
    } else if (npositions != 0 && this->plus_positions[i] == NULL) {
      fprintf(stderr,"npositions != 0, this->plus_positions[i] == NULL\n");
      abort();
    } else {
      for (j = 0; j < npositions; j++) {
	if (this->plus_positions[i][j] == 0U) {
	  fprintf(stderr,"this->plus_positions[i][i] == 0\n");
	  abort();
	}
      }
    }

    npositions = this->minus_npositions[i];
    if (npositions == 0 && this->minus_positions[i] != NULL) {
      fprintf(stderr,"npositions = 0, this->minus_positions[i] != NULL\n");
      abort();
    } else if (npositions != 0 && this->minus_positions[i] == NULL) {
      fprintf(stderr,"npositions != 0, this->minus_positions[i] == NULL\n");
      abort();
    } else {
      for (j = 0; j < npositions; j++) {
	if (this->minus_positions[i][j] == 0U) {
	  fprintf(stderr,"this->minus_positions[i][i] == 0\n");
	  abort();
	}
      }
    }
  }

  return;

}
#endif



static T
Stage1_new (Sequence_T queryuc, int maxtotallen, int matchsize, int maxentries) {
  T new = (T) MALLOC(sizeof(*new));
  Reader_T reader;
  int leftreadshift;
  
#ifdef PMAP
  new->querylength = Sequence_fulllength_given(queryuc);
#else
  new->querylength = Sequence_fulllength(queryuc);
#endif

  new->querystart = Sequence_trim_start(queryuc);
  new->queryend = Sequence_trim_end(queryuc);
  new->trimstart = new->querystart;
  new->trimend = new->queryend;
  new->trimlength = Sequence_trimlength(queryuc);

  new->maxtotallen = maxtotallen;
  new->maxentries = maxentries;
#ifdef PMAP
  new->oligosize = INDEX1PART_AA;
#else
  new->oligosize = INDEX1PART;
#endif

  reader = Reader_new(Sequence_fullpointer(queryuc),new->querystart,new->queryend,
		      /*reader_overlap*/new->oligosize,/*dibasep*/false);
#if 0
  debug(Sequence_print(queryuc,/*uppercasep*/true,/*wraplength*/50,/*trimmedp*/true));
#endif
#ifdef PMAP
  new->block5 = Block_new(FIVE,reader,3*new->querylength);
  new->block3 = Block_new(THREE,reader,3*new->querylength);
#else
  leftreadshift = 32 - 2*new->oligosize; /* chars are shifted into left of a 32 bit word */
  new->block5 = Block_new(FIVE,new->oligosize,leftreadshift,reader,new->querylength);
  new->block3 = Block_new(THREE,new->oligosize,leftreadshift,reader,new->querylength);
#endif
  new->matches5 = NULL;
  new->matches3 = NULL;

  new->plus_positions = (Genomicpos_T **) CALLOC(new->querylength,sizeof(Genomicpos_T *));
  new->minus_positions = (Genomicpos_T **) CALLOC(new->querylength,sizeof(Genomicpos_T *));
  new->plus_npositions = (int *) CALLOC(new->querylength,sizeof(int));
  new->minus_npositions = (int *) CALLOC(new->querylength,sizeof(int));
  new->processedp = (bool *) CALLOC(new->querylength,sizeof(bool));

  new->plus_matchedp = (bool *) CALLOC(new->querylength,sizeof(bool));
  new->minus_matchedp = (bool *) CALLOC(new->querylength,sizeof(bool));

  new->validp = (bool *) NULL;
#ifdef PMAP
  new->oligos = (Storedoligomer_T *) NULL;
#else
  new->forward_oligos = (Storedoligomer_T *) NULL;
  new->revcomp_oligos = (Storedoligomer_T *) NULL;
#endif

  return new;
}

static void
Stage1_free (T *old) {
  Reader_T reader;
  int i;

  /* Stage1_check(*old); */

  if ((*old)->validp != NULL) {
    FREE((*old)->validp);
#ifdef PMAP
    FREE((*old)->oligos);
#else
    FREE((*old)->revcomp_oligos);
    FREE((*old)->forward_oligos);
#endif
  }

  for (i = 0; i < (*old)->querylength; i++) {
    if ((*old)->plus_positions[i] != NULL) {
      FREE((*old)->plus_positions[i]);
    }
    if ((*old)->minus_positions[i] != NULL) {
      FREE((*old)->minus_positions[i]);
    }
  }
  
  FREE((*old)->plus_positions);
  FREE((*old)->minus_positions);
  FREE((*old)->plus_npositions);
  FREE((*old)->minus_npositions);
  FREE((*old)->processedp);

  FREE((*old)->plus_matchedp);
  FREE((*old)->minus_matchedp);

  reader = Block_reader((*old)->block5);
  Reader_free(&reader);

  Block_free(&(*old)->block3);
  Block_free(&(*old)->block5);

  FREE(*old);
  return;
}



static bool
connectable_p (Match_T match5, Match_T match3, int maxtotallen, int matchinterval) {
  Genomicpos_T position5, position3;
  int querypos5, querypos3, exonlen;
  bool forwardp5, forwardp3;

  debug5(printf("Comparing #%d:%u at querypos %d with #%d:%u at querypos %d\n",
		Match_chrnum(match5),Match_chrpos(match5),Match_querypos(match5),
		Match_chrnum(match3),Match_chrpos(match3),Match_querypos(match3)));

  if (Match_chrnum(match5) != Match_chrnum(match3)) {
    debug5(printf("No, different chromosomes\n\n"));
    return false;
  } else {
    querypos5 = Match_querypos(match5);
    querypos3 = Match_querypos(match3);
    exonlen = querypos3 - querypos5;
#if 0
    if (exonlen < matchinterval) {
      debug5(printf("No, exonlen == 0\n\n"));
      return false;
    } else {
#endif
      position5 = Match_position(match5);
      position3 = Match_position(match3);
      /* intronlen = abs(position3 - position5) - exonlen; -- shouldn't subtract unsigned ints */
      if (position3 > position5) {
	/* intronlen = position3 - position5 - exonlen; -- Don't subtract into a signed int */
	/* The check below is equivalent to intronlen > maxtotallen */
	if (position3 > (Genomicpos_T) maxtotallen + position5 + (Genomicpos_T) exonlen) {
	  debug5(printf("No, intron too long (%u > %u + %u + %u)\n\n",
			position3,maxtotallen,position5,exonlen));
	  return false;
	}
      } else {
	/* intronlen = position5 - position3 - exonlen; -- Don't subtract into a signed int */
	/* The check below is equivalent to intronlen > maxtotallen */
	if (position5 > (Genomicpos_T) maxtotallen + position3 + (Genomicpos_T) exonlen) {
	  debug5(printf("No, intron too long (%u > %u + %u + %u)\n\n",
			position5,maxtotallen,position3,exonlen));
	  return false;
	}
      }
      forwardp5 = Match_forwardp(match5);
      forwardp3 = Match_forwardp(match3);

      if (forwardp5 != forwardp3) {
	debug5(printf("No, forwardp different\n\n"));
	return false;
      } else if (forwardp5 == true && position3 < position5) {
	debug5(printf("No, forwardp true, but positions wrong\n\n"));
	return false;
      } else if (forwardp5 == false && position5 < position3) {
	debug5(printf("No, forwardp false, but positions wrong\n\n"));
	return false;
      } else if (Match_weight(match5) < MIN_MATCH_WEIGHT || Match_weight(match3) < MIN_MATCH_WEIGHT) {
	debug5(printf("Yes, but weights are too small: %.1f %.1f\n\n",
		      Match_weight(match5),Match_weight(match3)));
	return false;
      } else {
	debug5(printf("Yes\n\n"));
	return true;
      }
#if 0
    }
#endif
  }
}



/* Updates a list of Stage1_T objects */
static List_T
pair_up (bool *foundpairp, List_T gregionlist, int matchsize,
	 List_T newmatches5, List_T newmatches3, 
	 List_T matches5, List_T matches3, IIT_T chromosome_iit, int querylength,
	 int maxtotallen, int trimstart, int trimend, int trimlength) {
  List_T p, q, s, new_gregions = NULL;
  Match_T match5, match3;
  int matchinterval;
#ifdef DEBUG
  Gregion_T gregion;
#endif

#ifdef PMAP
  matchinterval = matchsize - INDEX1PART_AA;
#else
  matchinterval = matchsize - INDEX1PART;
#endif

  /* Do N vs N */
  for (q = newmatches5; q != NULL; q = q->rest) {
    if (Match_npairings(match5 = q->first) < PROMISCUOUS) {
      for (s = newmatches3; s != NULL; s = s->rest) {
	if (Match_npairings(match3 = s->first) < PROMISCUOUS) {
	  if (connectable_p(match5,match3,maxtotallen,matchinterval)) {
#if 1
	    if (Match_acceptable_pair(match5,match3,trimlength,matchsize) == true) {
#endif
	      new_gregions = List_push(new_gregions,Gregion_new_from_matches(match5,match3,chromosome_iit,
									     querylength,matchsize,trimstart,trimend));
#if 1
	    }
#endif
	  }
	}
      }
    }
  }

  /* Do N vs (N-1..1) */
  for (q = newmatches5; q != NULL; q = q->rest) {
    if (Match_npairings(match5 = q->first) < PROMISCUOUS) {
      for (s = matches3; s != NULL; s = s->rest) {
	if (Match_npairings(match3 = s->first) < PROMISCUOUS) {
	  if (connectable_p(match5,match3,maxtotallen,matchinterval)) {
#if 1
	    if (Match_acceptable_pair(match5,match3,trimlength,matchsize) == true) {
#endif
	      new_gregions = List_push(new_gregions,Gregion_new_from_matches(match5,match3,chromosome_iit,
									     querylength,matchsize,trimstart,trimend));
#if 1
	    }
#endif
	  }
	}
      }
    }
  }

  /* Do (N-1..1) vs N */
  for (q = matches5; q != NULL; q = q->rest) {
    if (Match_npairings(match5 = q->first) < PROMISCUOUS) {
      for (s = newmatches3; s != NULL; s = s->rest) {
	if (Match_npairings(match3 = s->first) < PROMISCUOUS) {
	  if (connectable_p(match5,match3,maxtotallen,matchinterval)) {
#if 1
	    if (Match_acceptable_pair(match5,match3,trimlength,matchsize) == true) {
#endif
	      new_gregions = List_push(new_gregions,Gregion_new_from_matches(match5,match3,chromosome_iit,
									     querylength,matchsize,trimstart,trimend));
#if 1
	    }
#endif
	  }
	}
      }
    }
  }

  if (new_gregions == NULL) {
    debug(printf("--No new gregions found\n"));
    *foundpairp = false;

  } else {
    debug(printf("--%d new gregions found before uniq\n",List_length(new_gregions)));
    debug(
	  for (p = new_gregions; p != NULL; p = p->rest) {
	    gregion = (Gregion_T) List_head(p);
	    printf("Before uniq: ");
	    Gregion_print(gregion);
	  }
	  );

    new_gregions = Gregion_filter_unique(new_gregions);

    debug(printf("--%d new gregions found after uniq\n",List_length(new_gregions)));
    *foundpairp = true;
    for (p = new_gregions; p != NULL; p = p->rest) {
	debug(
	      gregion = (Gregion_T) List_head(p);
	      printf("After uniq: ");
	      Gregion_print(gregion);
	      );
	gregionlist = List_push(gregionlist,(Gregion_T) List_head(p));
    }
    List_free(&new_gregions);
  }

  return gregionlist;
}


#if 0
static bool
repetitivep (Storedoligomer_T oligo, int oligosize) {
  int i;

  oligos = (Storedoligomer_T *) CALLOC(this->querylength,sizeof(Storedoligomer_T));
  for (querypos = 0; querypos <= this->querylength - this->oligobase; querypos++) {
    if (this->validp[querypos] == true) {
      oligos[k++] = this->forward_oligos[querypos];
    }
  }
  qsort(oligos,k,sizeof(Storedoligomer_T),Storedoligomer_compare);

  for (i = 0, j = 1; j < k; i++, j++) {
    if (oligos[j] == oligos[i]) {
      debug(printf("Found repetition of oligo %06X == %06X\n",oligos[i],oligos[j]));
      FREE(oligos);
      return true;
    }
  }

  FREE(oligos);
  return false;
}
#endif


static List_T
identify_singles (int *nnew, bool *overflowp, List_T matches, int merstart, Genomicpos_T positionadj,
		  int querylength, Genomicpos_T *positions, int npositions, 
		  int matchsize, IIT_T chromosome_iit, 
		  Chrsubset_T chrsubset, Matchpool_T matchpool,
		  bool forwardp, bool fivep, int maxentries) {
  List_T newmatches = NULL, p;
  Match_T match;
  Genomicpos_T position;
  int i = 0, nentries = 0;
  bool donep = false;
  double weight;

  if (npositions == 0) {
    *nnew = 0;
    *overflowp = false;
    return matches;
  } else {
    position = positions[0];
  }    

  Matchpool_save(matchpool);

  while (donep == false) {
    if (!chrsubset || Chrsubset_includep(chrsubset,position,chromosome_iit) == true) {
      if (++nentries > maxentries) {
	donep = true;
      } else {
	newmatches = Matchpool_push(newmatches,matchpool,merstart,querylength,forwardp,fivep,
				    position+positionadj,chromosome_iit);
      }
    }

    /* Advance */
    if (++i >= npositions) {
      donep = true;
    } else {
      position = positions[i];
    }
  }

  if (nentries > maxentries) {
    /* Too many entries.  Not unique enough in genome */
    *nnew = 0;
    *overflowp = true;
    debug(printf("  Singles overflow at %d\n",merstart));

    /* Not necessary to free */
    Matchpool_restore(matchpool);

  } else {
    *nnew = nentries;
    *overflowp = false;
    if (nentries > 0) {
      weight = 1.0/(double) nentries;
      for (p = newmatches; p != NULL; p = p->rest) {
	match = (Match_T) p->first;
	Match_set_weight(match,weight);

	debug(
	      Match_print(match,chromosome_iit);
	      Match_print_mer(match,queryuc_ptr,global_genome,chromosome_iit,matchsize);
	      printf("\n");
	      );
      }
    }
    matches = Matchpool_transfer(matches,newmatches);
  }

  return matches;
}


static int
binary_search (int lowi, int highi, Genomicpos_T *positions, Genomicpos_T goal) {
  bool foundp = false;
  int middlei;

#ifdef NOBINARY
  return lowi;
#endif

  if (goal == 0U) {
    return lowi;
  }

  while (!foundp && lowi < highi) {
    middlei = (lowi+highi)/2;
    debug4(printf("  binary: %d:%u %d:%u %d:%u   vs. %u\n",
		      lowi,positions[lowi],middlei,positions[middlei],
		      highi,positions[highi],goal));
    if (goal < positions[middlei]) {
      highi = middlei;
    } else if (goal > positions[middlei]) {
      lowi = middlei + 1;
    } else {
      foundp = true;
    }
  }

  if (foundp == true) {
    debug4(printf("returning %d\n\n",middlei));
    return middlei;
  } else {
    debug4(printf("returning %d\n\n",highi));
    return highi;
  }
}


/* positions0 should be left of positions1, and expecteddist should be
 * positive.  This procedure assumes that the entries in position0 and
 * positions1 are in order.  They are assumed not to have duplicates, which
 * are removed by Indexdb_write_positions and Indexdb_read. */
static List_T
identify_doubles (int *nnew, bool *overflowp, List_T matches, int merstart, Genomicpos_T positionadj,
		  int querylength, Genomicpos_T *positions0, int npositions0, 
		  Genomicpos_T *positions1, int npositions1,
		  int matchsize, IIT_T chromosome_iit, 
		  Chrsubset_T chrsubset, Matchpool_T matchpool,
		  bool forwardp, bool fivep, int maxentries) {
  List_T newmatches = NULL, p;
  Match_T match;
  Genomicpos_T expected0, position0, expected1, position1;
  int i = 0, j = 0, nentries = 0;
  bool donep = false;
  double weight;

  /*
  debug(printf("Entering identify_doubles with merstart = %d, positionadj = %u, expecteddist = %d\n",
	       merstart,positionadj,expecteddist));
  */
  if (npositions0 == 0) {
    /* debug(printf("Leaving identify_doubles because npositions0 == 0\n\n")); */
    *nnew = 0;
    *overflowp = false;
    return matches;
  } else {
    position0 = positions0[0];
    expected1 = position0 /*+ expecteddist*/;
    if (npositions1 == 0) {
      /* debug(printf("Leaving identify_doubles because npositions1 == 0\n\n")); */
      *nnew = 0;
      *overflowp = false;
      return matches;
    } else {
      position1 = positions1[0];
#if 0
      if (position1 < expecteddist) {
	expected0 = 0U;
      } else {
#endif
	expected0 = position1 /*- expecteddist*/;
#if 0
      }
#endif
    }
  }

  Matchpool_save(matchpool);

  while (donep == false) {
    debug4(printf("  %d:%u %d:%u\n",i,position0,j,position1));
    debug4(
	   if (abs(position1-position0) < 100) {
	     printf("Close: %u %u\n",position0,position1);
	   }
	  );

    if (expected1 < position1) {
      /* Advance position0 */
      if (npositions0 - i < BINARY_FOLDDIFF*(npositions1 - j)) {
	i++;
      } else {
	debug4(printf("  remaining positions: %d and %d => binary\n",npositions0-i,npositions1-j));
	i = binary_search(i+1,npositions0,positions0,expected0);
      }
      if (i >= npositions0) {
	donep = true;
      } else {
	position0 = positions0[i];
	expected1 = position0 /*+ expecteddist*/;
      }

    } else if (expected1 > position1) {
      /* Advance position1 */
      if (npositions1 - j < BINARY_FOLDDIFF*(npositions0 - i)) {
	j++;
      } else {
	debug4(printf("  remaining positions: %d and %d => binary\n",npositions0-i,npositions1-j));
	j = binary_search(j+1,npositions1,positions1,expected1);
      }
      if (j >= npositions1) {
	donep = true;
      } else {
	position1 = positions1[j];
#if 0
	if (position1 < expecteddist) {
	  expected0 = 0U;
	} else {
#endif
	  expected0 = position1 /*- expecteddist*/;
#if 0
	}
#endif
      }

    } else {
      if (!chrsubset || Chrsubset_includep(chrsubset,position0,chromosome_iit) == true) {
	if (++nentries > maxentries) {
	  donep = true;
	} else {
	  newmatches = Matchpool_push(newmatches,matchpool,merstart,querylength,forwardp,fivep,
				      position0+positionadj,chromosome_iit);
	}
      }

      /* Advance both */
      if (++i >= npositions0) {
	donep = true;
      } else {
	position0 = positions0[i];
	expected1 = position0 /*+ expecteddist*/;
	if (++j >= npositions1) {
	  donep = true;
	} else {
	  position1 = positions1[j];
	  expected0 = position1 /*- expecteddist*/;
	}
      }
    }
  }

  if (nentries > maxentries) {
    /* Too many entries.  Not unique enough in genome */
    *nnew = 0;
    *overflowp = true;
    /* debug(printf("  Doubles overflow at %d\n\n",querypos1)); */

    /* Not necessary to free */
    Matchpool_restore(matchpool);

  } else {
    /* debug(printf("Leaving identify_doubles with %d matches\n\n",List_length(newmatches))); */
    *nnew = nentries;
    *overflowp = false;
    if (nentries > 0) {
      weight = 1.0/(double) nentries;

      debug(if (newmatches != NULL) {
	  printf("Entering identify_doubles with merstart = %d, positionadj = %u\n",
		 merstart,positionadj);
	});

      for (p = newmatches; p != NULL; p = p->rest) {
	match = (Match_T) p->first;
	Match_set_weight(match,weight);
	debug(
	      Match_print(match,chromosome_iit);
	      Match_print_mer(match,queryuc_ptr,global_genome,chromosome_iit,matchsize);
	      printf("\n");
	      );
      }
    }
    matches = Matchpool_transfer(matches,newmatches);
  }

  return matches;
}


/* Involves search of three lists.  This procedure assumes that the
 * entries in positions0, positions1, and positions2 are in order.  They are assumed
 * not to have duplicates, which are removed by
 * Indexdb_write_positions and Indexdb_read. */
static List_T
identify_triples (int *nnew, bool *overflowp, List_T matches, int merstart, Genomicpos_T positionadj,
		  int querylength, Genomicpos_T *positions0, int npositions0, 
		  Genomicpos_T *positions1, int npositions1, Genomicpos_T *positions2, int npositions2,
		  int matchsize, int expecteddist1, int expecteddist2, IIT_T chromosome_iit, 
		  Chrsubset_T chrsubset, Matchpool_T matchpool,
		  bool forwardp, bool fivep, int maxentries) {
  List_T newmatches = NULL, p;
  Match_T match;
  Genomicpos_T position0, expected1, position1, expected2, position2;
  int i = 0, j = 0, k = 0, nentries = 0;
  int low2, middle2, high2;
  bool donep = false, foundp;
  double weight;
  expecteddist1 = expecteddist2 = 0;

  if (npositions0 == 0) {
    *nnew = 0;
    *overflowp = false;
    return matches;
  } else {
    position0 = positions0[0];
    expected1 = position0 + expecteddist1;
    if (npositions1 == 0) {
      *nnew = 0;
      *overflowp = false;
      return matches;
    } else {
      position1 = positions1[0];
      expected2 = position1 + expecteddist2;
      if (npositions2 == 0) {
	*nnew = 0;
	*overflowp = false;
	return matches;
      } else {
	position2 = positions2[0];
      }
    }
  }
      

  Matchpool_save(matchpool);

  while (donep == false) {
    debug4(printf("  %d:%u %d:%u %d:%u\n",
		  i,position0,j,position1,k,position2));
    if (expected1 < position1) {
      /* Advance position0 */
      if (++i >= npositions0) {
	donep = true;
      } else {
	position0 = positions0[i];
	expected1 = position0 + expecteddist1;
      }
    } else if (expected1 > position1) {
      /* Advance position1 */
      if (++j >= npositions1) {
	donep = true;
      } else {
	position1 = positions1[j];
	expected2 = position1 + expecteddist2;
      }
    } else if (expected2 < position2) {
      /* Advance position1 */
      if (++j >= npositions1) {
	donep = true;
      } else {
	position1 = positions1[j];
	expected2 = position1 + expecteddist2;
      }
    } else if (expected2 > position2) {
      /* Binary search */
      low2 = k+1;
      high2 = npositions2;
      foundp = false;
      debug4(
	     for (middle2 = low2; middle2 < high2; middle2++) {
	       printf("  --%d:%u %d:%u %d:%u\n",
		      i,position0,j,position1,middle2,
		      positions2[middle2]);
	     }
	     );

      while (!foundp && low2 < high2) {
	middle2 = (low2+high2)/2;
	debug4(printf("  **%d:%u %d:%u %d:%u   vs. %u\n",
		      low2,positions2[low2],
		      middle2,positions2[middle2],
		      high2,positions2[high2],
		      expected2));
	if (expected2 < positions2[middle2]) {
	  high2 = middle2;
	} else if (expected2 > positions2[middle2]) {
	  low2 = middle2 + 1;
	} else {
	  foundp = true;
	}
      }
      if (foundp == true) {
	k = middle2;
	position2 = positions2[k];
      } else if ((k = high2) >= npositions2) {
	donep = true;
      } else {
	position2 = positions2[k];
      }

    } else {
      debug4(printf("Success at position %u\n",positions0[i]));
      if (!chrsubset || Chrsubset_includep(chrsubset,position0,chromosome_iit) == true) {
	if (++nentries > maxentries) {
	  donep = true;
	} else {
	  newmatches = Matchpool_push(newmatches,matchpool,merstart,querylength,forwardp,fivep,
				      position0+positionadj,chromosome_iit);
	}
      }

      /* Advance all */
      if (++i >= npositions0) {
	donep = true;
      } else {
	position0 = positions0[i];
	expected1 = position0 + expecteddist1;
	if (++j >= npositions1) {
	  donep = true;
	} else {
	  position1 = positions1[j];
	  expected2 = position1 + expecteddist2;
	  if (++k >= npositions2) {
	    donep = true;
	  } else {
	    position2 = positions2[k];
	  }
	}
      }
    }
  }

  if (nentries > maxentries) {
    *nnew = 0;
    *overflowp = true;
    debug(printf("  Triples overflow at %d\n",merstart));
    Matchpool_restore(matchpool);
  } else {
    *nnew = nentries;
    *overflowp = false;
    if (nentries > 0) {
      weight = 1.0/(double) nentries;
      for (p = newmatches; p != NULL; p = p->rest) {
	match = (Match_T) p->first;
	Match_set_weight(match,weight);
      }
    }
    matches = Matchpool_transfer(matches,newmatches);
  }

  return matches;
}


/************************************************************************

              merstart
5', forward:  prevpos (pos0)     querypos (pos1)
5', revcomp:  prevpos (pos1)     querypos (pos0)

3', forward:  querypos (pos0)    prevpos (pos1)
3', revcomp:  querypos (pos1)    prevpos (pos0)

************************************************************************/

static List_T
identify_matches (int *nnew, bool *overflowp, List_T matches, int querypos, int querylength,
		  int oligosize, int matchinterval, int matchinterval_nt,
		  Genomicpos_T **plus_positions, int *plus_npositions,
		  Genomicpos_T **minus_positions, int *minus_npositions, 
		  IIT_T chromosome_iit, Chrsubset_T chrsubset, Matchpool_T matchpool,
		  bool forwardp, bool fivep, int maxentries) {
  int prevpos, middlepos;
  int merstart, pos0, pos1;
  Genomicpos_T **positions, positionadj = 0U;
  int *npositions;
  int matchsize;

#ifdef PMAP
  matchsize = matchinterval + INDEX1PART_AA;
#else
  matchsize = matchinterval + INDEX1PART;
#endif

  if (fivep == true) {
    prevpos = querypos - matchinterval;
    middlepos = querypos - oligosize;
    merstart = prevpos;
  } else {
    prevpos = querypos + matchinterval;
    middlepos = querypos + oligosize;
    merstart = querypos;
  }
  if (forwardp == fivep) {
    pos0 = prevpos;
    pos1 = querypos;
  } else {
    pos0 = querypos;
    pos1 = prevpos;
  }

  if (forwardp == true) {
    positions = plus_positions;
    npositions = plus_npositions;
  } else {
    positions = minus_positions;
    npositions = minus_npositions;
#ifndef PMAP
    /* Need this adjustment because gmapindex has only one
       idxpositions file for both strands, where position always
       points to lowest coordinate of the 12-mer */
    positionadj = matchsize - 1U;
#endif
  }

  if (matchsize == oligosize) {
    return identify_singles(&(*nnew),&(*overflowp),matches,merstart,positionadj,querylength,
			    positions[pos0],npositions[pos0],
			    matchsize,chromosome_iit,chrsubset,matchpool,forwardp,fivep,maxentries);
  } else if (matchsize <= 2*oligosize) {
    /* debug(printf("Using positions at %d and %d\n",pos0,pos1)); */
    return identify_doubles(&(*nnew),&(*overflowp),matches,merstart,positionadj,querylength,
			    positions[pos0],npositions[pos0],
			    positions[pos1],npositions[pos1],matchsize,
			    chromosome_iit,chrsubset,matchpool,forwardp,fivep,maxentries);
  } else if (matchsize == 3*oligosize) {
    /* This branch hasn't been tested very well */
    debug(printf("Using positions at %d and %d and %d\n",pos0,middlepos,pos1));
    return identify_triples(&(*nnew),&(*overflowp),matches,merstart,positionadj,querylength,
			    positions[pos0],npositions[pos0],
			    positions[middlepos],npositions[middlepos],
			    positions[pos1],npositions[pos1],matchsize,
#ifdef PMAP
			    oligosize*3,oligosize*3,
#else
			    oligosize,oligosize,
#endif
			    chromosome_iit,chrsubset,matchpool,forwardp,fivep,maxentries);
  } else {
    abort();
  }
}


/* Need to change these procedures from n*m to n+m */
static List_T
find_5prime_matches (int *nnew, List_T matches5, T this, int matchsize,
		     Genomicpos_T **plus_positions, int *plus_npositions,
		     Genomicpos_T **minus_positions, int *minus_npositions,
		     IIT_T chromosome_iit, Chrsubset_T chrsubset, Matchpool_T matchpool,
		     int querystart, int querylength, int maxentries, bool pairedp) {
  int merstart;			/* Note: negative values must be allowed */
  int nnewplus = 0, nnewminus = 0;
  bool overflowp;
  int matchinterval, matchinterval_nt;

  matchinterval = matchsize - this->oligosize;
#ifdef PMAP
  matchinterval_nt = matchinterval*3;
#else
  matchinterval_nt = matchinterval;
#endif

  if ((merstart = querystart - matchinterval) < 0) {
    debug3(printf("Quitting because %d - %d < 0\n",querystart,matchinterval));
    *nnew = 0;
  } else {
    debug3(printf("Identifying 5' matches for %d-mer at %d and %d.  maxentries=%d.\n",
		  matchsize,merstart,querystart,maxentries));

    debug3(printf("  Previous status of plus_matchedp at %d is %d\n",merstart,this->plus_matchedp[merstart]));
    if (pairedp == true || this->plus_matchedp[merstart] == false) {
      matches5 = identify_matches(&nnewplus,&overflowp,matches5,querystart,querylength,
				  this->oligosize,matchinterval,matchinterval_nt,
				  plus_positions,plus_npositions,minus_positions,minus_npositions,
				  chromosome_iit,chrsubset,matchpool,/*forwardp*/true,/*fivep*/true,maxentries);
      if (overflowp == false) {
	debug3(printf("  No overflow so setting plus_matchedp at %d to true\n",merstart));
	this->plus_matchedp[merstart] = true;
      }
    }

    debug3(printf("  Previous status of minus_matchedp at %d is %d\n",merstart,this->minus_matchedp[merstart]));
    if (pairedp == true || this->minus_matchedp[merstart] == false) {
      matches5 = identify_matches(&nnewminus,&overflowp,matches5,querystart,querylength,
				  this->oligosize,matchinterval,matchinterval_nt,
				  plus_positions,plus_npositions,minus_positions,minus_npositions,
				  chromosome_iit,chrsubset,matchpool,/*forwardp*/false,/*fivep*/true,maxentries);
      if (overflowp == false) {
	debug3(printf("  No overflow so setting minus_matchedp at %d to true\n",merstart));
	this->minus_matchedp[merstart] = true;
      }
    }

    *nnew = nnewplus + nnewminus;
  }

  return matches5;
}

static List_T
find_3prime_matches (int *nnew, List_T matches3, T this, int matchsize,
		     Genomicpos_T **plus_positions, int *plus_npositions,
		     Genomicpos_T **minus_positions, int *minus_npositions,
		     IIT_T chromosome_iit, Chrsubset_T chrsubset, Matchpool_T matchpool,
		     int queryend, int querylength, int maxentries, bool pairedp) {
  int merstart;
  int nnewplus = 0, nnewminus = 0;
  bool overflowp;
  int matchinterval, matchinterval_nt;

  matchinterval = matchsize - this->oligosize;
#ifdef PMAP
  matchinterval_nt = matchinterval*3;
#else
  matchinterval_nt = matchinterval;
#endif

  if (queryend + matchsize > querylength) {
    debug3(printf("Quitting because %d + %d > %d\n",queryend,matchsize,querylength));
    *nnew = 0;
  } else {
    merstart = queryend;
    debug3(printf("Identifying 3' matches for %d-mer at %d and %d.  maxentries=%d\n",
		  matchsize,queryend,queryend+matchinterval,maxentries));

    /* If maxentries > 0, then we are working on pairs.  Otherwise, check for a successful previous check */
    debug3(printf("  Previous status of plus_matchedp at %d is %d\n",merstart,this->plus_matchedp[merstart]));
    if (pairedp == true || this->plus_matchedp[merstart] == false) {

      matches3 = identify_matches(&nnewplus,&overflowp,matches3,queryend,querylength,
				  this->oligosize,matchinterval,matchinterval_nt,
				  plus_positions,plus_npositions,minus_positions,minus_npositions,
				  chromosome_iit,chrsubset,matchpool,/*forwardp*/true,/*fivep*/false,maxentries);
      if (overflowp == false) {
	debug3(printf("  No overflow so setting plus_matchedp at %d to true\n",merstart));
	this->plus_matchedp[merstart] = true;
      }
    }

    debug3(printf("  Previous status of minus_matchedp at %d is %d\n",merstart,this->minus_matchedp[merstart]));
    if (pairedp == true || this->minus_matchedp[merstart] == false) {
      matches3 = identify_matches(&nnewminus,&overflowp,matches3,queryend,querylength,
				  this->oligosize,matchinterval,matchinterval_nt,
				  plus_positions,plus_npositions,minus_positions,minus_npositions,
				  chromosome_iit,chrsubset,matchpool,/*forwardp*/false,/*fivep*/false,maxentries);
      if (overflowp == false) {
	debug3(printf("  No overflow so setting minus_matchedp at %d to true\n",merstart));
	this->minus_matchedp[merstart] = true;
      }
    }

    *nnew = nnewplus + nnewminus;
  }

  return matches3;
}


/*
static bool
check_fraction_paired (List_T matches5, List_T matches3) {
  List_T p;
  int npaired5 = 0, nsingle5 = 0, npaired3 = 0, nsingle3 = 0;
  double fracpaired5, fracpaired3;

  for (p = matches5; p != NULL; p = p->rest) {
    if (Match_pairedp((Match_T) List_head(p)) == true) {
      npaired5++;
      } else {
	nsingle5++;
      }
  }
  for (p = matches3; p != NULL; p = p->rest) {
    if (Match_pairedp((Match_T) List_head(p)) == true) {
      npaired3++;
    } else {
      nsingle3++;
    }
  }

  debug(printf("npaired5: %d, nsingle5: %d, npaired3: %d, nsingle3: %d\n",
	       npaired5,nsingle5,npaired3,nsingle3);
	);
  if (npaired5+nsingle5 == 0) {
    fracpaired5 = (double) (npaired5+1)/(double) (npaired5+nsingle5+1);
  } else {
    fracpaired5 = (double) npaired5/(double) (npaired5+nsingle5);
  }
  if (npaired3+nsingle3 == 0) {
    fracpaired3 = (double) npaired3/(double) (npaired3+nsingle3+1);
  } else {
    fracpaired3 = (double) npaired3/(double) (npaired3+nsingle3);
  }
  if (fracpaired5 > 0.75 || fracpaired3 > 0.75) {
    return true;
  } else {
    return false;
  }
}
*/

static List_T
stutter (List_T gregionlist, T this, int matchsize,
#ifdef PMAP
	 Indexdb_T indexdb_fwd,
	 Indexdb_T indexdb_rev,
#else
	 Indexdb_T indexdb, 
#endif
	 IIT_T chromosome_iit, Chrsubset_T chrsubset, 
	 Matchpool_T matchpool, int stuttercycles, int stutterhits) {
  List_T newmatches5 = NULL, newmatches3 = NULL;
  int stutterdist5 = 0, stutterdist3 = 0, maxbases, start5, start3;
  bool foundpairp;
  double n5hits = 0.0, n3hits = 0.0;
  int nnew;

  start5 = Block_querypos(this->block5);
  start3 = Block_querypos(this->block3);

#if 0
  maxbases = stuttercycles*this->matchsize;  /* (matchsize = matchinterval + INDEX1PART_AA/INDEX1PART); */
  if (maxbases > (start3 - start5)/2) {
    maxbases = (start3 - start5)/2;
  }
#else
  maxbases = (start3 - start5)/2;
#endif


  debug(printf("*** Beginning stutter.  maxbases = %d, stutterhits = %d ***\n",maxbases,stutterhits));

  while (Block_next(this->block5) == true && 
	 stutterdist5 < maxbases && n5hits < stutterhits) {
    this->querystart = Block_querypos(this->block5);
    if (this->processedp[this->querystart] == false) {
#ifdef PMAP
      Block_process_oligo(&(this->plus_positions[this->querystart]),&(this->plus_npositions[this->querystart]),
			  &(this->minus_positions[this->querystart]),&(this->minus_npositions[this->querystart]),
			  this->block5,indexdb_fwd,indexdb_rev);
#else
      Block_process_oligo(&(this->plus_positions[this->querystart]),&(this->plus_npositions[this->querystart]),
			  &(this->minus_positions[this->querystart]),&(this->minus_npositions[this->querystart]),
			  this->block5,indexdb);
#endif
      debug(printf("stutter: Processing 5' position %d: %d plus, %d minus",
		   this->querystart,this->plus_npositions[this->querystart],this->minus_npositions[this->querystart]));
      debug1(
	     for (i = 0; i < this->plus_npositions[this->querystart]; i++) {
	       printf(" +%u",this->plus_positions[this->querystart][i]);
	     }
	     for (i = 0; i < this->minus_npositions[this->querystart]; i++) {
	       printf(" -%u",this->minus_positions[this->querystart][i]);
	     }
	     );
      debug(printf("\n"));

      this->processedp[this->querystart] = true;
    }
    newmatches5 = find_5prime_matches(&nnew,newmatches5,this,matchsize,
				      this->plus_positions,this->plus_npositions,
				      this->minus_positions,this->minus_npositions,
				      chromosome_iit,chrsubset,matchpool,this->querystart,
				      this->querylength,this->maxentries,true);

    stutterdist5 = Block_querypos(this->block5) - start5;
    if (nnew > 0) {
      n5hits += 1.0/(double) (1 + nnew);
      debug(printf("n5hits = %.1f, stutterdist5 = %d\n",n5hits,stutterdist5));
    }
  }

  while (Block_next(this->block3) == true && 
	 stutterdist3 < maxbases && n3hits < stutterhits) {
    this->queryend = Block_querypos(this->block3);
    if (this->processedp[this->queryend] == false) {
#ifdef PMAP
      Block_process_oligo(&(this->plus_positions[this->queryend]),&(this->plus_npositions[this->queryend]),
			  &(this->minus_positions[this->queryend]),&(this->minus_npositions[this->queryend]),
			  this->block3,indexdb_fwd,indexdb_rev);
#else
      Block_process_oligo(&(this->plus_positions[this->queryend]),&(this->plus_npositions[this->queryend]),
			  &(this->minus_positions[this->queryend]),&(this->minus_npositions[this->queryend]),
			  this->block3,indexdb);
#endif
      debug(printf("stutter: Processing 3' position %d: %d plus, %d minus",
		   this->queryend,this->plus_npositions[this->queryend],this->minus_npositions[this->queryend]));
      debug1(
	     for (i = 0; i < this->plus_npositions[this->queryend]; i++) {
	       printf(" +%u",this->plus_positions[this->queryend][i]);
	     }
	     for (i = 0; i < this->minus_npositions[this->queryend]; i++) {
	       printf(" -%u",this->minus_positions[this->queryend][i]);
	     }
	     );
      debug(printf("\n"));
      this->processedp[this->queryend] = true;
    }
    newmatches3 = find_3prime_matches(&nnew,newmatches3,this,matchsize,
				      this->plus_positions,this->plus_npositions,
				      this->minus_positions,this->minus_npositions,
				      chromosome_iit,chrsubset,matchpool,this->queryend,
				      this->querylength,this->maxentries,true);

    stutterdist3 = start3 - Block_querypos(this->block3);
    if (nnew > 0) {
      n3hits += 1.0/(double) (1 + nnew);
      debug(printf("n3hits = %.1f, stutterdist3 = %d\n",n3hits,stutterdist3));
    }
  }

  debug(printf("*** Ending stutter ***\n"));

  gregionlist = pair_up(&foundpairp,gregionlist,matchsize,newmatches5,newmatches3,
			this->matches5,this->matches3,chromosome_iit,this->querylength,this->maxtotallen,
			this->trimstart,this->trimend,this->trimlength);

  this->matches5 = Matchpool_transfer(this->matches5,newmatches5);
  this->matches3 = Matchpool_transfer(this->matches3,newmatches3);

  return gregionlist;
}


/* Tries to find matches on the 5' end to unpaired hits from the 3' end */
static List_T
fill_in_5 (List_T gregionlist, T this, int matchsize, List_T dangling3,
#ifdef PMAP
	   Indexdb_T indexdb_fwd,
	   Indexdb_T indexdb_rev,
#else
	   Indexdb_T indexdb, 
#endif
	   IIT_T chromosome_iit, Chrsubset_T chrsubset, 
	   Matchpool_T matchpool) {
  List_T newmatches5 = NULL;
  int fillindist5 = 0, maxbases, start5;
  bool foundpairp = false;
  int nnew;
#ifdef DEBUG1
  int i;
#endif

  start5 = Block_querypos(this->block5);
  maxbases = MAX_FILL_IN;
  if (maxbases > this->querylength/2 - start5) {
    maxbases = this->querylength/2 - start5;
  }
  debug(printf("*** Beginning fill_in_5.  maxbases = %d ***\n",maxbases));

  while (Block_next(this->block5) == true && 
	 fillindist5 < maxbases && foundpairp == false) {
    this->querystart = Block_querypos(this->block5);
    if (this->processedp[this->querystart] == false) {
#ifdef PMAP
      Block_process_oligo(&(this->plus_positions[this->querystart]),&(this->plus_npositions[this->querystart]),
			  &(this->minus_positions[this->querystart]),&(this->minus_npositions[this->querystart]),
			  this->block5,indexdb_fwd,indexdb_rev);
#else
      Block_process_oligo(&(this->plus_positions[this->querystart]),&(this->plus_npositions[this->querystart]),
			  &(this->minus_positions[this->querystart]),&(this->minus_npositions[this->querystart]),
			  this->block5,indexdb);
#endif
      debug(printf("fill_in_5: Processing 5' position %d: %d plus, %d minus",
		   this->querystart,this->plus_npositions[this->querystart],this->minus_npositions[this->querystart]));
      debug1(
	     for (i = 0; i < this->plus_npositions[this->querystart]; i++) {
	       printf(" +%u",this->plus_positions[this->querystart][i]);
	     }
	     for (i = 0; i < this->minus_npositions[this->querystart]; i++) {
	       printf(" -%u",this->minus_positions[this->querystart][i]);
	     }
	     );
      debug(printf("\n"));
      this->processedp[this->querystart] = true;
    }
    newmatches5 = find_5prime_matches(&nnew,newmatches5,this,matchsize,
				      this->plus_positions,this->plus_npositions,
				      this->minus_positions,this->minus_npositions,
				      chromosome_iit,chrsubset,matchpool,this->querystart,
				      this->querylength,this->maxentries,true);
    fillindist5 = Block_querypos(this->block5) - start5;

    if (nnew > 0) {
      gregionlist = pair_up(&foundpairp,gregionlist,matchsize,newmatches5,(List_T) NULL,
			    (List_T) NULL,dangling3,chromosome_iit,this->querylength,this->maxtotallen,
			    this->trimstart,this->trimend,this->trimlength);
      debug(printf("   Foundpairp = %d\n",foundpairp));
    }
  }

  /* Mark newmatches5 as being pairedp if they match non-dangling matches3 */
  gregionlist = pair_up(&foundpairp,gregionlist,matchsize,newmatches5,(List_T) NULL,
			(List_T) NULL,this->matches3,chromosome_iit,this->querylength,this->maxtotallen,
			this->trimstart,this->trimend,this->trimlength);

  this->matches5 = Matchpool_transfer(this->matches5,newmatches5);

  debug(printf("*** Ending fill_in_5 ***\n"));
  return gregionlist;
}


/* Tries to find matches on the 5' end to unpaired hits from the 3' end */
static List_T
fill_in_3 (List_T gregionlist, T this, int matchsize, List_T dangling5, 
#ifdef PMAP
	   Indexdb_T indexdb_fwd,
	   Indexdb_T indexdb_rev,
#else
	   Indexdb_T indexdb,
#endif
	   IIT_T chromosome_iit, Chrsubset_T chrsubset,
	   Matchpool_T matchpool) {
  List_T newmatches3 = NULL;
  int fillindist3 = 0, maxbases, start3;
  bool foundpairp = false;
  int nnew;
#ifdef DEBUG1
  int i;
#endif

  start3 = Block_querypos(this->block3);
  maxbases = MAX_FILL_IN;
  if (maxbases > start3 - this->querylength/2) {
    maxbases = start3 - this->querylength/2;
  }
  debug(printf("*** Beginning fill_in_3.  maxbases = %d ***\n",maxbases));

  while (Block_next(this->block3) == true && 
	 fillindist3 < maxbases && foundpairp == false) {
    this->queryend = Block_querypos(this->block3);
    if (this->processedp[this->queryend] == false) {
#ifdef PMAP
      Block_process_oligo(&(this->plus_positions[this->queryend]),&(this->plus_npositions[this->queryend]),
			  &(this->minus_positions[this->queryend]),&(this->minus_npositions[this->queryend]),
			  this->block3,indexdb_fwd,indexdb_rev);
#else
      Block_process_oligo(&(this->plus_positions[this->queryend]),&(this->plus_npositions[this->queryend]),
			  &(this->minus_positions[this->queryend]),&(this->minus_npositions[this->queryend]),
			  this->block3,indexdb);
#endif
      debug(printf("fill_in_3: Processing 3' position %d: %d plus, %d minus",
		   this->queryend,this->plus_npositions[this->queryend],this->minus_npositions[this->queryend]));
      debug1(
	     for (i = 0; i < this->plus_npositions[this->queryend]; i++) {
	       printf(" +%u",this->plus_positions[this->queryend][i]);
	     }
	     for (i = 0; i < this->minus_npositions[this->queryend]; i++) {
	       printf(" -%u",this->minus_positions[this->queryend][i]);
	     }
	     );
      debug(printf("\n"));
      this->processedp[this->queryend] = true;
    }
    newmatches3 = find_3prime_matches(&nnew,newmatches3,this,matchsize,
				      this->plus_positions,this->plus_npositions,
				      this->minus_positions,this->minus_npositions,
				      chromosome_iit,chrsubset,matchpool,this->queryend,
				      this->querylength,this->maxentries,true);
    fillindist3 = start3 - Block_querypos(this->block3);

    if (nnew > 0) {
      gregionlist = pair_up(&foundpairp,gregionlist,matchsize,(List_T) NULL,newmatches3,
			    dangling5,(List_T) NULL,chromosome_iit,this->querylength,this->maxtotallen,
			    this->trimstart,this->trimend,this->trimlength);
      debug(printf("   Foundpairp = %d\n",foundpairp));
    }
  }

  /* Mark newmatches3 as being pairedp if they match non-dangling matches5 */
  gregionlist = pair_up(&foundpairp,gregionlist,matchsize,(List_T) NULL,newmatches3,
			this->matches5,(List_T) NULL,chromosome_iit,this->querylength,this->maxtotallen,
			this->trimstart,this->trimend,this->trimlength);

  this->matches3 = Matchpool_transfer(this->matches3,newmatches3);

  debug(printf("*** Ending fill_in_3 ***\n"));
  return gregionlist;
}


#if 0
static void
sample (T this,
#ifdef PMAP
	Indexdb_T indexdb_fwd,
	Indexdb_T indexdb_rev,
#else
	Indexdb_T indexdb,
#endif
	int nskip) {
  int mod5 = 0, mod3 = 0;
#ifdef DEBUG1
  int i;
#endif

  Block_restore(this->block5);
  Block_restore(this->block3);

  while (Block_next(this->block5) == true) {
    this->querystart = Block_querypos(this->block5);
    if (this->processedp[this->querystart] == false) {
#ifdef PMAP
      Block_process_oligo(&(this->plus_positions[this->querystart]),&(this->plus_npositions[this->querystart]),
			  &(this->minus_positions[this->querystart]),&(this->minus_npositions[this->querystart]),
			  this->block5,indexdb_fwd,indexdb_rev);
#else
      Block_process_oligo(&(this->plus_positions[this->querystart]),&(this->plus_npositions[this->querystart]),
			  &(this->minus_positions[this->querystart]),&(this->minus_npositions[this->querystart]),
			  this->block5,indexdb);
#endif
      debug(printf("sample: Processing 5' position %d: %d plus, %d minus",
		   this->querystart,this->plus_npositions[this->querystart],this->minus_npositions[this->querystart]));
      debug1(
	     for (i = 0; i < this->plus_npositions[this->querystart]; i++) {
	       printf(" +%u",this->plus_positions[this->querystart][i]);
	     }
	     for (i = 0; i < this->minus_npositions[this->querystart]; i++) {
	       printf(" -%u",this->minus_positions[this->querystart][i]);
	     }
	     );
      debug(printf("\n"));
      this->processedp[this->querystart] = true;

      if (++mod5 % SAMPLERUN == 0) {
	mod5 = 0;
	Block_skip(this->block5,nskip);
      }
    }
  }

  while (Block_next(this->block3) == true) {
    this->queryend = Block_querypos(this->block3);
    if (this->processedp[this->queryend] == false) {
#ifdef PMAP
      Block_process_oligo(&(this->plus_positions[this->queryend]),&(this->plus_npositions[this->queryend]),
			  &(this->minus_positions[this->queryend]),&(this->minus_npositions[this->queryend]),
			  this->block3,indexdb_fwd,indexdb_rev);
#else
      Block_process_oligo(&(this->plus_positions[this->queryend]),&(this->plus_npositions[this->queryend]),
			  &(this->minus_positions[this->queryend]),&(this->minus_npositions[this->queryend]),
			  this->block3,indexdb);
#endif
      debug(printf("sample: Processing 3' position %d: %d plus, %d minus",
		   this->queryend,this->plus_npositions[this->queryend],this->minus_npositions[this->queryend]));
      debug1(
	     for (i = 0; i < this->plus_npositions[this->queryend]; i++) {
	       printf(" +%u",this->plus_positions[this->queryend][i]);
	     }
	     for (i = 0; i < this->minus_npositions[this->queryend]; i++) {
	       printf(" -%u",this->minus_positions[this->queryend][i]);
	     }
	     );
      debug(printf("\n"));
      this->processedp[this->queryend] = true;

      if (++mod3 % SAMPLERUN == 0) {
	mod3 = 0;
	Block_skip(this->block3,nskip);
      }
    }
  }

  return;
}
#endif


static Genomicpos_T *
find_range (int **querypositions, int *ninrange, int starti, int endi,
	    Genomicpos_T **positions, int *npositions, Genomicpos_T leftbound, Genomicpos_T rightbound) {
  Genomicpos_T *range;
  int i;
  int querypos;

  if (rightbound < leftbound) {
    abort();
  }

  *ninrange = 0;
  for (querypos = starti; querypos <= endi; querypos++) {
    i = binary_search(0,npositions[querypos],positions[querypos],leftbound);
    while (i < npositions[querypos] && positions[querypos][i] < rightbound) {
      debug2(printf("At querypos %d, found position %u in (%u,%u)\n",querypos,positions[querypos][i],leftbound,rightbound));
      (*ninrange)++;
      i++;
    }
  }

  if (*ninrange == 0) {
    *querypositions = (int *) NULL;
    return (Genomicpos_T *) NULL;
  } else {
    *querypositions = (int *) CALLOC(*ninrange,sizeof(Genomicpos_T));
    range = (Genomicpos_T *) CALLOC(*ninrange,sizeof(Genomicpos_T));
  }

  *ninrange = 0;
  for (querypos = starti; querypos <= endi; querypos++) {
    i = binary_search(0,npositions[querypos],positions[querypos],leftbound);
    while (i < npositions[querypos] && positions[querypos][i] < rightbound) {
      (*querypositions)[*ninrange] = querypos;
      range[*ninrange] = positions[querypos][i];
      (*ninrange)++;
      i++;
    }
  }

  return range;
}

static void
find_extensions (Genomicpos_T *extension5, Genomicpos_T *extension3, T this,
		 Gregion_T gregion, int maxintronlen_bound, bool continuousp) {
  int *querypositions, querystart, queryend, ninrange, i, j, lastj;
  Genomicpos_T *range, leftbound, rightbound, best_start, best_end, expectedi, expectedj;
  int best_concentration, concentration;
  int maxintronlen5, maxintronlen3;
#ifdef DEBUG2
  int best_querystart, best_queryend;
#endif

  querystart = Gregion_querystart(gregion);
  queryend = Gregion_queryend(gregion);
  debug2(printf("Entering Stage1_find_extensions with querystart = %d, queryend = %d, querylength %d\n",
		querystart,queryend,this->querylength));

  debug2(printf("continuousp = %d, this->trimlength %d (vs %d), querystart %d (vs %d)\n",
		continuousp,this->trimlength,SINGLEEXONLENGTH,querystart,NOEXTENDLEN));
  if (continuousp == true || this->trimlength < SINGLEEXONLENGTH || querystart < NOEXTENDLEN) {
    maxintronlen5 = querystart + 20;
  } else {
    maxintronlen5 = maxintronlen_bound;
  }

  debug2(printf("continuousp = %d, this->trimlength %d (vs %d), this->trimlength - queryend %d (vs %d)\n",
		continuousp,this->trimlength,SINGLEEXONLENGTH,this->trimlength - queryend,NOEXTENDLEN));
  if (continuousp == true || this->trimlength < SINGLEEXONLENGTH || this->trimlength - queryend < NOEXTENDLEN) {
    maxintronlen3 = this->querylength - queryend + 20;
  } else {
    maxintronlen3 = maxintronlen_bound;
  }
  debug2(printf("Set maxintronlen5 = %u, maxintronlen3 = %u\n",maxintronlen5,maxintronlen3));

  if (Gregion_plusp(gregion) == true) {
    rightbound = Gregion_genomicstart(gregion); /* was Match_position(match5)  */
    if (rightbound < maxintronlen5) {
      leftbound = 0U;
    } else {
      leftbound = rightbound - maxintronlen5;
    }
    range = find_range(&querypositions,&ninrange,0,querystart-1,
		       this->plus_positions,this->plus_npositions,
		       leftbound,rightbound);

    best_concentration = 0;
    best_start = Gregion_genomicstart(gregion);	/* was Match_position(match5) */
    debug2(best_querystart = querystart);
    for (i = 0; i < ninrange; i++) {
      debug2(printf("Looking at range %d = %u@%d (distance = %u)\n",
		    i,range[i],querypositions[i],Gregion_genomicstart(gregion)-range[i]));
      concentration = 1;
      lastj = i;
      for (j = i+1; j < ninrange; j++) {
	debug2(printf("  %u@%d",range[j],querypositions[j]));
	expectedj = range[i] + querypositions[j] - querypositions[i];
	if (range[j] + 20 > expectedj && range[j] < expectedj + 20) {
	  concentration++;
	  lastj = j;
	  debug2(printf("*"));
	}
      }
      debug2(printf("\nConcentration is %d\n\n",concentration));
      if (concentration > best_concentration || 
	  (concentration == best_concentration && range[i] > best_start)) {
	best_concentration = concentration;
	best_start = range[i];
	debug2(best_querystart = querypositions[i]);
      }
    }
    FREE(querypositions);
    FREE(range);

    *extension5 = Gregion_genomicstart(gregion) - best_start;  /* was Match_position(match5) */

  } else {
    leftbound = Gregion_genomicend(gregion); /* was Match_position(match5) */
    rightbound = leftbound + maxintronlen5;
    range = find_range(&querypositions,&ninrange,0,querystart-1,
		       this->minus_positions,this->minus_npositions,
		       leftbound,rightbound);

    best_concentration = 0;
    best_start = Gregion_genomicend(gregion); /* was Match_position(match5) */
    debug2(best_querystart = querystart);
    for (i = 0; i < ninrange; i++) {
      debug2(printf("Looking at range %d = %u@%d (distance = %u)\n",
		    i,range[i],querypositions[i],range[i]-Gregion_genomicend(gregion)));
      concentration = 1;
      lastj = i;
      for (j = i+1; j < ninrange; j++) {
	debug2(printf("  %u%d",range[j],querypositions[j]));
	expectedi = range[j] + querypositions[j] - querypositions[i];
	if (range[i] + 20 > expectedi && range[i] < expectedi + 20) {
	  concentration++;
	  lastj = j;
	  debug2(printf("*"));
	}
      }
      debug2(printf("\nConcentration is %d\n\n",concentration));
      if (concentration > best_concentration ||
	  (concentration == best_concentration && range[i] < best_start)) {
	best_concentration = concentration;
	best_start = range[i];
	debug2(best_querystart = querypositions[i]);
      }
    }
    FREE(querypositions);
    FREE(range);

    *extension5 = best_start - Gregion_genomicend(gregion); /* was - Match_position(match5) */
  }

  debug2(printf("5' extension, from positions, best_querystart = %d\n",best_querystart));
  debug2(printf("5' extension, From positions, extension5 = %d\n",*extension5));

  if (Gregion_plusp(gregion) == true) {
    leftbound = Gregion_genomicend(gregion); /* was Match_position(match3) */
    rightbound = leftbound + maxintronlen3;
    range = find_range(&querypositions,&ninrange,queryend+this->oligosize+1,this->querylength-1,
		       this->plus_positions,this->plus_npositions,
		       leftbound,rightbound);

    best_concentration = 0;
    best_end = Gregion_genomicend(gregion);  /* was Match_position(match3); */
    debug2(best_queryend = queryend);
    for (i = 0; i < ninrange; i++) {
      debug2(printf("Looking at range %d = %u@%d (distance = %u)\n",
		    i,range[i],querypositions[i],range[i]-Gregion_genomicend(gregion)));
      concentration = 1;
      lastj = i;
      for (j = i+1; j < ninrange; j++) {
	debug2(printf("  %u@%d",range[j],querypositions[j]));
	expectedj = range[i] + querypositions[j] - querypositions[i];
	if (range[j] + 20 > expectedj && range[j] < expectedj + 20) {
	  concentration++;
	  lastj = j;
	  debug2(printf("*"));
	}
      }
      debug2(printf("\nConcentration is %d\n\n",concentration));
      if (concentration > best_concentration ||
	  (concentration == best_concentration && range[lastj] < best_end)) {
	best_concentration = concentration;
	best_end = range[lastj];
	debug2(best_queryend = querypositions[lastj]);
      }
    }
    FREE(querypositions);
    FREE(range);

    *extension3 = best_end - Gregion_genomicend(gregion); /* was - Match_position(match3); */

  } else {
    rightbound = Gregion_genomicstart(gregion);  /* was Match_position(match3); */
    if (rightbound < maxintronlen3) {
      leftbound = 0U;
    } else {
      leftbound = rightbound - maxintronlen3;
    }
    range = find_range(&querypositions,&ninrange,queryend+this->oligosize+1,this->querylength-1,
		       this->minus_positions,this->minus_npositions,
		       leftbound,rightbound);

    best_concentration = 0;
    best_end = Gregion_genomicstart(gregion);  /* was Match_position(match3); */
    debug2(best_queryend = queryend);
    for (i = 0; i < ninrange; i++) {
      debug2(printf("Looking at range %d = %u@%d (distance = %u)\n",
		    i,range[i],querypositions[i],Gregion_genomicstart(gregion)-range[i]));
      concentration = 1;
      lastj = i;
      for (j = i+1; j < ninrange; j++) {
	debug2(printf("  %u@%d",range[j],querypositions[j]));
	expectedi = range[j] + querypositions[j] - querypositions[i];
	if (range[i] + 20 > expectedi && range[i] < expectedi + 20) {
	  concentration++;
	  lastj = j;
	  debug2(printf("*"));
	}
      }
      debug2(printf("\nConcentration is %d\n\n",concentration));
      if (concentration > best_concentration ||
	  (concentration == best_concentration && range[lastj] > best_end)) {
	best_concentration = concentration;
	best_end = range[lastj];
	debug2(best_queryend = querypositions[lastj]);
      }
    }
    FREE(querypositions);
    FREE(range);

    *extension3 = Gregion_genomicstart(gregion) - best_end;  /* was Match_position(match3) */
  }
    
  debug2(printf("3' extension, from positions, best_queryend = %d\n",best_queryend));
  debug2(printf("3' extension, from positions, extension3 = %d\n",*extension3));

  return;
}



static List_T
find_first_pair (bool *foundpairp, List_T gregionlist, T this, int matchsize,
#ifdef PMAP
		 Indexdb_T indexdb_fwd,
		 Indexdb_T indexdb_rev,
#else
		 Indexdb_T indexdb,
#endif
		 IIT_T chromosome_iit, Chrsubset_T chrsubset,
		 Matchpool_T matchpool, int maxattempts) {
  List_T newmatches5 = NULL, newmatches3 = NULL;
  bool donep = false;
  int nnew;
  double n5hits = 0.0, n3hits = 0.0;
  int nblocks5 = 0, nblocks3 = 0;
#ifdef DEBUG1
  int i;
#endif

  Block_reset_ends(this->block5);
  Block_reset_ends(this->block3);

  debug(printf("*** Starting stage1 with matchsize %d ***\n",matchsize));
  debug(printf("    querystart = %d, queryend = %d\n",Block_querypos(this->block5),Block_querypos(this->block3)));

  *foundpairp = false;
  while (!donep && (*foundpairp) == false) {
    if (nblocks5 >= maxattempts && nblocks3 >= maxattempts) {
      donep = true;
    } else if (n5hits <= n3hits && nblocks5 < maxattempts) {
      if (Block_next(this->block5) == false) {
	donep = true;
      } else {
	this->querystart = Block_querypos(this->block5);
	if (this->processedp[this->querystart] == false) {
#ifdef PMAP
	  Block_process_oligo(&(this->plus_positions[this->querystart]),&(this->plus_npositions[this->querystart]),
			      &(this->minus_positions[this->querystart]),&(this->minus_npositions[this->querystart]),
			      this->block5,indexdb_fwd,indexdb_rev);
	  debug(printf("find_first_pair: Processing 5' position %d (aaindex %u): %d plus, %d minus",
		       this->querystart,Block_aaindex(this->block5),
		       this->plus_npositions[this->querystart],this->minus_npositions[this->querystart]));
#else
	  Block_process_oligo(&(this->plus_positions[this->querystart]),&(this->plus_npositions[this->querystart]),
			      &(this->minus_positions[this->querystart]),&(this->minus_npositions[this->querystart]),
			      this->block5,indexdb);
	  debug(printf("find_first_pair: Processing 5' position %d (forward %06X, revcomp %06X): %d plus, %d minus",
		       this->querystart,Block_forward(this->block5),Block_revcomp(this->block5),
		       this->plus_npositions[this->querystart],this->minus_npositions[this->querystart]));
#endif
	  debug1(
		 for (i = 0; i < this->plus_npositions[this->querystart]; i++) {
		   printf(" +%u",this->plus_positions[this->querystart][i]);
		 }
		 for (i = 0; i < this->minus_npositions[this->querystart]; i++) {
		   printf(" -%u",this->minus_positions[this->querystart][i]);
		 }
		 );
	  debug(printf("\n"));
	  this->processedp[this->querystart] = true;
	  nblocks5++;
	}
	newmatches5 = find_5prime_matches(&nnew,NULL,this,matchsize,
					  this->plus_positions,this->plus_npositions,
					  this->minus_positions,this->minus_npositions,chromosome_iit,
					  chrsubset,matchpool,this->querystart,this->querylength,
					  this->maxentries,true);
	if (nnew > 0) {
	  n5hits += 1.0/(double) (1 + nnew);
	  debug(printf("    n5hits: %.1f, n3hits: %.1f\n",n5hits,n3hits));
	  gregionlist = pair_up(&(*foundpairp),gregionlist,matchsize,newmatches5,NULL,
				this->matches5,this->matches3,chromosome_iit,this->querylength,this->maxtotallen,
				this->trimstart,this->trimend,this->trimlength);
	  this->matches5 = Matchpool_transfer(this->matches5,newmatches5);
	  newmatches5 = NULL;
	}
      }

    } else {
      if (Block_next(this->block3) == false) {
	donep = true;
      } else {
	this->queryend = Block_querypos(this->block3);
	if (this->processedp[this->queryend] == false) {
#ifdef PMAP
	  Block_process_oligo(&(this->plus_positions[this->queryend]),&(this->plus_npositions[this->queryend]),
			      &(this->minus_positions[this->queryend]),&(this->minus_npositions[this->queryend]),
			      this->block3,indexdb_fwd,indexdb_rev);
	  debug(printf("find_first_pair: Processing 3' position %d (aaindex %u): %d plus, %d minus",
		       this->queryend,Block_aaindex(this->block3),
		       this->plus_npositions[this->queryend],this->minus_npositions[this->queryend]));
#else
	  Block_process_oligo(&(this->plus_positions[this->queryend]),&(this->plus_npositions[this->queryend]),
			      &(this->minus_positions[this->queryend]),&(this->minus_npositions[this->queryend]),
			      this->block3,indexdb);
	  debug(printf("find_first_pair: Processing 3' position %d (forward %06X, revcomp %06X): %d plus, %d minus",
		       this->queryend,Block_forward(this->block3),Block_revcomp(this->block3),
		       this->plus_npositions[this->queryend],this->minus_npositions[this->queryend]));
#endif
	  debug1(
		 for (i = 0; i < this->plus_npositions[this->queryend]; i++) {
		   printf(" +%u",this->plus_positions[this->queryend][i]);
		 }
		 for (i = 0; i < this->minus_npositions[this->queryend]; i++) {
		   printf(" -%u",this->minus_positions[this->queryend][i]);
		 }
		 );
	  debug(printf("\n"));
	  this->processedp[this->queryend] = true;
	  nblocks3++;
	}
	newmatches3 = find_3prime_matches(&nnew,NULL,this,matchsize,
					  this->plus_positions,this->plus_npositions,
					  this->minus_positions,this->minus_npositions,
					  chromosome_iit,chrsubset,matchpool,this->queryend,
					  this->querylength,this->maxentries,true);
	if (nnew > 0) {
	  n3hits += 1.0/(double) (1 + nnew);
	  debug(printf("    n5hits: %.1f, n3hits: %.1f\n",n5hits,n3hits));
	  gregionlist = pair_up(&(*foundpairp),gregionlist,matchsize,NULL,newmatches3,
				this->matches5,this->matches3,chromosome_iit,this->querylength,this->maxtotallen,
				this->trimstart,this->trimend,this->trimlength);
	  this->matches3 = Matchpool_transfer(this->matches3,newmatches3);
	  newmatches3 = NULL;
	}
      }
    }
  }

  return gregionlist;
}


/************************************************************************/


/* A dangling match is one that has not been paired up with a match
   from the other end of the cDNA sequence.  A significant fraction of
   dangling matches may indicate the need to continue finding more
   matches from the other end.  Conversely, the absence of dangling
   matches on both ends indicates that no other possibilities exist in
   the genome. */

static int
count_dangling (List_T matches) {
  int ndangling = 0;
  Match_T match;
  List_T p;

  for (p = matches; p != NULL; p = p->rest) {
    match = (Match_T) p->first;
    if (Match_npairings(match) == 0) {
      ndangling++;
    }
  }
  return ndangling;
}

static double
dangling_pct (List_T matches) {
  double ndangling = 0.0, denominator = 0.0;
  Match_T match;
  List_T p;
  bool weightp = false;

  for (p = matches; p != NULL; p = p->rest) {
    match = (Match_T) p->first;
    if (Match_npairings(match) == 0) {
      ndangling += Match_weight(match);
    }
    if (Match_has_weight_p(match) == true) {
      denominator += Match_weight(match);
      weightp = true;
    }
  }
  if (weightp == false) {
    return 0.0;
  } else {
    debug(printf("(%f/%f) ",ndangling,denominator));
    return ndangling/denominator;
  }
}

static List_T
get_dangling (List_T matches, Matchpool_T matchpool) {
  List_T dangling = NULL, p;
  Match_T match;

  for (p = matches; p != NULL; p = p->rest) {
    match = (Match_T) p->first;
    if (Match_npairings(match) == 0) {
      dangling = Matchpool_push_existing(dangling,matchpool,match);
    }
  }
  return dangling;
}


/************************************************************************
 *   Procedures for solving using a heap
 ************************************************************************/

static void
read_oligos (T this, Sequence_T queryuc, int oligobase) {
  Reader_T reader;
  int querypos, leftreadshift;
  Oligostate_T last_state = INIT;
#ifdef PMAP
  unsigned int aaindex = 0U;
#else
  Storedoligomer_T forward = 0U, revcomp = 0U;
#endif
  Storedoligomer_T oligobase_mask;

  this->validp = (bool *) CALLOC(this->querylength,sizeof(bool));
#ifdef PMAP
  this->oligos = (Storedoligomer_T *) CALLOC(this->querylength,sizeof(Storedoligomer_T));
#else
  this->forward_oligos = (Storedoligomer_T *) CALLOC(this->querylength,sizeof(Storedoligomer_T));
  this->revcomp_oligos = (Storedoligomer_T *) CALLOC(this->querylength,sizeof(Storedoligomer_T));
#endif

  reader = Reader_new(Sequence_fullpointer(queryuc),/*querystart*/0,/*queryend*/this->querylength,
		      /*reader_overlap*/oligobase,/*dibasep*/false);
#ifndef PMAP
  leftreadshift = 32 - 2*oligobase; /* chars are shifted into left of 32 bit word */
  oligobase_mask = ~(~0U << 2*oligobase);
  debug(printf("oligobase is %d, oligobase_mask is %08X\n",oligobase,oligobase_mask));
#endif

  /* Prevents us from processing invalid query 12-mers */
  for (querypos = 0; querypos <= this->querylength - oligobase; querypos++) {
    this->validp[querypos] = false;
  }

  /* Note: leftshifting is done here, rather than in Oligo_lookup */
  while ((last_state = Oligo_next(last_state,&querypos,
#ifdef PMAP
				  &aaindex,
#else
				  &forward,&revcomp,oligobase,
#endif
				  reader,/*cdnaend*/FIVE)) != DONE) {
    
    this->validp[querypos] = true;
#ifdef PMAP
    this->oligos[querypos] = aaindex;
#else
    this->forward_oligos[querypos] = forward & oligobase_mask;
    this->revcomp_oligos[querypos] = (revcomp >> leftreadshift) & oligobase_mask;
#endif

#if 0
    if (this->forward_oligos[querypos] == 0U || this->revcomp_oligos[querypos] == 0U) {
      debug(printf("Position %d is poly-A or poly-T\n",querypos));
      this->polyat[querypos] = true;
    }
#endif

  }

  Reader_free(&reader);
  return;
}


struct Rep_T {
  Storedoligomer_T oligo;
  int querypos;
};

static int
Rep_compare (const void *a, const void *b) {
  struct Rep_T x = * (struct Rep_T *) a;
  struct Rep_T y = * (struct Rep_T *) b;

  if (x.oligo < y.oligo) {
    return -1;
  } else if (y.oligo < x.oligo) {
    return +1;
  } else if (x.querypos < y.querypos) {
    return -1;
  } else if (y.querypos < x.querypos) {
    return +1;
  } else {
    return 0;
  }
}

static void
identify_repeated_oligos (T this, int oligobase, int querylength) {
  struct Rep_T *reps;
  int querypos, i, j, k, n = 0, min, max;

  debug(printf("Starting identify_repeated_oligos\n"));

  reps = (struct Rep_T *) CALLOC(this->querylength,sizeof(struct Rep_T));
  for (querypos = 0; querypos <= this->querylength - oligobase; querypos++) {
    if (this->validp[querypos] == true) {
#ifdef PMAP
      reps[n].oligo = this->oligos[querypos];
#else
      reps[n].oligo = this->forward_oligos[querypos];
#endif
      reps[n].querypos = querypos;
      n++;
    }
  }
  
  qsort(reps,n,sizeof(struct Rep_T),Rep_compare);
#if 0
  /* Test for thread safety */
  for (i = 0; i < n-1; i++) {
    if (Rep_compare(&(reps[i]),&(reps[i+1])) > 0) {
      abort();
    }
  }
#endif

  for (i = 0, j = 1; j < n; i++, j++) {
    if (reps[j].oligo == reps[i].oligo) {
#ifdef PMAP
      debug(printf("Found repetition of oligo %u at %d and %d\n",
		   reps[i].oligo,reps[i].querypos,reps[j].querypos));
#else
      debug(printf("Found repetition of oligo %06X at %d and %d\n",
		   reps[i].oligo,reps[i].querypos,reps[j].querypos));
#endif

      if (reps[j].querypos - reps[i].querypos <= MIN_REPEAT) {
#if 0
	this->validp[reps[j].querypos] = false;
	this->validp[reps[i].querypos] = false;
	this->processedp[reps[j].querypos] = true;
	this->processedp[reps[i].querypos] = true;
#else
	/* Wiping out everything around the repeated oligos */
	if ((min = reps[i].querypos - oligobase) < 0) {
	  min = 0;
	}
	if ((max = reps[i].querypos + oligobase) > querylength) {
	  max = querylength;
	}
	for (k = min; k < max; k++) {
	  this->validp[k] = false;
	  this->processedp[k] = true;
	}

	if ((min = reps[j].querypos - oligobase) < 0) {
	  min = 0;
	}
	if ((max = reps[j].querypos + oligobase) > querylength) {
	  max = querylength;
	}
	for (k = min; k < max; k++) {
	  this->validp[k] = false;
	  this->processedp[k] = true;
	}
#endif

      }
    }
  }

  FREE(reps);
  return;
}


#if 0
static void
check_old_new (Genomicpos_T *old_positions, int old_npositions, Genomicpos_T *new_positions, int new_npositions) {
  int i;

  if (new_npositions != old_npositions) {
    abort();
  } else {
    for (i = 0; i < new_npositions; i++) {
      if (new_positions[i] != old_positions[i]) {
	abort();
      }
    }
  }
  return;
}
#endif


#define SIZELIMIT 1

#ifdef PMAP
static void
sample_oligos (T this, Indexdb_T indexdb_fwd, Indexdb_T indexdb_rev, int querylength, int oligobase) {
  int querypos;
  int plus_npositions, minus_npositions;
  Genomicpos_T *plus_positions, *minus_positions;

  for (querypos = 0; querypos < querylength - oligobase; querypos++) {
#if 0
    if (this->validp[querypos] == true && this->processedp[querypos] == true) {
      printf("sample_oligos: Querypos %d, oligo is %06X\n",querypos,this->oligos[querypos]);
      plus_positions = Indexdb_read_with_diagterm(&plus_npositions,indexdb_fwd,this->oligos[querypos],
						  /*diagterm*/querylength-querypos);
      check_old_new(this->plus_positions[querypos],this->plus_npositions[querypos],plus_positions,plus_npositions);

      minus_positions = Indexdb_read_with_diagterm(&minus_npositions,indexdb_rev,this->oligos[querypos],
						   /*diagterm*/querypos + INDEX1PART_AA*3);
      check_old_new(this->minus_positions[querypos],this->minus_npositions[querypos],minus_positions,minus_npositions);

    }
#endif

    /* FORMULA */
    if (this->validp[querypos] == true && this->processedp[querypos] == false) {
      this->plus_positions[querypos] = 
	Indexdb_read_with_diagterm(&(this->plus_npositions[querypos]),indexdb_fwd,this->oligos[querypos],
				   /*diagterm*/querylength-querypos);
      this->minus_positions[querypos] = 
	Indexdb_read_with_diagterm(&(this->minus_npositions[querypos]),indexdb_rev,this->oligos[querypos],
				   /*diagterm*/querypos + INDEX1PART_AA*3);
      debug(printf("Sampling at querypos %d, plus_npositions = %d, minus_npositions = %d\n",
		   querypos,this->plus_npositions[querypos],this->minus_npositions[querypos]));
      this->processedp[querypos] = true;
    }
  }

  return;
}
#else
static void
sample_oligos (T this, Indexdb_T indexdb, int querylength, int oligobase, int indexdb_size_threshold) {
  int querypos;
#if 0
  Genomicpos_T *plus_positions, *minus_positions;
  int plus_npositions, minus_npositions;
#endif

  for (querypos = 0; querypos < querylength - oligobase; querypos++) {
#if 0
    if (this->validp[querypos] == true && this->processedp[querypos] == true) {
      printf("sample_oligos: Querypos %d, oligos are %06X and %06X\n",querypos,this->forward_oligos[querypos],this->revcomp_oligos[querypos]);
      plus_positions = Indexdb_read_with_diagterm(&plus_npositions,indexdb,this->forward_oligos[querypos],
						  /*diagterm*/querylength-querypos);
      check_old_new(this->plus_positions[querypos],this->plus_npositions[querypos],plus_positions,plus_npositions);

      minus_positions = Indexdb_read_with_diagterm(&minus_npositions,indexdb,this->revcomp_oligos[querypos],
						   /*diagterm*/querypos + INDEX1PART);
      check_old_new(this->minus_positions[querypos],this->minus_npositions[querypos],minus_positions,minus_npositions);

    }
#endif

    /* FORMULA */
    if (this->validp[querypos] == true && this->processedp[querypos] == false) {
#ifdef SIZELIMIT
      this->plus_positions[querypos] = 
	Indexdb_read_with_diagterm_sizelimit(&(this->plus_npositions[querypos]),indexdb,this->forward_oligos[querypos],
					     /*diagterm*/querylength-querypos,indexdb_size_threshold);
      this->minus_positions[querypos] =
	Indexdb_read_with_diagterm_sizelimit(&(this->minus_npositions[querypos]),indexdb,this->revcomp_oligos[querypos],
					     /*diagterm*/querypos + INDEX1PART,indexdb_size_threshold);
#else
      this->plus_positions[querypos] = 
	Indexdb_read_with_diagterm(&(this->plus_npositions[querypos]),indexdb,this->forward_oligos[querypos],
				   /*diagterm*/querylength-querypos);
      this->minus_positions[querypos] =
	Indexdb_read_with_diagterm(&(this->minus_npositions[querypos]),indexdb,this->revcomp_oligos[querypos],
				   /*diagterm*/querypos + INDEX1PART);
#endif
      debug(printf("Sampling at querypos %d, plus_npositions = %d, minus_npositions = %d\n",
		   querypos,this->plus_npositions[querypos],this->minus_npositions[querypos]));
      this->processedp[querypos] = true;
    }
  }

  return;
}
#endif




#define PARENT(i) (i >> 1)
#define LEFT(i) (i << 1)
#define RIGHT(i) ((i << 1) | 1)


typedef struct Batch_T *Batch_T;
struct Batch_T {
  int querypos;
  int ndiagonals;
  Genomicpos_T diagonal;
  Genomicpos_T *diagonals;
};

static void
Batch_init (Batch_T batch, int querypos, Genomicpos_T *diagonals, int ndiagonals) {

  batch->querypos = querypos;
  batch->diagonals = diagonals;
  batch->diagonal = *diagonals;
  batch->ndiagonals = ndiagonals;

  return;
}


static void
min_heap_insert (Batch_T *heap, int *heapsize, Batch_T batch) {
  int querypos, i;
  Genomicpos_T diagonal;

  /* debug0(printf("Inserting into heap: diagonal %u at querypos %d\n",diagonal,querypos)); */

  querypos = batch->querypos;
  diagonal = batch->diagonal;

  i = ++(*heapsize);
  /* sort primarily by diagonal, then by querypos */
  while (i > 1 && (heap[PARENT(i)]->diagonal > diagonal || 
		   (heap[PARENT(i)]->diagonal == diagonal && heap[PARENT(i)]->querypos > querypos))) {
    heap[i] = heap[PARENT(i)];
    i = PARENT(i);
  }
  heap[i] = batch;

  return;
}

/* A diagonal segment */
typedef struct Segment_T *Segment_T;
struct Segment_T {
  Genomicpos_T diagonal;
  int chrnum;
  int querypos5;
  int querypos3;
  int median;
  int score;
  int noligomers;	    /* Needed to speed up single_mismatches */
};



/* Reduces diagonals to handle small indels */
static void
collapse_diagonals (Genomicpos_T **diagonals, int *ndiagonals, 
		    Storedoligomer_T *oligos, int oligobase, int querylength) {
  Batch_T batch, *heap, sentinel;
  struct Batch_T sentinel_struct, *batchpool;
  int maxsegments, heapsize = 0, i;
  int parenti, smallesti, righti;
  int querypos;
  Genomicpos_T diagonal, base_diagonal, last_diagonal;
#ifdef DEBUG6
  int j;
#endif

  debug6(printf("*** Starting collapse_diagonals ***\n"));

  /* Set up batches */
  maxsegments = 0;

  batchpool = (struct Batch_T *) CALLOC(querylength-oligobase+1,sizeof(struct Batch_T));
  heap = (Batch_T *) CALLOC(2*querylength+1+1,sizeof(Batch_T));
  for (querypos = 0, i = 0; querypos <= querylength - oligobase; querypos++) {
    if (ndiagonals[querypos] > 0) {
      maxsegments += ndiagonals[querypos];
#ifdef PMAP
      debug6(printf("Adding batch for querypos %d with %d diagonals and aaindex %u\n",
		    querypos,ndiagonals[querypos],oligos[querypos]));
#else
      debug6(printf("Adding batch for querypos %d with %d diagonals and oligo %06X\n",
		    querypos,ndiagonals[querypos],oligos[querypos]));
#endif
      batch = &(batchpool[i++]);
      Batch_init(batch,querypos,diagonals[querypos],ndiagonals[querypos]);
      min_heap_insert(heap,&heapsize,batch);
    }
  }

  if (maxsegments == 0) {
    FREE(heap);
    FREE(batchpool);
    return;
  }

  /* Set up rest of heap */
  sentinel_struct.querypos = querylength; /* infinity */
  sentinel_struct.diagonal = (Genomicpos_T) -1; /* infinity */
  sentinel = &sentinel_struct;
  for (i = heapsize+1; i <= 2*heapsize+1; i++) {
    heap[i] = sentinel;
  }


  /* Initialize loop */
  batch = heap[1];
  base_diagonal = last_diagonal = diagonal = batch->diagonal;
  debug6(printf("diagonal %u\n",diagonal));

  if (--batch->ndiagonals <= 0) {
    /* Use last entry in heap for insertion */
    batch = heap[heapsize];
    heap[heapsize--] = sentinel;

  } else {
    /* Use this batch for insertion (same querypos) */
    batch->diagonal = *(++batch->diagonals);
  }

  /* heapify */
  diagonal = batch->diagonal;
  parenti = 1;
  smallesti = 2 + (heap[3]->diagonal < heap[2]->diagonal);
  /* Note that diagonal will never exceed a sentinel diagonal */
  while (diagonal > heap[smallesti]->diagonal) {
    heap[parenti] = heap[smallesti];
    parenti = smallesti;
    smallesti = LEFT(parenti);
    righti = smallesti+1;
    smallesti += (heap[righti]->diagonal < heap[smallesti]->diagonal);
  }
  heap[parenti] = batch;


  /* Continue after initialization */ 
  while (heapsize > 0) {
    batch = heap[1];
    diagonal = batch->diagonal;
    debug6(printf("diagonal %u\n",diagonal));

#if 0
    /* Compare against last_diagonal, not base_diagonal to allow a chain */
    if (diagonal <= last_diagonal + COLLAPSE_DISTANCE) {
      /* Compress.  Modifies diagonals. */
      debug6(printf("  collapsing to %u\n",base_diagonal));
      *batch->diagonals = base_diagonal;
    } else {
      /* New base diagonal */
      base_diagonal = diagonal;
    }
    last_diagonal = diagonal;
#else
    /* Compare against base_diagonal to ignore chains */
    if (diagonal == base_diagonal) {
      /* Do nothing */
    } else if (diagonal <= base_diagonal + COLLAPSE_DISTANCE) {
      /* Compress.  Modifies diagonals. */
      debug6(printf("  collapsing to %u\n",base_diagonal));
      *batch->diagonals = base_diagonal;
    } else {
      /* New base diagonal */
      base_diagonal = diagonal;
    }
#endif

    if (--batch->ndiagonals <= 0) {
      /* Use last entry in heap for insertion */
      batch = heap[heapsize];
      heap[heapsize--] = sentinel;

    } else {
      /* Use this batch for insertion (same querypos) */
      batch->diagonal = *(++batch->diagonals);
    }

    /* heapify */
    diagonal = batch->diagonal;
    parenti = 1;
    smallesti = 2 + (heap[3]->diagonal < heap[2]->diagonal);
    /* Note that diagonal/querypos will never exceed a sentinel diagonal/querypos */
    while (diagonal > heap[smallesti]->diagonal) {
      heap[parenti] = heap[smallesti];
      parenti = smallesti;
      smallesti = LEFT(parenti);
      righti = smallesti+1;
      smallesti += (heap[righti]->diagonal < heap[smallesti]->diagonal);
    }
    heap[parenti] = batch;
    
  }

  /* Terminate loop. */
  FREE(heap);
  FREE(batchpool);

  return;
}


static struct Segment_T *
find_segments (int *nsegments, Genomicpos_T **diagonals, int *ndiagonals, 
	       Storedoligomer_T *oligos, int oligobase, int querylength,
	       IIT_T chromosome_iit, Chrsubset_T chrsubset,
	       Sequence_T queryseq, int threshold_score, bool plusp) {
  struct Segment_T *segments, *ptr;
  Batch_T batch, *heap, sentinel;
  struct Batch_T sentinel_struct, *batchpool;
  int maxsegments, heapsize = 0, i;
  int parenti, smallesti, righti;
  int querypos, last_querypos;
  Genomicpos_T diagonal, last_diagonal;
  int *ordered;
#ifdef DEBUG6
  int j;
#endif

  debug6(printf("*** Starting find_segments ***\n"));

  /* Set up batches */
  maxsegments = 0;

  batchpool = (struct Batch_T *) CALLOC(querylength-oligobase+1,sizeof(struct Batch_T));
  heap = (Batch_T *) CALLOC(2*querylength+1+1,sizeof(Batch_T));
  for (querypos = 0, i = 0; querypos <= querylength - oligobase; querypos++) {
    if (ndiagonals[querypos] > 0) {
      maxsegments += ndiagonals[querypos];
#ifdef PMAP
      debug6(printf("Adding batch for querypos %d with %d diagonals and aaindex %u\n",
		    querypos,ndiagonals[querypos],oligos[querypos]));
#else
      debug6(printf("Adding batch for querypos %d with %d diagonals and oligo %06X\n",
		    querypos,ndiagonals[querypos],oligos[querypos]));
#endif
      batch = &(batchpool[i++]);
      Batch_init(batch,querypos,diagonals[querypos],ndiagonals[querypos]);
      min_heap_insert(heap,&heapsize,batch);
    }
  }

  if (maxsegments == 0) {
    FREE(heap);
    FREE(batchpool);
    *nsegments = 0;
    return (struct Segment_T *) NULL;
  } else {
    ptr = segments = (struct Segment_T *) CALLOC(maxsegments,sizeof(struct Segment_T));
    ordered = (int *) CALLOC(i,sizeof(int));
  }

  /* Set up rest of heap */
  sentinel_struct.querypos = querylength; /* infinity */
  sentinel_struct.diagonal = (Genomicpos_T) -1; /* infinity */
  sentinel = &sentinel_struct;
  for (i = heapsize+1; i <= 2*heapsize+1; i++) {
    heap[i] = sentinel;
  }


  /* Initialize loop */
  batch = heap[1];
  last_querypos = querypos = batch->querypos;
  last_diagonal = diagonal = batch->diagonal;
  debug6(printf("diagonal %u, querypos %d\n",diagonal,querypos));
#ifdef PMAP
  ptr->score = 3*oligobase;
#else
  ptr->score = oligobase;
#endif
  ptr->querypos5 = ptr->querypos3 = querypos;
  ordered[0] = querypos;
  ptr->noligomers = 1;

  if (--batch->ndiagonals <= 0) {
    /* Use last entry in heap for insertion */
    batch = heap[heapsize];
    querypos = batch->querypos;
    heap[heapsize--] = sentinel;

  } else {
    /* Use this batch for insertion (same querypos) */
    batch->diagonal = *(++batch->diagonals);
  }

  /* heapify */
  diagonal = batch->diagonal;
  parenti = 1;
  smallesti = 2 + ((heap[3]->diagonal < heap[2]->diagonal) |
		   ((heap[3]->diagonal == heap[2]->diagonal) &
		    (heap[3]->querypos < heap[2]->querypos)));
  /* Note that diagonal/querypos will never exceed a sentinel diagonal/querypos */
  while (diagonal > heap[smallesti]->diagonal ||
	 (diagonal == heap[smallesti]->diagonal &&
	  querypos > heap[smallesti]->querypos)) {
    heap[parenti] = heap[smallesti];
    parenti = smallesti;
    smallesti = LEFT(parenti);
    righti = smallesti+1;
    smallesti += ((heap[righti]->diagonal < heap[smallesti]->diagonal) |
		  ((heap[righti]->diagonal == heap[smallesti]->diagonal) &
		   (heap[righti]->querypos < heap[smallesti]->querypos)));
  }
  heap[parenti] = batch;


  /* Continue after initializaation */ 
  while (heapsize > 0) {
    batch = heap[1];
    querypos = batch->querypos;
    diagonal = batch->diagonal;
    debug6(printf("diagonal %u, querypos %d\n",diagonal,querypos));

    if (diagonal == last_diagonal) {
      /* Continuation of exact match or substitution */
#ifdef PMAP
      if (querypos <= last_querypos + oligobase) {
	ptr->score += 3*(querypos - last_querypos);
      } else {
	ptr->score += 3*oligobase;
      }
#else
      if (querypos <= last_querypos + oligobase) {
	ptr->score += querypos - last_querypos;
      } else {
	ptr->score += oligobase;
      }
#endif

      if (querypos > last_querypos) {
	ordered[ptr->noligomers++] = querypos;
      }
      ptr->querypos3 = querypos;

    } else {
      /* End of diagonal */
      ptr->diagonal = last_diagonal;

      if (ptr->score >= threshold_score) {
	/* The above test allows shorter matches on either end to find splicing, and requires longer matches internally */
	debug6(printf("  => %c, score %d, noligomers %d, querypos %d..%d, median %d, position %u..%u\n",
		      plusp ? '+' : '-',ptr->score,ptr->noligomers,ptr->querypos5,ptr->querypos3,
		      (ordered[ptr->noligomers/2] + ordered[ptr->noligomers/2 - ((ptr->noligomers+1) % 2)])/2,
		      plusp ? ptr->diagonal + ptr->querypos5 - querylength : ptr->diagonal - ptr->querypos5,
		      plusp ? ptr->diagonal + ptr->querypos3 - querylength : ptr->diagonal - ptr->querypos3));
	ptr->median = (ordered[ptr->noligomers/2] + ordered[ptr->noligomers/2 - ((ptr->noligomers+1) % 2)])/2;
	ptr->querypos3 += oligobase;
	ptr++;
      }

      ptr->querypos5 = ptr->querypos3 = querypos;
      ordered[0] = querypos;
      last_diagonal = diagonal;

#ifdef PMAP
      ptr->score = 3*oligobase;
#else
      ptr->score = oligobase;
#endif
      ptr->noligomers = 1;
    }
    last_querypos = querypos;


    if (--batch->ndiagonals <= 0) {
      /* Use last entry in heap for insertion */
      batch = heap[heapsize];
      querypos = batch->querypos;
      heap[heapsize--] = sentinel;

    } else {
      /* Use this batch for insertion (same querypos) */
      batch->diagonal = *(++batch->diagonals);
    }

    /* heapify */
    diagonal = batch->diagonal;
    parenti = 1;
    debug7(printf("Comparing left %d/right %d: %u @ %d and %u @ %d\n",
		  2,3,heap[3]->diagonal,heap[3]->querypos,heap[4]->diagonal,heap[4]->querypos));
    smallesti = 2 + ((heap[3]->diagonal < heap[2]->diagonal) |
		     ((heap[3]->diagonal == heap[2]->diagonal) &
		      (heap[3]->querypos < heap[2]->querypos)));
    /* Note that diagonal/querypos will never exceed a sentinel diagonal/querypos */
    while (diagonal > heap[smallesti]->diagonal ||
	   (diagonal == heap[smallesti]->diagonal &&
	    querypos > heap[smallesti]->querypos)) {
      heap[parenti] = heap[smallesti];
      parenti = smallesti;
      smallesti = LEFT(parenti);
      righti = smallesti+1;
      debug7(printf("Comparing left %d/right %d (heapsize %d): %u @ %d and %u @ %d\n",
		    righti,smallesti,heapsize,heap[righti]->diagonal,
		    heap[righti]->querypos,heap[smallesti]->diagonal,
		    heap[smallesti]->querypos));
      smallesti += ((heap[righti]->diagonal < heap[smallesti]->diagonal) |
		    ((heap[righti]->diagonal == heap[smallesti]->diagonal) &
		     (heap[righti]->querypos < heap[smallesti]->querypos)));
    }
    heap[parenti] = batch;
    
  }

  /* Terminate loop. */
  ptr->diagonal = last_diagonal;
  if (ptr->score >= threshold_score) {
    /* The above test allows shorter matches on either end to find splicing, and requires longer matches internally */
    debug6(printf("  => %c, score %d, noligomers %d, querypos %d..%d, median %d, position %u..%u\n",
		  plusp ? '+' : '-',ptr->score,ptr->noligomers,ptr->querypos5,ptr->querypos3,
		  (ordered[ptr->noligomers/2] + ordered[ptr->noligomers/2 - ((ptr->noligomers+1) % 2)])/2,
		  plusp ? ptr->diagonal + ptr->querypos5 - querylength : ptr->diagonal - ptr->querypos5,
		  plusp ? ptr->diagonal + ptr->querypos3 - querylength : ptr->diagonal - ptr->querypos3));
    ptr->median = (ordered[ptr->noligomers/2] + ordered[ptr->noligomers/2 - ((ptr->noligomers+1) % 2)])/2;
    ptr->querypos3 += oligobase;
    ptr++;			/* Needed to get correct value for nsegments below */
  }

  FREE(heap);
  FREE(batchpool);

  *nsegments = ptr - segments;
  debug6(for (j = 0; j < *nsegments; j++) {
	   printf("diagonal %u\tquerypos %d..%d\tscore %d\tnoligomers %d\n",
		  segments[j].diagonal,segments[j].querypos5,segments[j].querypos3,
		  segments[j].score,segments[j].noligomers);
	   if (segments[j].querypos3 < segments[j].querypos5) {
	     abort();
	   }
	 });

  FREE(ordered);
  return segments;
}


static int **
compute_paths (int ***prev, struct Segment_T *segments, int nsegments, int maxexons, bool plusp) {
  int **scores;
  int *bestscore, score, intronadj, overlapadj;
  struct Segment_T *segmentj;
  int k, j, i, *besti;
  int medianj;
  Genomicpos_T diagonalj, intronlength;

  if (nsegments == 0) {
    *prev = (int **) NULL;
    return (int **) NULL;
  } else {
    *prev = (int **) CALLOC(maxexons+1,sizeof(int *));
    scores = (int **) CALLOC(maxexons+1,sizeof(int *));
    for (i = 1; i <= maxexons; i++) {
      (*prev)[i] = (int *) CALLOC(nsegments,sizeof(int));
      scores[i] = (int *) CALLOC(nsegments,sizeof(int));
    }
    bestscore = (int *) CALLOC(maxexons+1,sizeof(int));
    besti = (int *) CALLOC(maxexons+1,sizeof(int));
  }

  if (plusp == true) {
    for (j = 0; j < nsegments; j++) {
      segmentj = &(segments[j]);
      diagonalj = segmentj->diagonal;
      medianj = segmentj->median;

      /* k = 1 */
      scores[1][j] = segmentj->score;
      (*prev)[1][j] = -1;

      for (k = 2; k <= maxexons; k++) {
	bestscore[k] = 0;
	besti[k] = -1;
      }

      i = j-1;
      while (i >= 0 && segments[i].diagonal + HIGHINTRONLENGTH > diagonalj) {
	/* for plus, median should be ascending */
	if (segments[i].median < medianj) {
	  intronlength = diagonalj - segments[i].diagonal;
	  intronadj = intronlength/INTRONLEN_PENALTY;
	  overlapadj = 0;
	  if (segmentj->querypos5 < segments[i].querypos3) {
#ifdef PMAP
	    overlapadj = 3*(segments[i].querypos3 - segmentj->querypos5);
#else
	    overlapadj = (segments[i].querypos3 - segmentj->querypos5);
#endif
	  }
	  
	  if (segmentj->score > intronadj + overlapadj) {
	    /* Require that score be monotonically increasing */
	    for (k = 2; k <= maxexons; k++) {
	      if ((score = scores[k-1][i] - intronadj - overlapadj) > bestscore[k]) {
		bestscore[k] = score;
		besti[k] = i;
	      }
	    }

	    /* For k = maxexons, also check against the same level */
	    if ((score = scores[maxexons][i] - intronadj - overlapadj) > bestscore[maxexons]) {
	      bestscore[maxexons] = score;
	      besti[maxexons] = i;
	    }
	  }

	}
	i--;
      }

      for (k = 2; k <= maxexons; k++) {
	scores[k][j] = bestscore[k] + segmentj->score;
	(*prev)[k][j] = besti[k];
	debug8(printf("+#%d, nexons %d: diagonal %u, querypos %d..%d, median %d, score %u from previous #%d\n",
		      j,k,diagonalj,segmentj->querypos5,segmentj->querypos3,medianj,scores[k][j],besti[k]));
      }
    }

  } else {
    for (j = 0; j < nsegments; j++) {
      segmentj = &(segments[j]);
      diagonalj = segmentj->diagonal;
      medianj = segmentj->median;

      /* k = 1 */
      scores[1][j] = segmentj->score;
      (*prev)[1][j] = -1;

      for (k = 2; k <= maxexons; k++) {
	bestscore[k] = 0;
	besti[k] = -1;
      }

      i = j-1;
      while (i >= 0 && segments[i].diagonal + HIGHINTRONLENGTH > diagonalj) {
	/* for minus, median should be descending */
	if (segments[i].median > medianj) {
	  intronlength = diagonalj - segments[i].diagonal;
	  intronadj = intronlength/INTRONLEN_PENALTY;
	  overlapadj = 0;
	  if (segments[i].querypos5 < segmentj->querypos3) {
#ifdef PMAP
	    overlapadj = 3*(segmentj->querypos3 - segments[i].querypos5);
#else
	    overlapadj = (segmentj->querypos3 - segments[i].querypos5);
#endif
	  }
	  
	  if (segmentj->score > intronadj + overlapadj) {
	    /* Require that score be monotonically increasing */
	    for (k = 2; k <= maxexons; k++) {
	      if ((score = scores[k-1][i] - intronadj - overlapadj) > bestscore[k]) {
		bestscore[k] = score;
		besti[k] = i;
	      }
	    }

	    /* For k = maxexons, also check against the same level */
	    if ((score = scores[maxexons][i] - intronadj - overlapadj) > bestscore[maxexons]) {
	      bestscore[maxexons] = score;
	      besti[maxexons] = i;
	    }
	  }

	}
	i--;
      }

      for (k = 2; k <= maxexons; k++) {
	scores[k][j] = bestscore[k] + segmentj->score;
	(*prev)[k][j] = besti[k];
	debug8(printf("-#%d, nexons %d: diagonal %u, querypos %d..%d, median %d, score %u from previous #%d\n",
		      j,k,diagonalj,segmentj->querypos5,segmentj->querypos3,medianj,scores[k][j],besti[k]));
      }
    }
  }

  FREE(besti);
  FREE(bestscore);

  return scores;
}


static int *
find_best_scores (int **nthbest, int **plus_scores, int plus_nscores,
		  int **minus_scores, int minus_nscores, int maxexons,
		  int n) {
  int *bestscores;
  int ninserted = 0, k, j, i;
  int *heap, newscore;
  int parenti, smallesti, righti;

  bestscores = (int *) CALLOC(maxexons+1,sizeof(int));
  *nthbest = (int *) CALLOC(maxexons+1,sizeof(int));
  heap = (int *) CALLOC(2*n+1+1,sizeof(int));

  for (k = 1; k <= maxexons; k++) {
    /* Initialize heap */
    heap[1] = 0;
    for (i = 2; i < 2*n+1+1; i++) {
      heap[i] = 1000000000;
    }

    /* plus */
    for (j = 0; j < plus_nscores; j++) {
      if ((newscore = plus_scores[k][j]) > heap[1]) {
	if (ninserted < n) {
	  /* put new score at bottom and heapify up */
	  i = ++ninserted;
	  while (i > 1 && heap[PARENT(i)] > newscore) {
	    heap[i] = heap[PARENT(i)];
	    i = PARENT(i);
	  }
	  heap[i] = newscore;

	} else {
	  /* replace nth largest with new score and heapify down */
	  parenti = 1;
	  smallesti = 2 + (heap[3] < heap[2]);
	  while (newscore > heap[smallesti]) {
	    heap[parenti] = heap[smallesti];
	    parenti = smallesti;
	    smallesti = LEFT(parenti);
	    righti = smallesti+1;
	    smallesti += heap[righti] < heap[smallesti];
	  }
	  heap[parenti] = newscore;
	}
      }
    }

    /* minus */
    for (j = 0; j < minus_nscores; j++) {
      if ((newscore = minus_scores[k][j]) > heap[1]) {
	if (ninserted < n) {
	  /* put new score at bottom and heapify up */
	  i = ++ninserted;
	  while (i > 1 && heap[PARENT(i)] > newscore) {
	    heap[i] = heap[PARENT(i)];
	    i = PARENT(i);
	  }
	  heap[i] = newscore;

	} else {
	  /* replace nth largest with new score and heapify down */
	  parenti = 1;
	  smallesti = 2 + (heap[3] < heap[2]);
	  while (newscore > heap[smallesti]) {
	    heap[parenti] = heap[smallesti];
	    parenti = smallesti;
	    smallesti = LEFT(parenti);
	    righti = smallesti+1;
	    smallesti += heap[righti] < heap[smallesti];
	  }
	  heap[parenti] = newscore;
	}
      }
    }

    (*nthbest)[k] = heap[1];
    bestscores[k] = 0;
    for (j = 1; j <= ninserted; j++) {
      if (heap[j] > bestscores[k]) {
	bestscores[k] = heap[j];
      }
    }
    debug8(printf("For %d exons, best score is %d, %dth best is %d\n",bestscores[k],n,(*nthbest)[k]));
  }

  FREE(heap);
  return bestscores;
}




static List_T
find_good_paths (List_T gregionlist, int nexons, int *prev, int *scores, 
		 struct Segment_T *segments, int nsegments, int threshold_score,
		 IIT_T chromosome_iit, int querylength, int trimstart, int trimend, bool plusp) {
  int bestj, besti;
  int querystart, queryend;
  Genomicpos_T genomicstart, genomicend;

  debug9(printf("Starting find_good_paths with %d segments and threshold_score %d\n",nsegments,threshold_score));

  for (bestj = nsegments-1; bestj >= 0; bestj--) {
    debug9(printf("#%d: diagonal %u\tpathscore %d\tprev %d\n",
		  bestj,segments[bestj].diagonal,scores[bestj],prev[bestj]));
    if (scores[bestj] >= threshold_score) {
      /* Traceback and wipe out scores along the way, so we don't use them again */
      besti = bestj;
      debug9(printf("*diagonal %u\tquerypos %d..%d\tscore %d\tnoligomers %d\n",
		    segments[besti].diagonal,segments[besti].querypos5,segments[besti].querypos3,
		    segments[besti].score,segments[besti].noligomers));
      while (prev[besti] >= 0) {
	besti = prev[besti];
	/* scores[besti] = 0; -- wipe out */
	debug9(printf("*diagonal %u\tquerypos %d..%d\tscore %d\tnoligomers %d\n",
		      segments[besti].diagonal,segments[besti].querypos5,segments[besti].querypos3,
		      segments[besti].score,segments[besti].noligomers));
      }
      if (plusp == true) {
	querystart = segments[besti].querypos5;
	queryend = segments[bestj].querypos3;
	if (segments[besti].diagonal + querystart > querylength) {
	  genomicstart = segments[besti].diagonal + querystart - querylength;
	  genomicend = segments[bestj].diagonal + queryend - querylength;
	  debug9(printf("Pushing gregion for %d plus: %d..%d: %u..%u, score=%d\n",
			bestj,querystart,queryend,genomicstart,genomicend,scores[bestj]));
#ifdef PMAP
	  gregionlist = List_push(gregionlist,Gregion_new(nexons,genomicstart,genomicend,/*plusp*/true,chromosome_iit,
							  querystart,queryend,querylength,
							  /*matchsize*/INDEX1PART_AA,trimstart,trimend));
#else
	  gregionlist = List_push(gregionlist,Gregion_new(nexons,genomicstart,genomicend,/*plusp*/true,chromosome_iit,
							  querystart,queryend,querylength,
							  /*matchsize*/INDEX1PART,trimstart,trimend));
#endif
	}
      } else if (plusp == false) {
	querystart = segments[bestj].querypos5;
	queryend = segments[besti].querypos3;
	if (segments[besti].diagonal > queryend) {
	  genomicstart = segments[besti].diagonal - queryend;
	  genomicend = segments[bestj].diagonal - querystart;
	  debug9(printf("Pushing gregion for %d minus: %d..%d: %u..%u, score=%d\n",
			bestj,querystart,queryend,genomicstart,genomicend,scores[bestj]));
#ifdef PMAP
	  gregionlist = List_push(gregionlist,Gregion_new(nexons,genomicstart,genomicend,/*plusp*/false,chromosome_iit,
							  querystart,queryend,querylength,
							  /*matchsize*/INDEX1PART_AA,trimstart,trimend));
#else
	  gregionlist = List_push(gregionlist,Gregion_new(nexons,genomicstart,genomicend,/*plusp*/false,chromosome_iit,
							  querystart,queryend,querylength,
							  /*matchsize*/INDEX1PART,trimstart,trimend));
#endif
	}
      } else {
	fprintf(stderr,"Got strange value %d for plusp\n",plusp);
	abort();
      }
    }
  }

  debug9(printf("Returning %d gregions\n",List_length(gregionlist)));
  return gregionlist;
}



/* Probably don't want to iterate on 18-mers or shorter oligomers.  If
   we can't find a matching pair of 24-mers, it must be cross-species,
   so we should rely on our other method */

static List_T
scan_ends (T this,
#ifdef PMAP
	   Indexdb_T indexdb_fwd,
	   Indexdb_T indexdb_rev,
#else
	   Indexdb_T indexdb,
#endif
	   IIT_T chromosome_iit, Chrsubset_T chrsubset, Matchpool_T matchpool,
	   int stuttercycles, int stutterhits, int trimstart, int trimend, Genomicpos_T genome_totallength,
	   Diagnostic_T diagnostic, bool iteratep) {
  List_T gregionlist = NULL;
  double dangling5_pct, dangling3_pct;
  List_T dangling5, dangling3;
  bool foundpairp = false, loopp = true;
  int matchsize;
  int maxattempts = MAX_ATTEMPTS_UNIT;

  debug(printf("Starting scan_ends with trimstart %d, trimend %d\n",trimstart,trimend));

  matchsize = this->oligosize + this->oligosize;
  while (loopp && matchsize > this->oligosize && foundpairp == false) {
#ifdef PMAP
    gregionlist = find_first_pair(&foundpairp,gregionlist,this,matchsize,
				  indexdb_fwd,indexdb_rev,chromosome_iit,chrsubset,matchpool,
				  maxattempts);
    if (foundpairp == false) {
      matchsize -= this->oligosize;
      maxattempts += MAX_ATTEMPTS_UNIT;
      stutterhits *= 2;
    }
#else
    gregionlist = find_first_pair(&foundpairp,gregionlist,this,matchsize,
				  indexdb,chromosome_iit,chrsubset,matchpool,
				  maxattempts);
    if (foundpairp == false) {
      matchsize -= this->oligosize/2;
      maxattempts += MAX_ATTEMPTS_UNIT;
      stutterhits *= 2;
    }
#endif
    if (iteratep == false) {
      loopp = false;
    }
  }

  if (foundpairp == false) {
    debug(printf("*** No pair found ***\n"));

    diagnostic->firstpair_found_5 = 0;
    diagnostic->firstpair_found_3 = 0;
    diagnostic->stutter_searched_5 = diagnostic->firstpair_found_5;
    diagnostic->stutter_searched_3 = diagnostic->firstpair_found_3;
    diagnostic->stutter_nmatchpairs = 0;

    diagnostic->stutter_matches_5 = List_length(this->matches5);
    diagnostic->stutter_matches_3 = List_length(this->matches3);
    diagnostic->dangling_5 = count_dangling(this->matches5);
    diagnostic->dangling_3 = count_dangling(this->matches3);
    diagnostic->dangling_nmatchpairs = 0;

  } else {
    debug(printf("*** Found pair ***\n"));
    diagnostic->firstpair_found_5 = Block_querypos(this->block5);
    diagnostic->firstpair_found_3 = Block_querypos(this->block3);

#ifdef PMAP
    gregionlist = stutter(gregionlist,this,matchsize,indexdb_fwd,indexdb_rev,
			  chromosome_iit,chrsubset,matchpool,stuttercycles,stutterhits);
#else
    gregionlist = stutter(gregionlist,this,matchsize,indexdb,
			  chromosome_iit,chrsubset,matchpool,stuttercycles,stutterhits);
#endif

    diagnostic->stutter_searched_5 = Block_querypos(this->block5);
    diagnostic->stutter_searched_3 = Block_querypos(this->block3);
    diagnostic->stutter_nmatchpairs = List_length(gregionlist);
  
    diagnostic->stutter_matches_5 = List_length(this->matches5);
    diagnostic->stutter_matches_3 = List_length(this->matches3);
    diagnostic->dangling_5 = count_dangling(this->matches5);
    diagnostic->dangling_3 = count_dangling(this->matches3);

    dangling5_pct = dangling_pct(this->matches5);
    dangling3_pct = dangling_pct(this->matches3);
    debug(printf("Dangling on 5' end: %d/%d\n",
		 count_dangling(this->matches5),List_length(this->matches5)));
    debug(printf("Dangling on 3' end: %d/%d\n",
		 count_dangling(this->matches3),List_length(this->matches3)));

    if (dangling5_pct > MAX_DANGLING_PCT) {
      dangling5 = get_dangling(this->matches5,matchpool);
#ifdef PMAP
      gregionlist = fill_in_3(gregionlist,this,matchsize,dangling5,indexdb_fwd,indexdb_rev,
			      chromosome_iit,chrsubset,matchpool);
#else
      gregionlist = fill_in_3(gregionlist,this,matchsize,dangling5,indexdb,
			      chromosome_iit,chrsubset,matchpool);
#endif
      /* Not necessary to free */
      /* List_free(&dangling5); */
    }

    if (dangling3_pct > MAX_DANGLING_PCT) {
      dangling3 = get_dangling(this->matches3,matchpool);
#ifdef PMAP
      gregionlist = fill_in_5(gregionlist,this,matchsize,dangling3,indexdb_fwd,indexdb_rev,
			      chromosome_iit,chrsubset,matchpool);
#else
      gregionlist = fill_in_5(gregionlist,this,matchsize,dangling3,indexdb,
			      chromosome_iit,chrsubset,matchpool);
#endif
      /* Not necessary to free */
      /* List_free(&dangling3); */
    }

    gregionlist = Gregion_filter_unique(gregionlist);
    diagnostic->dangling_nmatchpairs = List_length(gregionlist);
  }

  debug(printf("Returning %d elements in gregionlist\n",List_length(gregionlist)));
  return gregionlist;
}


#if 0
static bool
sufficient_gregion_p (List_T gregionlist) {
  List_T p;
  Gregion_T gregion;

  debug(printf("Checking for sufficient gregion on %d gregions\n",List_length(gregionlist)));
  for (p = gregionlist; p != NULL; p = List_next(p)) {
    gregion = (Gregion_T) List_head(p);
    if (Gregion_sufficient_support(gregion) == true) {
      debug(printf("Sufficient support found\n"));
      return true;
    }
  }

  debug(printf("Sufficient support not found\n"));
  return false;
}
#endif


List_T
Stage1_compute (bool *lowidentityp, Sequence_T queryuc, 
#ifdef PMAP
		Indexdb_T indexdb_fwd,
		Indexdb_T indexdb_rev,
#else
		Indexdb_T indexdb,
#endif
		int indexdb_size_threshold, IIT_T chromosome_iit, Chrsubset_T chrsubset,
		Matchpool_T matchpool, int maxintronlen_bound, int maxtotallen_bound,
		int stuttercycles, int stutterhits, bool subsequencep, Genome_T genome,
		Genomicpos_T genome_totallength, Diagnostic_T diagnostic, Stopwatch_T stopwatch) {
  List_T gregionlist = NULL, p, q;
  T this = NULL;
  int trimlength, trimstart, trimend, maxtotallen, matchsize, maxentries, i;
  Genomicpos_T extension5, extension3;
  Gregion_T gregion;

  struct Segment_T *plus_segments = NULL, *minus_segments = NULL;
  int plus_nsegments, minus_nsegments, maxexons, k;
  int *nthbest, *bestscores;
  int **plus_prev = NULL, **plus_scores = NULL, **minus_prev = NULL, **minus_scores = NULL;

  *lowidentityp = false;
#ifdef DEBUG
  global_chromosome_iit = chromosome_iit;
#endif

  Stopwatch_start(stopwatch);
  diagnostic->sampling_rounds = 0;
  diagnostic->sampling_nskip = 0;

  debug(global_genome = genome);
  debug(queryuc_ptr = Sequence_fullpointer(queryuc));

  /* Don't multiply trimlength by 3 in PMAP */
  trimlength = Sequence_trimlength(queryuc);
  trimstart = Sequence_trim_start(queryuc);
  trimend = Sequence_trim_end(queryuc);
  debug(printf("At start of Stage1_compute, we have trimstart %d, trimend %d\n",trimstart,trimend));

  if (trimlength <= SINGLEEXONLENGTH) {
    maxtotallen = 40 + trimlength;
  } else {
    maxtotallen = trimlength*SLOPE;
    if (maxtotallen < 10000) {
      maxtotallen = 10000;
    } else if (maxtotallen > maxtotallen_bound) {
      maxtotallen = maxtotallen_bound;
    }
  }

  debug(fprintf(stderr,"trimlength = %d, maxtotallen = %d\n",trimlength,maxtotallen));

  /* Scan ends (find first pair and stutter) */
#ifdef PMAP
  matchsize = INDEX1PART_AA + INDEX1PART_AA;
#else
  matchsize = INDEX1PART + INDEX1PART;
#endif
  maxentries = MAXENTRIES;

  debug(printf("Finding first pair with matchsize = %d, maxentries = %d\n",matchsize,maxentries));

  this = Stage1_new(queryuc,maxtotallen,matchsize,maxentries);

#ifdef PMAP
  read_oligos(this,queryuc,/*oligobase*/INDEX1PART_AA);
  identify_repeated_oligos(this,/*oligobase*/INDEX1PART_AA,this->querylength);
  gregionlist = scan_ends(this,indexdb_fwd,indexdb_rev,chromosome_iit,chrsubset,matchpool,
			  stuttercycles,stutterhits,trimstart,trimend,genome_totallength,diagnostic,
			  /*iteratep*/false);
#else
  read_oligos(this,queryuc,/*oligobase*/INDEX1PART);
  identify_repeated_oligos(this,/*oligobase*/INDEX1PART,this->querylength);
  gregionlist = scan_ends(this,indexdb,chromosome_iit,chrsubset,matchpool,
			  stuttercycles,stutterhits,trimstart,trimend,genome_totallength,diagnostic,
			  /*iteratep*/false);
#endif
  debug(printf("\nDangling5 = %f, Dangling3 = %f\n",dangling_pct(this->matches5),dangling_pct(this->matches3)));

  if (gregionlist == NULL) {
    /* Don't use dangling to determine lowidentityp */
    *lowidentityp = true;
  }

  debug(printf("\nAfter scan_ends:\n"));
  debug(
	for (p = gregionlist; p != NULL; p = List_next(p)) {
	  gregion = (Gregion_T) List_head(p);
	  Gregion_print(gregion);
	}
	);
  debug(printf("End of scan_ends.\n\n"));

  /* Perform sampling, if necessary */
  if (gregionlist == NULL ||
      (Gregion_best_weight(gregionlist) < SUFFICIENT_FIRST_WEIGHT &&
       dangling_pct(this->matches5) > MAX_DANGLING_PCT &&
       dangling_pct(this->matches3) > MAX_DANGLING_PCT)) {

#if 0
    /* Start over by clearing gregionlist */
    for (p = gregionlist; p != NULL; p = List_next(p)) {
      gregion = (Gregion_T) List_head(p);
      Gregion_free(&gregion);
    }
    List_free(&gregionlist);
    gregionlist = (List_T) NULL;
#endif

    debug(printf("Starting sample_oligos\n"));
#ifdef PMAP
    sample_oligos(this,indexdb_fwd,indexdb_rev,this->querylength,/*oligobase*/INDEX1PART_AA);
    collapse_diagonals(this->plus_positions,this->plus_npositions,this->oligos,
		       /*oligobase*/INDEX1PART_AA,this->querylength);
    plus_segments = find_segments(&plus_nsegments,this->plus_positions,this->plus_npositions,
				  this->oligos,/*oligobase*/INDEX1PART_AA,this->querylength,
				  chromosome_iit,chrsubset,queryuc,/*threshold_score*/27,
				  /*plusp*/true);
    collapse_diagonals(this->minus_positions,this->minus_npositions,this->oligos,
		       /*oligobase*/INDEX1PART_AA,this->querylength);
    minus_segments = find_segments(&minus_nsegments,this->minus_positions,this->minus_npositions,
				   this->oligos,/*oligobase*/INDEX1PART_AA,this->querylength,
				   chromosome_iit,chrsubset,queryuc,/*threshold_score*/27,
				   /*plusp*/false);
#else
    sample_oligos(this,indexdb,this->querylength,/*oligobase*/INDEX1PART,indexdb_size_threshold);
    collapse_diagonals(this->plus_positions,this->plus_npositions,this->forward_oligos,
		       /*oligobase*/INDEX1PART,this->querylength);
    plus_segments = find_segments(&plus_nsegments,this->plus_positions,this->plus_npositions,
				  this->forward_oligos,/*oligobase*/INDEX1PART,this->querylength,
				  chromosome_iit,chrsubset,queryuc,/*threshold_score*/18,
				  /*plusp*/true);

    collapse_diagonals(this->minus_positions,this->minus_npositions,this->revcomp_oligos,
		       /*oligobase*/INDEX1PART,this->querylength);
    minus_segments = find_segments(&minus_nsegments,this->minus_positions,this->minus_npositions,
				  this->revcomp_oligos,/*oligobase*/INDEX1PART,this->querylength,
				   chromosome_iit,chrsubset,queryuc,/*threshold_score*/18,
				   /*plusp*/false);
#endif

    maxexons = 3;
    plus_scores = compute_paths(&plus_prev,plus_segments,plus_nsegments,maxexons,/*plusp*/true);
    minus_scores = compute_paths(&minus_prev,minus_segments,minus_nsegments,maxexons,/*plusp*/false);

    bestscores = find_best_scores(&nthbest,plus_scores,/*plus_nscores*/plus_nsegments,
				  minus_scores,/*minus_nscores*/minus_nsegments,maxexons,/*n*/NBEST);

    for (k = 1; k <= maxexons; k++) {
      if (nthbest[k] < bestscores[k] - SUBOPTIMAL) {
	nthbest[k] = bestscores[k] - SUBOPTIMAL;
      }
      if (plus_nsegments > 0) {
	gregionlist = find_good_paths(gregionlist,/*nexons*/k,plus_prev[k],plus_scores[k],
				      plus_segments,plus_nsegments,/*threshold_score*/nthbest[k],
				      chromosome_iit,this->querylength,trimstart,trimend,/*plusp*/true);
      }
      if (minus_nsegments > 0) {
	gregionlist = find_good_paths(gregionlist,/*nexons*/k,minus_prev[k],minus_scores[k],
				      minus_segments,minus_nsegments,/*threshold_score*/nthbest[k],
				      chromosome_iit,this->querylength,trimstart,trimend,/*plusp*/false);
      }
    }

    FREE(nthbest);
    FREE(bestscores);
    if (minus_nsegments > 0) {
      for (k = 1; k <= maxexons; k++) {
	FREE(minus_scores[k]);
	FREE(minus_prev[k]);
      }
      FREE(minus_scores);
      FREE(minus_prev);
    }
    if (plus_nsegments > 0) {
      for (k = 1; k <= maxexons; k++) {
	FREE(plus_scores[k]);
	FREE(plus_prev[k]);
      }
      FREE(plus_scores);
      FREE(plus_prev);
    }
    FREE(minus_segments);
    FREE(plus_segments);
  }

  /* Clean up gregionlist */
  debug(printf("Starting extensions\n"));
  for (p = gregionlist; p != NULL; p = List_next(p)) {
    gregion = (Gregion_T) List_head(p);
    if (Gregion_extendedp(gregion) == false) {
      /* Need to extend, otherwise we won't align NM_003360 */
      find_extensions(&extension5,&extension3,this,gregion,maxintronlen_bound,/*continuousp*/false);
      Gregion_extend(gregion,extension5,extension3,this->querylength);
    }
  }
  debug(printf("Finished with extensions\n"));

  debug(printf("Before filtering, %d regions\n",List_length(gregionlist)));
  debug(
	for (p = gregionlist; p != NULL; p = List_next(p)) {
	  gregion = (Gregion_T) List_head(p);
	  Gregion_print(gregion);
	}
	);

#if 0
  /* Don't filter for support anymore */
  gregionlist = Gregion_filter_support(gregionlist,BOUNDARY_SUPPORT,PCT_MAX_SUPPORT,DIFF_MAX_SUPPORT);
  debug(printf("After filtering for support, %d regions\n",List_length(gregionlist)));
#endif

#if 0
  /* Don't filter for max_regions anymore */
  if (List_length(gregionlist) > MAX_GREGIONS_PRE_UNIQUE) {
    debug("Too many gregions, so erasing them\n");
    for (p = gregionlist; p != NULL; p = List_next(p)) {
      gregion = (Gregion_T) List_head(p);
      Gregion_free(&gregion);
    }
    List_free(&gregionlist);
    gregionlist = NULL;

  } else {
#endif

    gregionlist = Gregion_filter_unique(gregionlist);
    debug(printf("After filtering for unique, %d regions\n",List_length(gregionlist)));
    debug(
	  for (p = gregionlist; p != NULL; p = List_next(p)) {
	    gregion = (Gregion_T) List_head(p);
	    Gregion_print(gregion);
	  }
	  );

    debug(
	  if (List_length(gregionlist) > MAX_GREGIONS_POST_UNIQUE) {
	    printf("Too many gregions %d, so taking the top %d\n",List_length(gregionlist),MAX_GREGIONS_POST_UNIQUE);
	  });

    for (p = gregionlist, i = 1; p != NULL && i < MAX_GREGIONS_POST_UNIQUE; p = List_next(p)) {
      /* Advance */
      i++;
    }
    if (p != NULL) {
      q = List_next(p);
      p->rest = (List_T) NULL;
      for (p = q; p != NULL; p = List_next(p)) {
	gregion = (Gregion_T) List_head(p);
	Gregion_free(&gregion);
      }
      List_free(&q);
    }
#if 0
  }
#endif

  diagnostic->stage1_runtime = Stopwatch_stop(stopwatch);
  diagnostic->ngregions = List_length(gregionlist);

  Stage1_free(&this);

  debug(printf("Stage 1 returning %d regions\n",List_length(gregionlist)));
  return gregionlist;
}

