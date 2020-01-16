// FIXME: PARTICLE MOVERS NEED TO BE OVERALLOCATED IN STRUCTORS TO
// ACCOUNT FOR SPLITTING THE MOVER ARRAY BETWEEN HOST AND PIPELINES

#define IN_spa

#define HAS_V4_PIPELINE
#define HAS_V8_PIPELINE
#define HAS_V16_PIPELINE

#include "spa_private.h"

#include "../../../util/pipelines/pipelines_exec.h"

#include "RAJA/RAJA.hpp"

//----------------------------------------------------------------------------//
// Reference implementation for an advance_p pipeline function which does not
// make use of explicit calls to vector intrinsic functions.
//----------------------------------------------------------------------------//

void
advance_p_pipeline_scalar( advance_p_pipeline_args_t * args,
                           int pipeline_rank,
                           int n_pipeline )
{
    particle_t           * ALIGNED(128) p0 = args->p0;
    accumulator_t        * ALIGNED(128) a0 = args->a0;
    const interpolator_t * ALIGNED(128) f0 = args->f0;
    const grid_t *                      g  = args->g;

    particle_t           * ALIGNED(32)  p;
    particle_mover_t     * ALIGNED(16)  pm;

    const float qdt_2mc        = args->qdt_2mc;
    const float cdt_dx         = args->cdt_dx;
    const float cdt_dy         = args->cdt_dy;
    const float cdt_dz         = args->cdt_dz;
    const float qsp            = args->qsp;
    const float one            = 1.;
    const float one_third      = 1./3.;
    const float two_fifteenths = 2./15.;

    int itmp, n, max_nm;

    // Determine which quads of particles quads this pipeline processes

    DISTRIBUTE( args->np, 16, pipeline_rank, n_pipeline, itmp, n );
    p = args->p0 + itmp;

    // Determine which movers are reserved for this pipeline
    // Movers (16 bytes) should be reserved for pipelines in at least
    // multiples of 8 such that the set of particle movers reserved for
    // a pipeline is 128-byte aligned and a multiple of 128-byte in
    // size.  The host is guaranteed to get enough movers to process its
    // particles with this allocation.

    max_nm = args->max_nm - (args->np&15);
    if( max_nm<0 ) max_nm = 0;
    DISTRIBUTE( max_nm, 8, pipeline_rank, n_pipeline, itmp, max_nm );
    if( pipeline_rank==n_pipeline ) max_nm = args->max_nm - itmp;
    pm   = args->pm + itmp;

    // TODO: these are not very thread safe
    int* nm = new int[1]; *nm  = 0;
    int ignore = 0;

    // Determine which accumulator array to use
    // The host gets the first accumulator array

    if( pipeline_rank!=n_pipeline )
        a0 += (1+pipeline_rank)*
            POW2_CEIL((args->nx+2)*(args->ny+2)*(args->nz+2),2);

    // Process particles for this pipeline

    using namespace RAJA::statement; // holds For and Lamda
    using fused_exec = RAJA::KernelPolicy<
      For<0, RAJA::loop_exec,
        Lambda<0>, // kernel 1
        Lambda<1>  // kernel 2
      >
    >;

    particle_t           * ALIGNED(32)  p_ = p;
    //for(;n;n--,p++) {
    //for(int i = 0; i < n; i++)
  RAJA::kernel_param<fused_exec>(
  //RAJA::make_tuple(RAJA::RangeSegment(0, n)),
  //RAJA::RangeSegment(0, n),
  RAJA::make_tuple(RAJA::RangeSegment(0, n)),
  RAJA::tuple<>(),
  //RAJA::tuple(),
  [=] RAJA_DEVICE (
      std::ptrdiff_t i
      )
    {
        float dx = p[i].dx;                             // Load position
        float dy = p[i].dy;
        float dz = p[i].dz;
        int ii   = p[i].i;

        // Interpolate E
        const interpolator_t* ALIGNED(16) f = f0 + ii;

        float hax  = qdt_2mc*(    ( f->ex    + dy*f->dexdy    ) +
                dz*( f->dexdz + dy*f->d2exdydz ) );
        float hay  = qdt_2mc*(    ( f->ey    + dz*f->deydz    ) +
                dx*( f->deydx + dz*f->d2eydzdx ) );
        float haz  = qdt_2mc*(    ( f->ez    + dx*f->dezdx    ) +
                dy*( f->dezdy + dx*f->d2ezdxdy ) );
        float cbx  = f->cbx + dx*f->dcbxdx;             // Interpolate B
        float cby  = f->cby + dy*f->dcbydy;
        float cbz  = f->cbz + dz*f->dcbzdz;
        float ux   = p[i].ux;                             // Load momentum
        float uy   = p[i].uy;
        float uz   = p[i].uz;
        ux  += hax;                               // Half advance E
        uy  += hay;
        uz  += haz;
        //p[i].ux  += hax;                               // Half advance E
        //p[i].uy  += hay;
        //p[i].uz  += haz;

        float v0   = qdt_2mc/sqrtf(one + (ux*ux + (uy*uy + uz*uz)));
        /**/                                      // Boris - scalars
        float v1   = cbx*cbx + (cby*cby + cbz*cbz);
        float v2   = (v0*v0)*v1;
        float v3   = v0*(one+v2*(one_third+v2*two_fifteenths));
        float v4   = v3/(one+v1*(v3*v3));
        v4  += v4;
        v0   = ux + v3*( uy*cbz - uz*cby );       // Boris - uprime
        v1   = uy + v3*( uz*cbx - ux*cbz );
        v2   = uz + v3*( ux*cby - uy*cbx );
        ux  += v4*( v1*cbz - v2*cby );            // Boris - rotation
        uy  += v4*( v2*cbx - v0*cbz );
        uz  += v4*( v0*cby - v1*cbx );
        ux  += hax;                               // Half advance E
        uy  += hay;
        uz  += haz;
        p[i].ux = ux;                               // Store momentum
        p[i].uy = uy;
        p[i].uz = uz;
    },

  [=] RAJA_DEVICE (
      std::ptrdiff_t i
      )
    //for(int i = 0; i < n; i++)
    {
        // Reload ux
        float ux   = p[i].ux;                             // Load momentum
        float uy   = p[i].uy;
        float uz   = p[i].uz;

        // Reload dx
        float dx   = p[i].dx;                             // Load position
        float dy   = p[i].dy;
        float dz   = p[i].dz;
        float v0   = one/sqrtf(one + (ux*ux+ (uy*uy + uz*uz)));
        /**/                                      // Get norm displacement
        ux  *= cdt_dx;
        uy  *= cdt_dy;
        uz  *= cdt_dz;
        ux  *= v0;
        uy  *= v0;
        uz  *= v0;
        v0   = dx + ux;                           // Streak midpoint (inbnds)
        float v1   = dy + uy;
        float v2   = dz + uz;
        float v3   = v0 + ux;                           // New position
        float v4   = v1 + uy;
        float v5   = v2 + uz;

        // FIXME-KJB: COULD SHORT CIRCUIT ACCUMULATION IN THE CASE WHERE QSP==0!
        if(  v3<=one &&  v4<=one &&  v5<=one &&   // Check if inbnds
                -v3<=one && -v4<=one && -v5<=one ) {

            // Common case (inbnds).  Note: accumulator values are 4 times
            // the total physical charge that passed through the appropriate
            // current quadrant in a time-step

            float q  = p[i].w;
            q *= qsp;
            p[i].dx = v3;                             // Store new position
            p[i].dy = v4;
            p[i].dz = v5;
            dx = v0;                                // Streak midpoint
            dy = v1;
            dz = v2;
            v5 = q*ux*uy*uz*one_third;              // Compute correction

            // Get accumulator
            int ii   = p[i].i;
            float* ALIGNED(16) a = (float *)( a0 + ii );

#     define ACCUMULATE_J(X,Y,Z,offset)                                 \
            v4  = q*u##X;   /* v2 = q ux                            */        \
            v1  = v4*d##Y;  /* v1 = q ux dy                         */        \
            v0  = v4-v1;    /* v0 = q ux (1-dy)                     */        \
            v1 += v4;       /* v1 = q ux (1+dy)                     */        \
            v4  = one+d##Z; /* v4 = 1+dz                            */        \
            v2  = v0*v4;    /* v2 = q ux (1-dy)(1+dz)               */        \
            v3  = v1*v4;    /* v3 = q ux (1+dy)(1+dz)               */        \
            v4  = one-d##Z; /* v4 = 1-dz                            */        \
            v0 *= v4;       /* v0 = q ux (1-dy)(1-dz)               */        \
            v1 *= v4;       /* v1 = q ux (1+dy)(1-dz)               */        \
            v0 += v5;       /* v0 = q ux [ (1-dy)(1-dz) + uy*uz/3 ] */        \
            v1 -= v5;       /* v1 = q ux [ (1+dy)(1-dz) - uy*uz/3 ] */        \
            v2 -= v5;       /* v2 = q ux [ (1-dy)(1+dz) - uy*uz/3 ] */        \
            v3 += v5;       /* v3 = q ux [ (1+dy)(1+dz) + uy*uz/3 ] */        \
            RAJA::atomicAdd< RAJA::omp_atomic >( &a[offset+0 ], v0); \
            RAJA::atomicAdd< RAJA::omp_atomic >( &a[offset+1 ], v1); \
            RAJA::atomicAdd< RAJA::omp_atomic >( &a[offset+2 ], v2); \
            RAJA::atomicAdd< RAJA::omp_atomic >( &a[offset+3 ], v3)

            //a[offset+0] += v0;
            //a[offset+1] += v1;
            //a[offset+2] += v2;
            //a[offset+3] += v3

            ACCUMULATE_J( x,y,z, 0 );
            ACCUMULATE_J( y,z,x, 4 );
            ACCUMULATE_J( z,x,y, 8 );

#     undef ACCUMULATE_J

        }
        else
        {                                    // Unlikely
            DECLARE_ALIGNED_ARRAY( particle_mover_t, 16, local_pm, 1 );
            local_pm->dispx = ux;
            local_pm->dispy = uy;
            local_pm->dispz = uz;

            // TODO: this could be something like i.. but that fails?!
            local_pm->i = i + itmp; //p_ - p0;

            if( move_p( p0, local_pm, a0, g, qsp ) ) { // Unlikely
                if( *nm<max_nm ) {
                    int local_nm = RAJA::atomicAdd< RAJA::omp_atomic >(nm, 1);
                    pm[local_nm++] = local_pm[0];
                }
                else {
                    //ignore++;                 // Unlikely
                } // if
            } // if
        }

    }
  );

    args->seg[pipeline_rank].pm        = pm;
    args->seg[pipeline_rank].max_nm    = max_nm;
    args->seg[pipeline_rank].nm        = *nm;
    args->seg[pipeline_rank].n_ignored = ignore;
}

