/*
 * Copyright (c) 2002 Matteo Frigo
 * Copyright (c) 2002 Steven G. Johnson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/* $Id: rank-geq2.c,v 1.5 2002-09-16 02:30:26 stevenj Exp $ */

/* plans for RDFT of rank >= 2 (multidimensional) */

/* FIXME: this solver cannot strictly be applied to multidimensional
   DHTs, since the latter are not separable...up to rnk-1 additional
   post-processing passes may be required.  See also:

   R. N. Bracewell, O. Buneman, H. Hao, and J. Villasenor, "Fast
   two-dimensional Hartley transform," Proc. IEEE 74, 1282-1283 (1986).

   H. Hao and R. N. Bracewell, "A three-dimensional DFT algorithm
   using the fast Hartley transform," Proc. IEEE 75(2), 264-266 (1987).
*/

#include "rdft.h"

typedef struct {
     solver super;
     int spltrnk;
     const int *buddies;
     uint nbuddies;
} S;

typedef struct {
     plan_rdft super;

     plan *cld1, *cld2;
     const S *solver;
} P;

/* Compute multi-dimensional RDFT by applying the two cld plans
   (lower-rnk RDFTs). */
static void apply(plan *ego_, R *I, R *O)
{
     P *ego = (P *) ego_;
     plan_rdft *cld1, *cld2;

     cld1 = (plan_rdft *) ego->cld1;
     cld1->apply(ego->cld1, I, O);

     cld2 = (plan_rdft *) ego->cld2;
     cld2->apply(ego->cld2, O, O);
}


static void awake(plan *ego_, int flg)
{
     P *ego = (P *) ego_;
     AWAKE(ego->cld1, flg);
     AWAKE(ego->cld2, flg);
}

static void destroy(plan *ego_)
{
     P *ego = (P *) ego_;
     X(plan_destroy)(ego->cld2);
     X(plan_destroy)(ego->cld1);
     X(free)(ego);
}

static void print(plan *ego_, printer *p)
{
     P *ego = (P *) ego_;
     const S *s = ego->solver;
     p->print(p, "(rdft-rank>=2/%d%(%p%)%(%p%))",
	      s->spltrnk, ego->cld1, ego->cld2);
}

static int picksplit(const S *ego, const tensor sz, uint *rp)
{
     A(sz.rnk > 1); /* cannot split rnk <= 1 */
     if (!X(pickdim)(ego->spltrnk, ego->buddies, ego->nbuddies, sz, 1, rp))
	  return 0;
     *rp += 1; /* convert from dim. index to rank */
     if (*rp >= sz.rnk) /* split must reduce rank */
	  return 0;
     return 1;
}

static int applicable(const solver *ego_, const problem *p_, uint *rp)
{
     if (RDFTP(p_)) {
          const problem_rdft *p = (const problem_rdft *) p_;
          const S *ego = (const S *)ego_;
          return (1
                  && p->sz.rnk >= 2
                  && picksplit(ego, p->sz, rp)
                  && (0

                      /* can always operate out-of-place */
                      || p->I != p->O

                      /* Can operate in-place as long as all dimension
			 strides are the same, provided that the child
			 plans work in-place.  (This condition is
			 sufficient, but is it necessary?) */
                      || X(tensor_inplace_strides)(p->sz)
		       )
	       );
     }

     return 0;
}

/* TODO: revise this. */
static int score(const solver *ego_, const problem *p_, const planner *plnr)
{
     const S *ego = (const S *)ego_;
     const problem_rdft *p = (const problem_rdft *) p_;
     uint dummy;

     if (!applicable(ego_, p_, &dummy))
          return BAD;

     /* fftw2 behavior */
     if (NO_RANK_SPLITSP(plnr) && (ego->spltrnk != ego->buddies[0]))
	  return BAD;

     /* Heuristic: if the vector stride is greater than the transform
        sz, don't use (prefer to do the vector loop first with a
        vrank-geq1 plan). */
     if (p->vecsz.rnk > 0 &&
	 X(tensor_min_stride)(p->vecsz) > X(tensor_max_index)(p->sz))
          return UGLY;

     return GOOD;
}

static plan *mkplan(const solver *ego_, const problem *p_, planner *plnr)
{
     const S *ego = (const S *) ego_;
     const problem_rdft *p;
     P *pln;
     plan *cld1 = 0, *cld2 = 0;
     tensor sz1, rsz1, sz2, vecszi, sz2i, rsz2i;
     problem *cldp;
     uint spltrnk;

     static const plan_adt padt = {
	  X(rdft_solve), awake, print, destroy
     };

     if (!applicable(ego_, p_, &spltrnk))
          return (plan *) 0;

     p = (const problem_rdft *) p_;
     X(tensor_split)(p->sz, &sz1, spltrnk, &sz2);
     vecszi = X(tensor_copy_inplace)(p->vecsz, INPLACE_OS);
     sz2i = X(tensor_copy_inplace)(sz2, INPLACE_OS);
     rsz1 = X(rdft_real_sz)(p->kind, sz1);
     rsz2i = X(rdft_real_sz)(p->kind + spltrnk, sz2i);

     cldp = X(mkproblem_rdft_d)(X(tensor_copy)(sz2),
				X(tensor_append)(p->vecsz, rsz1),
				p->I, p->O, p->kind + spltrnk);
     cld1 = MKPLAN(plnr, cldp);
     X(problem_destroy)(cldp);
     if (!cld1)
          goto nada;

     cldp = X(mkproblem_rdft_d)(X(tensor_copy_inplace)(sz1, INPLACE_OS),
				X(tensor_append)(vecszi, rsz2i),
				p->O, p->O, p->kind);
     cld2 = MKPLAN(plnr, cldp);
     X(problem_destroy)(cldp);
     if (!cld2)
          goto nada;

     pln = MKPLAN_RDFT(P, &padt, apply);

     pln->cld1 = cld1;
     pln->cld2 = cld2;

     pln->solver = ego;
     pln->super.super.ops = X(ops_add)(cld1->ops, cld2->ops);

     X(tensor_destroy)(rsz2i);
     X(tensor_destroy)(rsz1);
     X(tensor_destroy)(sz2);
     X(tensor_destroy)(sz1);
     X(tensor_destroy)(vecszi);
     X(tensor_destroy)(sz2i);

     return &(pln->super.super);

 nada:
     if (cld2)
          X(plan_destroy)(cld2);
     if (cld1)
          X(plan_destroy)(cld1);
     X(tensor_destroy)(rsz2i);
     X(tensor_destroy)(rsz1);
     X(tensor_destroy)(sz2);
     X(tensor_destroy)(sz1);
     X(tensor_destroy)(vecszi);
     X(tensor_destroy)(sz2i);
     return (plan *) 0;
}

static solver *mksolver(int spltrnk, const int *buddies, uint nbuddies)
{
     static const solver_adt sadt = { mkplan, score };
     S *slv = MKSOLVER(S, &sadt);
     slv->spltrnk = spltrnk;
     slv->buddies = buddies;
     slv->nbuddies = nbuddies;
     return &(slv->super);
}

void X(rdft_rank_geq2_register)(planner *p)
{
     uint i;
     static const int buddies[] = { 0, 1, -2 };

     const uint nbuddies = sizeof(buddies) / sizeof(buddies[0]);

     for (i = 0; i < nbuddies; ++i)
          REGISTER_SOLVER(p, mksolver(buddies[i], buddies, nbuddies));

     /* FIXME: Should we try more buddies?  See also dft/rank-geq2. */
}
