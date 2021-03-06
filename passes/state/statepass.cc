/*************************************************************************
 *
 *  This file is part of the ACT library
 *
 *  Copyright (c) 2020 Rajit Manohar
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA  02110-1301, USA.
 *
 **************************************************************************
 */
#include <stdio.h>
#include <string.h>
#include <map>
#include <utility>
#include <config.h>
#include <act/passes/statepass.h>

static act_connection *_inv_hash (struct iHashtable *H, int idx)
{
  ihash_iter_t iter;
  ihash_bucket_t *ib;
  
  ihash_iter_init (H, &iter);
  while ((ib = ihash_iter_next (H, &iter))) {
    if (ib->i == idx) {
      return (act_connection *)ib->key;
    }
  }
  return NULL;
}

void *ActStatePass::local_op (Process *p, int mode)
{
  if (mode == 0) {
    return countLocalState (p);
  }
  else if (mode == 1) {
    printLocal (_fp, p);
  }
  return getMap (p);
}

/*
 * Count all the local booleans, and create a map from connection
 * pointers corresponding to them to integers.
 *
 * FIXME: deal with globals correctly.
 *
 *
 */
stateinfo_t *ActStatePass::countLocalState (Process *p)
{
  act_boolean_netlist_t *b;

  b = bp->getBNL (p);
  if (!b) {
    fatal_error ("Process `%s' does not have a booleanized view?",
		 p->getName());
  }

  if (_black_box_mode && p && p->isBlackBox()) {
    /* Black box module */
    return NULL;
  }

  Assert (b->cH, "Hmm");

  /* 
     The booleanized vars come in two flavors:
      - the raw booleans
      - the chp booleans

     1. b->cH->n == local variables used
     2. port list: variables that are out of consideration

     Once we have the set of variables, then we need state only for
     output variables.
  */
  int nvars = 0;
  int bool_count = 0;
  int chp_count = 0;

  /* count non-global variables that are used */
  int alt_portbools = 0;
  chp_offsets alt_portchp;
  alt_portchp.bools = 0;
  alt_portchp.ints = 0;
  alt_portchp.chans = 0;

  ihash_bucket_t *hb;
  ihash_iter_t iter;
  
  ihash_iter_init (b->cH, &iter);
  while ((hb = ihash_iter_next (b->cH, &iter))) {
    act_booleanized_var_t *v = (act_booleanized_var_t *)hb->v;
    if (v->used && !v->isglobal) {
      bool_count++;
      if (v->isport) {
	alt_portbools++;
      }
    }
    if (!v->used && v->usedchp && !v->isglobal) {
      /* variables that are used in chp that are not used in the
	 booleanized version */
      chp_count++;
      if (v->ischpport) {
	if (v->isint) {
	  alt_portchp.ints++;
	}
	else if (v->ischan) {
	  alt_portchp.chans++;
	}
	else {
	  alt_portchp.bools++;
	}
      }
    }
  }

  stateinfo_t *si;
  NEW (si, stateinfo_t);

  si->bnl = b;
  si->allbools = 0;
  si->map = NULL;
  si->ismulti = 0;
  si->nportbools = alt_portbools;

  /* init for hse/prs mode: only track bools */
  si->localbools = bool_count - si->nportbools;

  /* init for chp mode: bools, ints, chans; note that fractured ints
     are tracked as fractured ints. */

  si->chp_ismulti = 0;
  si->chpmap = NULL;
  si->nportchp = alt_portchp;
  si->nportchptot = alt_portchp.bools + alt_portchp.ints + alt_portchp.chans;
  si->localchp = chp_count - si->nportchptot;

#if 0
  printf ("%s: start \n", p->getName());
#endif  

  bitset_t *tmpbits;
  bitset_t *inpbits;
  
  if (si->localbools + si->nportbools > 0) {
    si->multi = bitset_new (si->localbools + si->nportbools);
    tmpbits = bitset_new (si->localbools + si->nportbools);
    inpbits = bitset_new (si->localbools + si->nportbools);
  }
  else {
    si->multi = NULL;
    tmpbits = NULL;
    inpbits = NULL;
  }

  bitset_t *tmpchp;
  bitset_t *inpchp;

  if (si->localchp + si->nportchptot > 0) {
    si->chpmulti = bitset_new (si->localchp + si->nportchptot);
    tmpchp = bitset_new (si->localchp + si->nportchptot);
    inpchp = bitset_new (si->localchp + si->nportchptot);
  }
  else {
    si->chpmulti = NULL;
    tmpchp = NULL;
    inpchp = NULL;
  }

  int idx = 0;
  int chpidx = 0;

  si->map = ihash_new (8);
  si->chpmap = ihash_new (4);

  /* map each connection pointer that corresponds to local state to an
     integer starting at zero
  */

  si->chp_all.bools = 0;
  si->chp_all.ints = 0;
  si->chp_all.chans = 0;

  ihash_bucket_t *ib;

  ihash_iter_init (b->cH, &iter);
  while ((ib = ihash_iter_next (b->cH, &iter))) {
    int found = 0;
    act_booleanized_var_t *v = (act_booleanized_var_t *) ib->v;
    int ocount = 0;

    if (v->used) {
      /* boolean state */
      if (v->isport) {
	/*-- in the port list; so port state, not local state --*/
	for (int k=0; k < A_LEN (b->ports); k++) {
	  if (b->ports[k].omit) continue;
	  if (ib->key == (long)b->ports[k].c) {
	    found = 1;
	    break;
	  }
	  ocount++;
	}
	Assert (found, "What?");
	ihash_bucket_t *x = ihash_add (si->map, ib->key);
	x->i = ocount - si->nportbools;
      }
      else if (!v->isglobal) {
	/*-- globals not handled here --*/
	ihash_bucket_t *x = ihash_add (si->map, ib->key);
	x->i = idx++;
	ocount = x->i + si->nportbools;
      }

#if 0
      ActId *id = ((act_connection *)ib->key)->toid();
      printf ("   var: ");
      id->Print (stdout);
      printf (" [out=%d]", v->output ? 1 : 0);
      printf ("\n");
#endif

      /*-- look for multi-drivers: globals are ignored --*/
      if (!v->isglobal) {
	if (v->output) {
	  if (bitset_tst (tmpbits, ocount)) {
	    /* found multi driver! */
	    bitset_set (si->multi, ocount);
#if 0
	    printf ("     -> multi-driver!\n");
#endif
	  }
	  else {
	    bitset_set (tmpbits, ocount);
	  }
	}
	else {
	  /* an input; mark it */
	  bitset_set (inpbits, ocount);
	}
      }
#if 0
      delete id;
#endif      
    }
      
    if (!v->used && v->usedchp) {
      /* extra chp state */
      if (v->ischpport) {
	for (int k=0; k < A_LEN (b->chpports); k++) {
	  if (b->chpports[k].omit) continue;
	  if (ib->key == (long)b->chpports[k].c) {
	    found = 1;
	    break;
	  }
	  {
	    ihash_bucket_t *xb = ihash_lookup (b->cH, (long)b->chpports[k].c);
	    act_booleanized_var_t *xv = (act_booleanized_var_t *)xb->v;
	    if (!xv->used) {
	      /* if it is used in the boolean pass, it's already
		 counted there */
	      ocount++;
	    }
	  }
	}
	Assert (found, "What?");
      }
      else if (!v->isglobal) {
	ihash_bucket_t *x = ihash_add (si->chpmap, ib->key);
	x->i = chpidx++;
	ocount = x->i + si->nportchptot;

	if (v->ischan) {
	  si->chp_all.chans++;
	}
	else if (v->isint) {
	  si->chp_all.ints++;
	}
	else {
	  si->chp_all.bools++;
	}
      }

#if 0
      ActId *id = ((act_connection *)ib->key)->toid();
      printf ("   var: ");
      id->Print (stdout);
      printf (" [out=%d]", v->output ? 1 : 0);
      printf ("\n");
#endif      
      if (!v->isglobal) {
	if (v->output) {
	  if (bitset_tst (tmpchp, ocount)) {
	    /* found multi driver! */
	    bitset_set (si->chpmulti, ocount);
#if 0
	    printf ("     -> multi-driver!\n");
#endif
	  }
	  else {
	    bitset_set (tmpchp, ocount);
	  }
	}
	else {
	  /* an input; mark it */
	  bitset_set (inpchp, ocount);
	}
      }
#if 0
      delete id;
#endif      
    }

  }

  Assert (idx == si->localbools, "What?");
  Assert (chpidx == si->localchp, "What?");
  Assert (si->localchp == si->chp_all.bools +
	  si->chp_all.ints + si->chp_all.chans, "What?");

  si->chp_local = si->chp_all;

#if 0
  printf ("%s: stats: %d local; %d port\n", p->getName(),
	  si->localbools, si->nportbools);
#endif

  /* sum up instance state, and compute offsets for each instance */
  si->allbools = si->localbools;

  ActInstiter i(p ? p->CurScope() : ActNamespace::Global()->CurScope());

  for (i = i.begin(); i != i.end(); i++) {
    ValueIdx *vx = *i;
    if (TypeFactory::isProcessType (vx->t)) {
      Process *x = dynamic_cast<Process *>(vx->t->BaseType());
      if (x->isExpanded()) {
	stateinfo_t *ti = (stateinfo_t *) getMap (x);
	int n_sub_bools;
	int n_sub_chp_bools;
	int n_sub_chp_ints;
	int n_sub_chp_chans;

	if (ti) {
	  n_sub_bools = ti->allbools;
	  n_sub_chp_bools = ti->chp_all.bools;
	  n_sub_chp_ints = ti->chp_all.ints;
	  n_sub_chp_chans = ti->chp_all.chans;
	}
	else {
	  /* black box */
	  act_boolean_netlist_t *bn = bp->getBNL (x);
	  Assert (bn, "What?");
	  n_sub_bools = 0;
	  n_sub_chp_chans = 0;
	  n_sub_chp_ints = 0;
	  n_sub_chp_bools = 0;
	}
	/* map valueidx pointer to the current bool offset */
	if (vx->t->arrayInfo()) {
	  si->allbools += vx->t->arrayInfo()->size()*n_sub_bools;
	}
	else {
	  si->allbools += n_sub_bools;
	}

	if (vx->t->arrayInfo()) {
	  si->chp_all.bools += vx->t->arrayInfo()->size()*n_sub_chp_bools;
	  si->chp_all.chans += vx->t->arrayInfo()->size()*n_sub_chp_chans;
	  si->chp_all.ints += vx->t->arrayInfo()->size()*n_sub_chp_ints;
	}
	else {
	  si->chp_all.bools += n_sub_chp_bools;
	  si->chp_all.chans += n_sub_chp_chans;
	  si->chp_all.ints += n_sub_chp_ints;
	}
      }
    }
  }

#if 0
  printf ("%s: fullstats\n", p->getName());
  printf ("  %d allbools\n", si->allbools);
  printf ("  %d chp_allbool\n", si->chp_all.bools);
  printf ("  %d chp_allchan\n", si->chp_all.chans);
  printf ("  %d chp_allint\n", si->chp_all.ints);
#endif
  

  /* now check for multi-drivers due to instances */
  int instcnt = 0;
  int chpinstcnt = 0;
  for (i = i.begin(); i != i.end(); i++) {
    ValueIdx *vx = *i;
    if (TypeFactory::isProcessType (vx->t)) {
      Process *x = dynamic_cast<Process *>(vx->t->BaseType());
      if (x->isExpanded()) {
	int ports_exist;
	act_boolean_netlist_t *sub;
	stateinfo_t *subsi;

	sub = bp->getBNL (x);
	subsi = getStateInfo (x);
	ports_exist = 0;
	for (int j=0; j < A_LEN (sub->ports); j++) {
	  if (sub->ports[j].omit == 0) {
	    ports_exist = 1;
	    break;
	  }
	}

	if (ports_exist) {
	  int sz;
	  if (vx->t->arrayInfo()) {
	    sz = vx->t->arrayInfo()->size();
	  }
	  else {
	    sz = 1;
	  }
	  
	  while (sz > 0) {
	    sz--;
	    for (int j=0; j < A_LEN (sub->ports); j++) {
	      act_connection *c;
	      ihash_bucket_t *bi;
	      int ocount;
	      if (sub->ports[j].omit) continue;

	      c = b->instports[instcnt];
	      bi = ihash_lookup (si->map, (long)c);

	      if (c->isglobal()) {
		/* ignore globals here */
		continue;
	      }
	      
	      if (bi) {
		ocount = bi->i + si->nportbools;
	      }
	      else {
		ocount = 0;
		for (int k=0; k < A_LEN (b->ports); k++) {
		  if (b->ports[k].omit) continue;
		  if (c == b->ports[k].c) {
		    break;
		  }
		  ocount++;
		}
		Assert (ocount < si->nportbools, "What?");
	      }
	      
	      if (!sub->ports[j].input) {
		if (bitset_tst (tmpbits, ocount)) {
		  /* found multi driver! */
		  bitset_set (si->multi, ocount);
		  if (subsi) {
		    /* could be NULL, if it is a black box */
		    subsi->ismulti = 1;
		  }
#if 0
		  printf (" *multi-driver: ");
		  ActId *id = c->toid();
		  id->Print (stdout);
		  delete id;
		  printf ("\n");
#endif		  
		}
		else {
		  bitset_set (tmpbits, ocount);
		}
	      }
	      else {
		bitset_set (inpbits, ocount);
	      }
	      instcnt++;
	    }
	  }
	}

	ports_exist = 0;
	for (int j=0; j < A_LEN (sub->chpports); j++) {
	  if (sub->chpports[j].omit == 0) {
	    ports_exist = 1;
	    break;
	  }
	}

	if (ports_exist) {
	  int sz;
	  if (vx->t->arrayInfo()) {
	    sz = vx->t->arrayInfo()->size();
	  }
	  else {
	    sz = 1;
	  }
	  
	  while (sz > 0) {
	    sz--;
	    for (int j=0; j < A_LEN (sub->chpports); j++) {
	      act_connection *c;
	      ihash_bucket_t *bi;
	      int ocount;
	      if (sub->chpports[j].omit) continue;

	      c = b->instchpports[chpinstcnt];

	      /* -- ignore globals -- */
	      if (c->isglobal()) continue;

	      ihash_bucket_t *xb = ihash_lookup (b->cH, (long)c);
	      if (xb) {
		act_booleanized_var_t *xv = (act_booleanized_var_t *) xb->v;
		if (xv->used) {
		/* handled in earlier pass */
		  continue;
		}
	      }
		
	      bi = ihash_lookup (si->chpmap, (long)c);
	      if (bi) {
		ocount = bi->i + si->nportchptot;
	      }
	      else {
		ocount = 0;
		for (int k=0; k < A_LEN (b->chpports); k++) {
		  if (b->chpports[k].omit) continue;
		  if (c == b->chpports[k].c) {
		    break;
		  }
		  ocount++;
		}
		Assert (ocount < si->nportchptot, "What?");
	      }
	      
	      if (!sub->chpports[j].input) {
		if (bitset_tst (tmpchp, ocount)) {
		  /* found multi driver! */
		  bitset_set (si->chpmulti, ocount);
		  if (subsi) {
		    /* could be NULL, if it is a black box */
		    subsi->chp_ismulti = 1;
		  }
#if 0
		  printf (" *multi-driver: ");
		  ActId *id = c->toid();
		  id->Print (stdout);
		  delete id;
		  printf ("\n");
#endif		  
		}
		else {
		  bitset_set (tmpchp, ocount);
		}
	      }
	      else {
		bitset_set (inpchp, ocount);
	      }
	      chpinstcnt++;
	    }
	  }
	}
      }
    }
  }

  if (Act::warn_no_local_driver) {
    /* now check if there is some local state that is actually never
       driven! */
    for (int i=0; i < si->localbools; i++) {
      if (bitset_tst (inpbits, i + si->nportbools) &&
	  !bitset_tst (tmpbits, i + si->nportbools)) {
	act_connection *tmpc = _inv_hash (si->map, i);
	Assert (tmpc, "How did we get here?");
	ActId *tmpid = tmpc->toid();
	fprintf (stderr, "WARNING: Process `%s': local variable `",
		 p ? p->getName() : "-toplevel-");
	tmpid->Print (stderr);
	fprintf (stderr, "': no driver\n");
	delete tmpid;
      }
    }

    for (int i=0; i < si->localchp; i++) {
      if (bitset_tst (inpchp, i + si->nportchptot) &&
	  !bitset_tst (tmpchp, i + si->nportchptot)) {
	act_connection *tmpc = _inv_hash (si->chpmap, i);
	Assert (tmpc, "How did we get here?");
	ActId *tmpid = tmpc->toid();
	fprintf (stderr, "WARNING: Process `%s': local variable `",
		 p ? p->getName() : "-toplevel-");
	tmpid->Print (stderr);
	fprintf (stderr, "': no driver\n");
	delete tmpid;
      }
    }
  }
  
  if (tmpbits) {
    bitset_free (tmpbits);
    bitset_free (inpbits);
  }

  if (tmpchp) {
    bitset_free (tmpchp);
    bitset_free (inpchp);
  }

  /* re-do CHP numbering */
  ihash_free (si->chpmap);
  si->chpmap = ihash_new (4);

  chp_offsets c_idx;
  c_idx.bools = 0;
  c_idx.ints = 0;
  c_idx.chans = 0;

  ihash_iter_init (b->cH, &iter);
  
  while ((ib = ihash_iter_next (b->cH, &iter))) {
    int found = 0;
    act_booleanized_var_t *v = (act_booleanized_var_t *) ib->v;
    int ocount = 0;

    if (v->used) {
      continue;
    }

    if (v->usedchp) {
      /* extra chp state */
      if (v->ischpport) {
	for (int k=0; k < A_LEN (b->chpports); k++) {
	  if (b->chpports[k].omit) continue;
	  if (ib->key == (long)b->chpports[k].c) {
	    found = 1;
	    break;
	  }
	  {
	    ihash_bucket_t *xb = ihash_lookup (b->cH, (long)b->chpports[k].c);
	    act_booleanized_var_t *xv = (act_booleanized_var_t *)xb->v;
	    if (!xv->used) {
	      /* if it is used in the boolean pass, it's already
		 counted there */
	      if (v->ischan) {
		if (xv->ischan) {
		  ocount++;
		}
	      }
	      else if (v->isint) {
		if (xv->isint) {
		  ocount++;
		}
	      }
	      else if (!(xv->isint || xv->ischan)) {
		ocount++;
	      }
	    }
	  }
	}
	Assert (found, "What?");
	ihash_bucket_t *x = ihash_add (si->chpmap, ib->key);
	if (v->ischan) {
	  x->i = ocount - si->nportchp.chans;
	}
	else if (v->isint) {
	  x->i = ocount - si->nportchp.ints;
	}
	else {
	  x->i = ocount - si->nportchp.bools - si->nportbools;
	}
      }
      else if (!v->isglobal) {
	ihash_bucket_t *x = ihash_add (si->chpmap, ib->key);
	if (v->ischan) {
	  x->i = c_idx.chans++;
	}
	else if (v->isint) {
	  x->i = c_idx.ints++;
	}
	else {
	  x->i = si->allbools + c_idx.bools++;
	}
      }
    }
  }
  
#if 0
  printf ("  bool-only:\n");
  printf ("   port: %d; local: %d; all: %d\n", si->nportbools,
	  si->localbools, si->allbools);
  printf ("  chp-extra:\n");
  printf ("   bool:: port: %d; all: %d\n",
	  si->nportchp.bools, si->chp_all.bools);
  printf ("   int::  port: %d; all: %d\n",
	  si->nportchp.ints, si->chp_all.ints);
  printf ("   chan:: port: %d; all: %d\n",
	  si->nportchp.chans, si->chp_all.chans);
  printf ("%s: done\n\n", p->getName());
#endif  

  return si;
}