//----------------------------------------------------------------------------//
// Top level function to select and call the proper advance_p pipeline
// function.
//----------------------------------------------------------------------------//

void
advance_p_pipeline( species_t * RESTRICT sp,
                    accumulator_array_t * RESTRICT aa,
                    const interpolator_array_t * RESTRICT ia )
{
  DECLARE_ALIGNED_ARRAY( advance_p_pipeline_args_t, 128, args, 1 );

  DECLARE_ALIGNED_ARRAY( particle_mover_seg_t, 128, seg, MAX_PIPELINE + 1 );

  int rank;

  if ( !sp || !aa || !ia || sp->g != aa->g || sp->g != ia->g )
  {
    ERROR( ( "Bad args" ) );
  }

  args->p0      = sp->p;
  args->pm      = sp->pm;
  args->a0      = aa->a;
  args->f0      = ia->i;
  args->seg     = seg;
  args->g       = sp->g;

  args->qdt_2mc = (sp->q*sp->g->dt)/(2*sp->m*sp->g->cvac);
  args->cdt_dx  = sp->g->cvac*sp->g->dt*sp->g->rdx;
  args->cdt_dy  = sp->g->cvac*sp->g->dt*sp->g->rdy;
  args->cdt_dz  = sp->g->cvac*sp->g->dt*sp->g->rdz;
  args->qsp     = sp->q;

  args->np      = sp->np;
  args->max_nm  = sp->max_nm;
  args->nx      = sp->g->nx;
  args->ny      = sp->g->ny;
  args->nz      = sp->g->nz;

  // Have the host processor do the last incomplete bundle if necessary.
  // Note: This is overlapped with the pipelined processing.  As such,
  // it uses an entire accumulator.  Reserving an entire accumulator
  // for the host processor to handle at most 15 particles is wasteful
  // of memory.  It is anticipated that it may be useful at some point
  // in the future have pipelines accumulating currents while the host
  // processor is doing other more substantive work (e.g. accumulating
  // currents from particles received from neighboring nodes).
  // However, it is worth reconsidering this at some point in the
  // future.

  EXEC_PIPELINES( advance_p, args, 0 );

  WAIT_PIPELINES();

  // FIXME: HIDEOUS HACK UNTIL BETTER PARTICLE MOVER SEMANTICS
  // INSTALLED FOR DEALING WITH PIPELINES.  COMPACT THE PARTICLE
  // MOVERS TO ELIMINATE HOLES FROM THE PIPELINING.

  sp->nm = 0;
  for( rank = 0; rank <= N_PIPELINE; rank++ )
  {
    if ( args->seg[rank].n_ignored )
    {
      WARNING( ( "Pipeline %i ran out of storage for %i movers",
                 rank, args->seg[rank].n_ignored ) );
    }

    if ( sp->pm + sp->nm != args->seg[rank].pm )
    {
      MOVE( sp->pm + sp->nm, args->seg[rank].pm, args->seg[rank].nm );
    }

    sp->nm += args->seg[rank].nm;
  }
}
