/* 
 * Written by:
 *   Kevin J. Bowers, Ph.D.
 *   Plasma Physics Group (X-1)
 *   Applied Physics Division
 *   Los Alamos National Lab
 * March/April 2004 - Revised and extened from earlier V4PIC versions
 *
 */

#include <string.h> /* For memset */
#include <species.h>

/* void sort_p( particle_t * RESTRICT ALIGNED p,
	     const int np,
	     const grid_t * RESTRICT g ) { */ 

void sort_p( struct _species_t * RESTRICT sp,
	     const grid_t * RESTRICT g ) {
  particle_t save_p, *src, *dest, *p;
  int np; 
  static int * ALIGNED next=NULL; 
  int * ALIGNED copy=NULL; 
  int i, j, nc;

  p =sp->p; 
  np=sp->np; 

  if( p==NULL ) { ERROR(("Bad particle array"));      return; }
  if( np<0    ) { ERROR(("Bad number of particles")); return; }
  if( g==NULL ) { ERROR(("Bad grid"));                return; }
  if( np==0   ) return; /* Do not need to sort */

  nc = (g->nx+2)*(g->ny+2)*(g->nz+2);

  /* Allocate the list allocation arrays */

  i = (nc+1)*sizeof(int);

  /* Making these into static to avoid heap shredding and implement 
     collision models by allowing access to copy array outside sort_p. */ 
  if ( !next ) {
    next = (int * ALIGNED)malloc_aligned( i, preferred_alignment );
    if( next==NULL ) {
      ERROR(("Failed to allocate next"));
      return;
    }
  }
  if ( !sp->copy ) {
    sp->copy = (int * ALIGNED)malloc_aligned( i, preferred_alignment );
    if( sp->copy==NULL ) {
      ERROR(("Failed to allocate copy"));
      free_aligned(next);
      return;
    }
  }  
  copy=sp->copy; 

  memset( next, 0, i );

  /* Count particles in each cell */
  for( i=0; i<np; i++ ) next[ p[i].i ]++;

  /* Convert the count to an allocation (and save a copy of the allocation) */
  j=0;
  for( i=0; i<=nc; i++ ) {
    copy[i] = j;
    j += next[i];
    next[i] = copy[i];
  }

  /* Run sort cycles until the list is sorted */
  i=0;
  while( i<nc ) {
    if( next[i]>=copy[i+1] ) i++;
    else {
      src = &p[ next[i] ];
      for(;;) {
        dest = &p[ next[ src->i ]++ ];
        if( src==dest ) break;
        save_p = *dest;
        *dest  = *src;
        *src   = save_p;
      }
    }
  }
}