ActStatePass::ActStatePass (Act *a) : ActPass (a, "collect_state")
{
  /*-- need the booleanize pass --*/
  ActPass *ap = a->pass_find ("booleanize");
  if (!ap) {
    bp = new ActBooleanizePass (a);
  }
  else {
    bp = dynamic_cast<ActBooleanizePass *>(ap);
    Assert (bp, "What?");
  }
  AddDependency ("booleanize");
  _black_box_mode = config_get_int ("net.black_box_mode");
}

void ActStatePass::free_local (void *v)
{
  stateinfo_t *s = (stateinfo_t *)v;

  if (s->map) {
    ihash_free (s->map);
    s->map = NULL;
  }
  if (s->multi) {
    bitset_free (s->multi);
  }
  if (s->chpmulti) {
    bitset_free (s->chpmulti);
  }
  if (s->chpmap) {
    ihash_free (s->chpmap);
  }
  FREE (s);
}

int ActStatePass::run (Process *p)
{
  return ActPass::run (p);
}

void ActStatePass::Print (FILE *fp, Process *p)
{
  if (!completed()) {
    warning ("ActStatePass::Print() called without pass being run");
    return;
  }
  _fp = fp;
  run_recursive (p, 1);
}

stateinfo_t *ActStatePass::getStateInfo (Process *p)
{
  if (!completed()) {
    return NULL;
  }
  void *v = getMap (p);
  return (stateinfo_t *) v;
}

