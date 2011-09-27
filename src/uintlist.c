static char rcsid[] = "$Id: uintlist.c,v 1.5 2010-02-03 18:19:20 twu Exp $";
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "uintlist.h"
#include <stdlib.h>
#include "mem.h"

#define T Uintlist_T
struct T {
  unsigned int first;
  T rest;
};

T
Uintlist_push (T list, unsigned int x) {
  T new = (T) MALLOC(sizeof(*new));
  
  new->first = x;
  new->rest = list;
  return new;
}

T
Uintlist_pop (T list, unsigned int *x) {
  T head;

  if (list) {
    head = list->rest;
    *x = list->first;
    FREE(list);
    return head;
  } else {
    return list;
  }
}
  
unsigned int
Uintlist_head (T list) {
  return list->first;
}

T
Uintlist_next (T list) {
  if (list) {
    return list->rest;
  } else {
    return NULL;
  }
}

void
Uintlist_head_set (T this, unsigned int x) {
  this->first = x;
  return;
}

void
Uintlist_free (T *list) {
  T prev;

  while ((prev = *list) != NULL) {
    *list = (*list)->rest;
    FREE(prev);
  }
}

T
Uintlist_reverse (T list) {
  T head = NULL, next;

  for ( ; list; list = next) {
    next = list->rest;
    list->rest = head;
    head = list;
  }
  return head;
}

int
Uintlist_length (T list) {
  int n;
  
  for (n = 0; list; list = list->rest) {
    n++;
  }
  return n;
}

unsigned int *
Uintlist_to_array (int *n, T list) {
  unsigned int *array;
  int i;

  *n = Uintlist_length(list);
  array = (unsigned int *) CALLOC(*n,sizeof(unsigned int));
  for (i = 0; i < *n; i++) {
    array[i] = list->first;
    list = list->rest;
  }
  return array;
}

T
Uintlist_copy (T list) {
  T head, *p = &head;

  for ( ; list; list = list->rest) {
    *p = (T) MALLOC(sizeof(**p));
    (*p)->first = list->first;
    p = &(*p)->rest;
  }
  *p = NULL;
  return head;
}

T
Uintlist_append (T list, T tail) {
  T *p = &list;

  while (*p) {
    p = &(*p)->rest;
  }
  *p = tail;
  return list;
}

unsigned int
Uintlist_last_value (T this) {
  T last = NULL, r;

  for (r = this; r != NULL; r = r->rest) {
    last = r;
  }
  return last->first;
}

unsigned int
Uintlist_index (T this, int index) {
  while (index-- > 0) {
    this = this->rest;
  }
  return this->first;
}


bool
Uintlist_find (T this, unsigned int value) {
  T r;

  for (r = this; r != NULL; r = r->rest) {
    if (r->first == value) {
      return true;
    }
  }
  return false;
}