void ActStatePass::printLocal (FILE *fp, Process *p)
{
  stateinfo_t *si;

  si = getStateInfo (p);

  fprintf (fp, "--- Process: %s ---\n", p ? p->getName() : "-toplevel-");

  if (!si) {
    act_boolean_netlist_t *bn = bp->getBNL (p);
    Assert (bn, "Hmm");
    fprintf (fp, "  ** black box **\n");
    fprintf (fp, "  portbools: %d\n", A_LEN (bn->ports));
  }
  else {
    fprintf (fp, "   nbools = %d, nvars = %d\n",
	     si->localbools, si->localchp);
    fprintf (fp, "  localbools: %d\n", si->localbools);
    fprintf (fp, "  portbools: %d\n", si->nportbools);
    fprintf (fp, "  ismulti: %d\n", si->ismulti);
    fprintf (fp, "  all booleans (incl. inst): %d\n", si->allbools);
  }
  fprintf (fp, "--- End Process: %s ---\n", p->getName());
}



int ActStatePass::getTypeOffset (stateinfo_t *si, act_connection *c,
				 int *offset, int *type)
{
  ihash_bucket_t *b;

  Assert (si && c && offset && type, "What?");

  b = ihash_lookup (si->map, (long)c);
  if (b) {
    *type = 0;
    *offset = b->i;
    return 1;
  }
  b = ihash_lookup (si->chpmap, (long)c);
  if (b) {
    act_booleanized_var_t *v;

    *offset = b->i;
    b = ihash_lookup (si->bnl->cH, (long)c);
    Assert (b, "What?");
    v = (act_booleanized_var_t *)b->v;
    if (v->ischan) {
      *type = 2;
    }
    else if (v->isint) {
      *type = 1;
    }
    else {
      *type = 0;
    }
    return 1;
  }
  return 0;
}
