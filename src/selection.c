/*
 * Copyright (c) 2012, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * selection.c
 *
 */

#define _GNU_SOURCE
#include <string.h>
#include <fnmatch.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "selection.h"
#include "solver.h"
#include "evr.h"


static int
str2archid(Pool *pool, const char *arch)
{
  Id id;
  if (!*arch)
    return 0;
  id = pool_str2id(pool, arch, 0);
  if (!id || id == ARCH_SRC || id == ARCH_NOSRC || id == ARCH_NOARCH)
    return id;
  if (pool->id2arch && (id > pool->lastarch || !pool->id2arch[id]))
    return 0;
  return id;
}

/* remove empty jobs from the selection */
static void
selection_prune(Pool *pool, Queue *selection)
{
  int i, j;
  Id p, pp;
  for (i = j = 0; i < selection->count; i += 2)
    {
      Id select = selection->elements[i] & SOLVER_SELECTMASK;
      p = 0;
      if (select == SOLVER_SOLVABLE_ALL)
	p = 1;
      else if (select == SOLVER_SOLVABLE_REPO)
	{
	  Solvable *s;
	  Repo *repo = pool_id2repo(pool, selection->elements[i + 1]);
	  if (repo)
	    {
	      FOR_REPO_SOLVABLES(repo, p, s)
	        break;
	    }
	}
      else
	{
	  FOR_JOB_SELECT(p, pp, select, selection->elements[i + 1])
	    break;
	}
      if (!p)
	continue;
      selection->elements[j] = selection->elements[i];
      selection->elements[j + 1] = selection->elements[i + 1];
      j += 2;
    }
  queue_truncate(selection, j);
}


static int
selection_solvables_sortcmp(const void *ap, const void *bp, void *dp)
{
  return *(const Id *)ap - *(const Id *)bp;
}

void
selection_solvables(Pool *pool, Queue *selection, Queue *pkgs)
{
  int i, j;
  Id p, pp, lastid;
  queue_empty(pkgs);
  for (i = 0; i < selection->count; i += 2)
    {
      Id select = selection->elements[i] & SOLVER_SELECTMASK;
      if (select == SOLVER_SOLVABLE_ALL)
	{
	  FOR_POOL_SOLVABLES(p)
	    queue_push(pkgs, p);
	}
      if (select == SOLVER_SOLVABLE_REPO)
	{
	  Solvable *s;
	  Repo *repo = pool_id2repo(pool, selection->elements[i + 1]);
	  if (repo)
	    {
	      FOR_REPO_SOLVABLES(repo, p, s)
	        queue_push(pkgs, p);
	    }
	}
      else
	{
	  FOR_JOB_SELECT(p, pp, select, selection->elements[i + 1])
	    queue_push(pkgs, p);
	}
    }
  if (pkgs->count < 2)
    return;
  /* sort and unify */
  solv_sort(pkgs->elements, pkgs->count, sizeof(Id), selection_solvables_sortcmp, NULL);
  lastid = pkgs->elements[0];
  for (i = j = 1; i < pkgs->count; i++)
    if (pkgs->elements[i] != lastid)
      pkgs->elements[j++] = lastid = pkgs->elements[i];
  queue_truncate(pkgs, j);
}

static void
selection_flatten(Pool *pool, Queue *selection)
{
  Queue q;
  int i;
  if (selection->count <= 2)
    return;
  for (i = 0; i < selection->count; i += 2)
    if ((selection->elements[i] & SOLVER_SELECTMASK) == SOLVER_SOLVABLE_ALL)
      {
	selection->elements[0] = selection->elements[i];
	selection->elements[1] = selection->elements[i + 1];
	queue_truncate(selection, 2);
	return;
      }
  queue_init(&q);
  selection_solvables(pool, selection, &q);
  if (!q.count)
    {
      queue_empty(selection);
      return;
    }
  queue_truncate(selection, 2);
  if (q.count > 1)
    {
      selection->elements[0] = SOLVER_SOLVABLE_ONE_OF;
      selection->elements[1] = pool_queuetowhatprovides(pool, &q);
    }
  else
    {
      selection->elements[0] = SOLVER_SOLVABLE | SOLVER_NOAUTOSET;
      selection->elements[1] = q.elements[0];
    }
}

/* only supports simple rels plus REL_ARCH */
static int
match_nevr_rel(Pool *pool, Solvable *s, Id rflags, Id revr)
{
  if (rflags == REL_ARCH)
    {
      if (s->arch != revr) 
	{
	  if (revr != ARCH_SRC || s->arch != ARCH_NOSRC)
	    return 0;
	}
      return 1;
    }
  if (rflags > 7)
    return 0;
  return pool_intersect_evrs(pool, REL_EQ, s->evr, rflags, revr);
}

/* only supports simple rels plus REL_ARCH */
/* prunes empty jobs */
static void
selection_filter_rel(Pool *pool, Queue *selection, Id relflags, Id relevr)
{
  int i;

  if (!selection->count)
    return;

  for (i = 0; i < selection->count; i += 2)
    {
      Id select = selection->elements[i] & SOLVER_SELECTMASK;
      Id id = selection->elements[i + 1];
      if (select == SOLVER_SOLVABLE || select == SOLVER_SOLVABLE_ONE_OF)
	{
	  /* done by selection_addextra, currently implies SELECTION_NAME */
	  Queue q;
	  Id p, pp;
	  int miss = 0;

	  queue_init(&q);
	  FOR_JOB_SELECT(p, pp, select, id)
	    {
	      Solvable *s = pool->solvables + p;
	      if (match_nevr_rel(pool, s, relflags, relevr))
	        queue_push(&q, p);
	      else
		miss = 1;
	    }
	  if (miss)
	    {
	      if (q.count == 1)
		{
		  selection->elements[i] = SOLVER_SOLVABLE | SOLVER_NOAUTOSET;
		  selection->elements[i + 1] = q.elements[0];
		}
	      else
		{
		  selection->elements[i] = SOLVER_SOLVABLE_ONE_OF;
		  selection->elements[i + 1] = pool_queuetowhatprovides(pool, &q);
		}
	    }
	  queue_free(&q);
	}
      else if (select == SOLVER_SOLVABLE_NAME || select == SOLVER_SOLVABLE_PROVIDES)
	{
	  /* don't stack src reldeps */
	  if (relflags == REL_ARCH && (relevr == ARCH_SRC || relevr == ARCH_NOSRC) && ISRELDEP(id))
	    {
	      Reldep *rd = GETRELDEP(pool, id);
	      if (rd->flags == REL_ARCH && rd->evr == ARCH_SRC)
		id = rd->name;
	    }
	  selection->elements[i + 1] = pool_rel2id(pool, id, relevr, relflags, 1);
	}
      else
	continue;	/* actually cannot happen */

      /* now add the setflags we gained */
      if (relflags == REL_ARCH)
	selection->elements[i] |= SOLVER_SETARCH;
      if (relflags == REL_EQ && select != SOLVER_SOLVABLE_PROVIDES)
	{
	  if (pool->disttype == DISTTYPE_DEB)
	    selection->elements[i] |= SOLVER_SETEVR;	/* debian can't match version only like rpm */
	  else
	    {
	      const char *rel =  strrchr(pool_id2str(pool, relevr), '-');
	      selection->elements[i] |= rel ? SOLVER_SETEVR : SOLVER_SETEV;
	    }
	}
    }
  /* now prune out empty elements */
  selection_prune(pool, selection);
}

/* limit a selection to to repository */
/* prunes empty jobs */
static void
selection_filter_repo(Pool *pool, Queue *selection, Repo *repo)
{
  Queue q;
  int i, j;

  if (!repo)
    {
      queue_empty(selection);
      return;
    }
  queue_init(&q);
  for (i = j = 0; i < selection->count; i += 2)
    {
      Id select = selection->elements[i] & SOLVER_SELECTMASK;
      Id id = selection->elements[i + 1];
      if (select == SOLVER_SOLVABLE_ALL)
	{
	  select = SOLVER_SOLVABLE_REPO;
	  id = repo->repoid;
	}
      else if (select == SOLVER_SOLVABLE_REPO)
	{
	  if (id != repo->repoid)
	    select = 0;
	}
      else
	{
	  int bad = 0;
	  Id p, pp;
	  queue_empty(&q);
	  FOR_JOB_SELECT(p, pp, select, id)
	    {
	      if (pool->solvables[p].repo != repo)
		bad = 1;
	      else
		queue_push(&q, p);
	    }
	  if (bad || !q.count)
	    {
	      if (!q.count)
		select = 0;		/* prune empty jobs */
	      else if (q.count == 1)
		{
		  select = SOLVER_SOLVABLE | SOLVER_NOAUTOSET;
		  id = q.elements[0];
		}
	      else
		{
		  select = SOLVER_SOLVABLE_ONE_OF;
	          id = pool_queuetowhatprovides(pool, &q);
		}
	    }
	}
      if (!select)
	continue;	/* job is now empty */
      if (select == SOLVER_SOLVABLE_REPO)
	{
	  Id p;
	  Solvable *s;
	  FOR_REPO_SOLVABLES(repo, p, s)
	    break;
	  if (!p)
	    continue;	/* repo is empty */
	}
      selection->elements[j++] = select | (selection->elements[i] & ~SOLVER_SELECTMASK) | SOLVER_SETREPO;
      selection->elements[j++] = id;
    }
  queue_truncate(selection, j);
  queue_free(&q);
}


/* change a SOLVER_SOLVABLE_NAME selection to something that also includes extra packages */
/* extra packages are: src, badarch, disabled */
/* used by selection_depglob and selection_depglob_id */
static void
selection_addextra(Pool *pool, Queue *selection, int flags)
{
  Queue q;
  Id p, name;
  int i, haveextra;

  if ((flags & SELECTION_INSTALLED_ONLY) != 0)
    flags &= ~SELECTION_WITH_SOURCE;

  if (!(flags & (SELECTION_WITH_SOURCE | SELECTION_WITH_DISABLED | SELECTION_WITH_BADARCH)))
    return;	/* nothing to add */

  queue_init(&q);
  for (i = 0; i < selection->count; i += 2)
    {
      if (selection->elements[i] != SOLVER_SOLVABLE_NAME)
	continue;
      name = selection->elements[i + 1];
      haveextra = 0;
      queue_empty(&q);
      FOR_POOL_SOLVABLES(p)
	{
	  Solvable *s = pool->solvables + p;
	  if (s->name != name)
	    continue;
	  if (s->arch == ARCH_SRC || s->arch == ARCH_NOSRC)
	    {
	      if (!(flags & SELECTION_WITH_SOURCE))
		continue;
	      if (!(flags & SELECTION_WITH_DISABLED) && pool_disabled_solvable(pool, s))
		continue;
	      haveextra = 1;
	    }
	  else
	    {
	      if ((flags & SELECTION_SOURCE_ONLY) != 0)
		continue;
	      if (s->repo != pool->installed)
		{
		  if (pool_disabled_solvable(pool, s))
		    {
		      if (!(flags & SELECTION_WITH_DISABLED))
			continue;
		      if (!(flags & SELECTION_WITH_BADARCH) && pool_badarch_solvable(pool, s))
			continue;
		      haveextra = 1;
		    }
		  else if (pool_badarch_solvable(pool, s))
		    {
		      if (!(flags & SELECTION_WITH_BADARCH))
			continue;
		      haveextra = 1;
		    }
		}
	    }
	  queue_push(&q, p);
	}
      if (!haveextra || !q.count)
	continue;
      if (q.count == 1)
	{
	  selection->elements[i] = SOLVER_SOLVABLE | SOLVER_NOAUTOSET;
	  selection->elements[i + 1] = q.elements[0];
	}
      else
	{
	  selection->elements[i] = SOLVER_SOLVABLE_ONE_OF;
	  selection->elements[i + 1] = pool_queuetowhatprovides(pool, &q);
	}
    }
  queue_free(&q);
}

static inline const char *
skipkind(const char *n)
{
  const char *s;
  for (s = n; *s >= 'a' && *s <= 'z'; s++)
    ;
  if (*s == ':' && s != n)
     return s + 1;
  return n;
}

static inline void
queue_pushunique2(Queue *q, Id id1, Id id2)
{
  int i;
  for (i = 0; i < q->count; i += 2)
    if (q->elements[i] == id1 && q->elements[i + 1] == id2)
      return;
  queue_push2(q, id1, id2);
}

/* this is the fast path of selection_depglob: the id for the name
 * is known and thus we can quickly check the existance of a
 * package with that name or provides */
static int
selection_depglob_id(Pool *pool, Queue *selection, Id id, int flags)
{
  Id p, pp, matchid;
  int match = 0;

  matchid = id;
  if ((flags & SELECTION_SOURCE_ONLY) != 0)
    {
      /* sources do not have provides */
      if ((flags & SELECTION_NAME) == 0)
	return 0;
      if ((flags & SELECTION_INSTALLED_ONLY) != 0)
	return 0;
      /* add ARCH_SRC to match only sources */
      matchid = pool_rel2id(pool, id, ARCH_SRC, REL_ARCH, 1);
    }

  FOR_PROVIDES(p, pp, matchid)
    {
      Solvable *s = pool->solvables + p;
      if ((flags & SELECTION_INSTALLED_ONLY) != 0 && s->repo != pool->installed)
	continue;
      match = 1;
      if (s->name == id && (flags & SELECTION_NAME) != 0)
	{
	  queue_push2(selection, SOLVER_SOLVABLE_NAME, matchid);
	  if ((flags & SELECTION_WITH_SOURCE) != 0)
	    selection_addextra(pool, selection, flags);
	  return SELECTION_NAME;
	}
    }

  if ((flags & SELECTION_NAME) != 0 && (flags & SELECTION_WITH_SOURCE) != 0 && (flags & SELECTION_INSTALLED_ONLY) == 0)
    {
      /* WITH_SOURCE case, but we had no match. try SOURCE_ONLY instead */
      matchid = pool_rel2id(pool, id, ARCH_SRC, REL_ARCH, 1);
      FOR_PROVIDES(p, pp, matchid)
	{
	  Solvable *s = pool->solvables + p;
	  if (s->name == id)
	    {
	      queue_push2(selection, SOLVER_SOLVABLE_NAME, matchid);
	      return SELECTION_NAME;
	    }
	}
    }

  if (match && (flags & SELECTION_PROVIDES) != 0)
    {
      queue_push2(selection, SOLVER_SOLVABLE_PROVIDES, id);
      return SELECTION_PROVIDES;
    }
  return 0;
}

/* match the name or a provides of a package */
/* note that for SELECTION_INSTALLED_ONLY we do not filter the results
 * so that the selection can be modified later. */
static int
selection_depglob(Pool *pool, Queue *selection, const char *name, int flags)
{
  Id id, p, pp;
  int match = 0;
  int doglob = 0;
  int nocase = 0;
  int globflags = 0;

  if ((flags & SELECTION_SOURCE_ONLY) != 0)
    {
      flags &= ~SELECTION_PROVIDES;	/* sources don't provide anything */
      flags &= ~SELECTION_WITH_SOURCE;
    }

  if (!(flags & (SELECTION_NAME|SELECTION_PROVIDES)))
    return 0;

  if ((flags & SELECTION_INSTALLED_ONLY) != 0 && !pool->installed)
    return 0;

  nocase = flags & SELECTION_NOCASE;
  if (!nocase && !(flags & (SELECTION_SKIP_KIND | SELECTION_WITH_BADARCH | SELECTION_WITH_DISABLED)))
    {
      id = pool_str2id(pool, name, 0);
      if (id)
	{
	  /* the id is know, do the fast id matching using the whatprovides lookup */
	  int ret = selection_depglob_id(pool, selection, id, flags);
	  if (ret)
	    return ret;
	}
    }

  if ((flags & SELECTION_GLOB) != 0 && strpbrk(name, "[*?") != 0)
    doglob = 1;

  if (!nocase && !(flags & (SELECTION_SKIP_KIND | SELECTION_WITH_BADARCH | SELECTION_WITH_DISABLED)) && !doglob)
    return 0;	/* all done above in depglob_id */

  if (doglob && nocase)
    globflags = FNM_CASEFOLD;

  if ((flags & SELECTION_NAME) != 0)
    {
      const char *n;
      /* looks like a name glob. hard work. */
      FOR_POOL_SOLVABLES(p)
	{
	  Solvable *s = pool->solvables + p;
	  if ((flags & SELECTION_INSTALLED_ONLY) != 0 && s->repo != pool->installed)
	    continue;
	  if (s->arch == ARCH_SRC || s->arch == ARCH_NOSRC)
	    {
	      if (!(flags & SELECTION_SOURCE_ONLY) && !(flags & SELECTION_WITH_SOURCE))
		continue;
	      if (!(flags & SELECTION_WITH_DISABLED) && pool_disabled_solvable(pool, s))
		continue;
	    }
	  else if (s->repo != pool->installed)
	    {
	      if (!(flags & SELECTION_WITH_DISABLED) && pool_disabled_solvable(pool, s))
		continue;
	      if (!(flags & SELECTION_WITH_BADARCH) && pool_badarch_solvable(pool, s))
		continue;
	    }
	  id = s->name;
	  n = pool_id2str(pool, id);
	  if (flags & SELECTION_SKIP_KIND)
	    n = skipkind(n);
	  if ((doglob ? fnmatch(name, n, globflags) : nocase ? strcasecmp(name, n) : strcmp(name, n)) == 0)
	    {
	      if ((flags & SELECTION_SOURCE_ONLY) != 0)
		{
		  if (s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
		    continue;
		  id = pool_rel2id(pool, id, ARCH_SRC, REL_ARCH, 1);
		}
	      queue_pushunique2(selection, SOLVER_SOLVABLE_NAME, id);
	      match = 1;
	    }
	}
      if (match)
	{
	  if ((flags & (SELECTION_WITH_SOURCE | SELECTION_WITH_BADARCH | SELECTION_WITH_DISABLED)) != 0)
	    selection_addextra(pool, selection, flags);
          return SELECTION_NAME;
	}
    }

  if ((flags & SELECTION_PROVIDES))
    {
      /* looks like a dep glob. really hard work. */
      for (id = 1; id < pool->ss.nstrings; id++)
	{
	  const char *n;
	  /* do we habe packages providing this id? */
	  if (!pool->whatprovides[id] || pool->whatprovides[id] == 1)
	    continue;
	  n = pool_id2str(pool, id);
	  if ((doglob ? fnmatch(name, n, globflags) : nocase ? strcasecmp(name, n) : strcmp(name, n)) == 0)
	    {
	      if ((flags & SELECTION_INSTALLED_ONLY) != 0)
		{
		  FOR_PROVIDES(p, pp, id)
		    if (pool->solvables[p].repo == pool->installed)
		      break;
		  if (!p)
		    continue;
		}
	      queue_push2(selection, SOLVER_SOLVABLE_PROVIDES, id);
	      match = 1;
	    }
	}
      if (match)
        return SELECTION_PROVIDES;
    }
  return 0;
}

/* like selection_depglob, but check for a .arch suffix if the depglob did
   not work and SELECTION_DOTARCH is used */
static int
selection_depglob_arch(Pool *pool, Queue *selection, const char *name, int flags)
{
  int ret;
  const char *r;
  Id archid;

  if ((ret = selection_depglob(pool, selection, name, flags)) != 0)
    return ret;
  if (!(flags & SELECTION_DOTARCH))
    return 0;
  /* check if there is an .arch suffix */
  if ((r = strrchr(name, '.')) != 0 && r[1] && (archid = str2archid(pool, r + 1)) != 0)
    {
      char *rname = solv_strdup(name);
      rname[r - name] = 0;
      if (archid == ARCH_SRC || archid == ARCH_NOSRC)
	flags |= SELECTION_SOURCE_ONLY;
      if ((ret = selection_depglob(pool, selection, rname, flags)) != 0)
	{
	  selection_filter_rel(pool, selection, REL_ARCH, archid);
	  solv_free(rname);
	  return selection->count ? ret | SELECTION_DOTARCH : 0;
	}
      solv_free(rname);
    }
  return 0;
}

static int
selection_filelist(Pool *pool, Queue *selection, const char *name, int flags)
{
  Dataiterator di;
  Queue q;
  int type;

  if ((flags & SELECTION_INSTALLED_ONLY) != 0 && !pool->installed)
    return 0;
  /* all files in the file list start with a '/' */
  if (*name != '/')
    {
      if (!(flags & SELECTION_GLOB))
	return 0;
      if (*name != '*' && *name != '[' && *name != '?')
	return 0;
    }
  type = !(flags & SELECTION_GLOB) || strpbrk(name, "[*?") == 0 ? SEARCH_STRING : SEARCH_GLOB;
  if ((flags & SELECTION_NOCASE) != 0)
    type |= SEARCH_NOCASE;
  queue_init(&q);
  dataiterator_init(&di, pool, flags & SELECTION_INSTALLED_ONLY ? pool->installed : 0, 0, SOLVABLE_FILELIST, name, type|SEARCH_FILES|SEARCH_COMPLETE_FILELIST);
  while (dataiterator_step(&di))
    {
      Solvable *s = pool->solvables + di.solvid;
      if (!s->repo)
	continue;
      if (s->arch == ARCH_SRC || s->arch == ARCH_NOSRC)
	{
	  if (!(flags & SELECTION_SOURCE_ONLY) && !(flags & SELECTION_WITH_SOURCE))
	    continue;
	  if (!(flags & SELECTION_WITH_DISABLED) && pool_disabled_solvable(pool, s))
	    continue;
	}
      else
	{
	  if ((flags & SELECTION_SOURCE_ONLY) != 0)
	    continue;
	  if (s->repo != pool->installed)
	    {
	      if (!(flags & SELECTION_WITH_DISABLED) && pool_disabled_solvable(pool, s))
		continue;
	      if (!(flags & SELECTION_WITH_BADARCH) && pool_badarch_solvable(pool, s))
		continue;
	    }
	}
      queue_push(&q, di.solvid);
      dataiterator_skip_solvable(&di);
    }
  dataiterator_free(&di);
  if (!q.count)
    return 0;
  if (q.count > 1)
    queue_push2(selection, SOLVER_SOLVABLE_ONE_OF, pool_queuetowhatprovides(pool, &q));
  else
    queue_push2(selection, SOLVER_SOLVABLE | SOLVER_NOAUTOSET, q.elements[0]);
  queue_free(&q);
  return SELECTION_FILELIST;
}

static char *
splitrel(char *rname, char *r, int *rflagsp)
{
  int nend = r - rname;
  int rflags = 0;
  if (nend && *r == '=' && r[-1] == '!')
    {
      nend--;
      r++;
      rflags = REL_LT|REL_GT;
    }
  for (; *r; r++)
    {
      if (*r == '<')
	rflags |= REL_LT;
      else if (*r == '=')
	rflags |= REL_EQ;
      else if (*r == '>')
	rflags |= REL_GT;
      else
	break;
    }
  while (*r && (*r == ' ' || *r == '\t'))
    r++;
  while (nend && (rname[nend - 1] == ' ' || rname[nend - 1] == '\t'))
    nend--;
  if (nend <= 0 || !*r || !rflags)
    return 0;
  *rflagsp = rflags;
  rname[nend] = 0;
  return r;
}

static int
selection_rel(Pool *pool, Queue *selection, const char *name, int flags)
{
  int ret, rflags = 0;
  char *r, *rname;

  /* relation case, support:
   * depglob rel
   * depglob.arch rel
   */
  if ((r = strpbrk(name, "<=>")) == 0)
    return 0;
  rname = solv_strdup(name);
  r = rname + (r - name);
  if ((r = splitrel(rname, r, &rflags)) == 0)
    {
      solv_free(rname);
      return 0;
    }
  if ((ret = selection_depglob_arch(pool, selection, rname, flags)) == 0)
    {
      solv_free(rname);
      return 0;
    }
  if (rflags)
    selection_filter_rel(pool, selection, rflags, pool_str2id(pool, r, 1));
  solv_free(rname);
  return selection->count ? ret | SELECTION_REL : 0;
}

#if defined(MULTI_SEMANTICS)
# define EVRCMP_DEPCMP (pool->disttype == DISTTYPE_DEB ? EVRCMP_COMPARE : EVRCMP_MATCH_RELEASE)
#elif defined(DEBIAN)
# define EVRCMP_DEPCMP EVRCMP_COMPARE
#else
# define EVRCMP_DEPCMP EVRCMP_MATCH_RELEASE
#endif

/* magic epoch promotion code, works only for SELECTION_NAME selections */
static void
selection_filter_evr(Pool *pool, Queue *selection, char *evr)
{
  int i, j;
  Queue q;
  Id qbuf[10];

  queue_init(&q);
  queue_init_buffer(&q, qbuf, sizeof(qbuf)/sizeof(*qbuf));
  for (i = j = 0; i < selection->count; i += 2)
    {
      Id select = selection->elements[i] & SOLVER_SELECTMASK;
      Id id = selection->elements[i + 1];
      Id p, pp;
      const char *lastepoch = 0;
      int lastepochlen = 0;

      queue_empty(&q);
      FOR_JOB_SELECT(p, pp, select, id)
	{
	  Solvable *s = pool->solvables + p;
	  const char *sevr = pool_id2str(pool, s->evr);
	  const char *sp;
	  for (sp = sevr; *sp >= '0' && *sp <= '9'; sp++)
	    ;
	  if (*sp != ':')
	    sp = sevr;
	  /* compare vr part */
	  if (strcmp(evr, sp != sevr ? sp + 1 : sevr) != 0)
	    {
	      int r = pool_evrcmp_str(pool, sp != sevr ? sp + 1 : sevr, evr, EVRCMP_DEPCMP);
	      if (r == -1 || r == 1)
		continue;	/* solvable does not match vr */
	    }
	  queue_push(&q, p);
	  if (sp > sevr)
	    {
	      while (sevr < sp && *sevr == '0')	/* normalize epoch */
		sevr++;
	    }
	  if (!lastepoch)
	    {
	      lastepoch = sevr;
	      lastepochlen = sp - sevr;
	    }
	  else if (lastepochlen != sp - sevr || strncmp(lastepoch, sevr, lastepochlen) != 0)
	    lastepochlen = -1;	/* multiple different epochs */
	}
      if (!lastepoch || lastepochlen == 0)
	id = pool_str2id(pool, evr, 1);		/* no match at all or zero epoch */
      else if (lastepochlen >= 0)
	{
	  /* found exactly one epoch, simply prepend */
	  char *evrx = solv_malloc(strlen(evr) + lastepochlen + 2);
	  strncpy(evrx, lastepoch, lastepochlen + 1);
	  strcpy(evrx + lastepochlen + 1, evr);
	  id = pool_str2id(pool, evrx, 1);
	  solv_free(evrx);
	}
      else
	{
	  /* multiple epochs in multiple solvables, convert to list of solvables */
	  selection->elements[j] = (selection->elements[i] & ~SOLVER_SELECTMASK) | SOLVER_SOLVABLE_ONE_OF;
	  selection->elements[j + 1] = pool_queuetowhatprovides(pool, &q);
	  j += 2;
	  continue;
	}
      queue_empty(&q);
      queue_push2(&q, selection->elements[i], selection->elements[i + 1]);
      selection_filter_rel(pool, &q, REL_EQ, id);
      if (!q.count)
        continue;		/* oops, no match */
      selection->elements[j] = q.elements[0];
      selection->elements[j + 1] = q.elements[1];
      j += 2;
    }
  queue_truncate(selection, j);
  queue_free(&q);
}

/* match the "canonical" name of the package */
static int
selection_canon(Pool *pool, Queue *selection, const char *name, int flags)
{
  char *rname, *r, *r2;
  Id archid = 0;
  int ret;

  /*
   * nameglob-version
   * nameglob-version.arch
   * nameglob-version-release
   * nameglob-version-release.arch
   */
  flags |= SELECTION_NAME;
  flags &= ~SELECTION_PROVIDES;

  if (pool->disttype == DISTTYPE_DEB)
    {
      if ((r = strchr(name, '_')) == 0)
	return 0;
      rname = solv_strdup(name);	/* so we can modify it */
      r = rname + (r - name);
      *r++ = 0;
      if ((ret = selection_depglob(pool, selection, rname, flags)) == 0)
	{
	  solv_free(rname);
	  return 0;
	}
      /* is there a vaild arch? */
      if ((r2 = strrchr(r, '_')) != 0 && r[1] && (archid = str2archid(pool, r + 1)) != 0)
	{
	  *r2 = 0;	/* split off */
          selection_filter_rel(pool, selection, REL_ARCH, archid);
	}
      selection_filter_rel(pool, selection, REL_EQ, pool_str2id(pool, r, 1));
      solv_free(rname);
      return selection->count ? ret | SELECTION_CANON : 0;
    }

  if (pool->disttype == DISTTYPE_HAIKU)
    {
      if ((r = strchr(name, '-')) == 0)
	return 0;
      rname = solv_strdup(name);	/* so we can modify it */
      r = rname + (r - name);
      *r++ = 0;
      if ((ret = selection_depglob(pool, selection, rname, flags)) == 0)
	{
	  solv_free(rname);
	  return 0;
	}
      /* is there a vaild arch? */
      if ((r2 = strrchr(r, '-')) != 0 && r[1] && (archid = str2archid(pool, r + 1)) != 0)
	{
	  *r2 = 0;	/* split off */
          selection_filter_rel(pool, selection, REL_ARCH, archid);
	}
      selection_filter_rel(pool, selection, REL_EQ, pool_str2id(pool, r, 1));
      solv_free(rname);
      return selection->count ? ret | SELECTION_CANON : 0;
    }

  if ((r = strrchr(name, '-')) == 0)
    return 0;
  rname = solv_strdup(name);	/* so we can modify it */
  r = rname + (r - name);
  *r = 0;

  /* split off potential arch part from version */
  if ((r2 = strrchr(r + 1, '.')) != 0 && r2[1] && (archid = str2archid(pool, r2 + 1)) != 0)
    *r2 = 0;	/* found valid arch, split it off */
  if (archid == ARCH_SRC || archid == ARCH_NOSRC)
    flags |= SELECTION_SOURCE_ONLY;

  /* try with just the version */
  if ((ret = selection_depglob(pool, selection, rname, flags)) == 0)
    {
      /* no luck, try with version-release */
      if ((r2 = strrchr(rname, '-')) == 0)
	{
	  solv_free(rname);
	  return 0;
	}
      *r = '-';
      *r2 = 0;
      r = r2;
      if ((ret = selection_depglob(pool, selection, rname, flags)) == 0)
	{
	  solv_free(rname);
	  return 0;
	}
    }
  if (archid)
    selection_filter_rel(pool, selection, REL_ARCH, archid);
  selection_filter_evr(pool, selection, r + 1);	/* magic epoch promotion */
  solv_free(rname);
  return selection->count ? ret | SELECTION_CANON : 0;
}

int
selection_make(Pool *pool, Queue *selection, const char *name, int flags)
{
  int ret = 0;
  if ((flags & SELECTION_MODEBITS) != 0)
    {
      Queue q;
      queue_init(&q);
      ret = selection_make(pool, &q, name, flags & ~SELECTION_MODEBITS);
      if ((flags & SELECTION_MODEBITS) == SELECTION_ADD)
	selection_add(pool, selection, &q);
      else if ((flags & SELECTION_MODEBITS) == SELECTION_SUBTRACT)
	selection_subtract(pool, selection, &q);
      else if (ret || !(flags & SELECTION_FILTER_KEEP_IFEMPTY))
	selection_filter(pool, selection, &q);
      queue_free(&q);
      return ret;
    }
  queue_empty(selection);
  if ((flags & SELECTION_INSTALLED_ONLY) != 0 && !pool->installed)
    return 0;
  if ((flags & SELECTION_FILELIST) != 0)
    ret = selection_filelist(pool, selection, name, flags);
  if (!ret && (flags & SELECTION_REL) != 0)
    ret = selection_rel(pool, selection, name, flags);
  if (!ret)
    ret = selection_depglob_arch(pool, selection, name, flags);
  if (!ret && (flags & SELECTION_CANON) != 0)
    ret = selection_canon(pool, selection, name, flags);
  if (selection->count && (flags & SELECTION_INSTALLED_ONLY) != 0)
    selection_filter_repo(pool, selection, pool->installed);
  if (!selection->count)
    return 0;	/* no match -> always return zero */
  if (ret && (flags & SELECTION_FLAT) != 0)
    selection_flatten(pool, selection);
  return ret;
}

struct limiter {
  int start;	/* either 2 or repofilter->start */
  int end;	/* either nsolvables or repofilter->end */
  Id *mapper;
  Repo *repofilter;
};

/* add matching src packages to simple SOLVABLE_NAME selections */
static void
setup_limiter(Pool *pool, int flags, struct limiter *limiter)
{
  limiter->start = 2;
  limiter->end = pool->nsolvables;
  limiter->mapper = 0;
  limiter->repofilter = 0;
  if ((flags & SELECTION_INSTALLED_ONLY) != 0)
    {
      if (!pool->installed)
	limiter->end = 0;
      else
	{
	  limiter->repofilter = pool->installed;
	  limiter->start = pool->installed->start;
	  limiter->end = pool->installed->end;
	}
    }
}

static int
matchdep_str(const char *pattern, const char *string, int flags)
{
  if (flags & SELECTION_GLOB)
    {
      int globflags = (flags & SELECTION_NOCASE) != 0 ? FNM_CASEFOLD : 0;
      return fnmatch(pattern, string, globflags) == 0 ? 1 : 0;
    }
  if (flags & SELECTION_NOCASE)
    return strcasecmp(pattern, string) == 0 ? 1 : 0;
  return strcmp(pattern, string) == 0 ? 1 : 0;
}

/* like pool_match_dep but uses matchdep_str to match the name for glob and nocase matching */
static int
matchdep(Pool *pool, Id id, char *rname, int rflags, Id revr, int flags)
{
  if (ISRELDEP(id))
    {
      Reldep *rd = GETRELDEP(pool, id);
      if (rd->flags > 7)
	{
	  if (rd->flags == REL_AND || rd->flags == REL_OR || rd->flags == REL_WITH || rd->flags == REL_WITHOUT || rd->flags == REL_COND || rd->flags == REL_UNLESS)
	    {
	      if (matchdep(pool, rd->name, rname, rflags, revr, flags))
		return 1;
	      if ((rd->flags == REL_COND || rd->flags == REL_UNLESS) && ISRELDEP(rd->evr))
		{
		  rd = GETRELDEP(pool, rd->evr);
		  if (rd->flags != REL_ELSE)
		    return 0;
		}
	      if (rd->flags != REL_COND && rd->flags != REL_UNLESS && rd->flags != REL_WITHOUT && matchdep(pool, rd->evr, rname, rflags, revr, flags))
		return 1;
	      return 0;
	    }
	  if (rd->flags == REL_ARCH)
	    return matchdep(pool, rd->name, rname, rflags, revr, flags);
	}
      if (!matchdep(pool, rd->name, rname, rflags, revr, flags))
	return 0;
      if (rflags && !pool_intersect_evrs(pool, rd->flags, rd->evr, rflags, revr))
	return 0;
      return 1;
    }
  return matchdep_str(rname, pool_id2str(pool, id), flags);
}

int
selection_make_matchdeps_limited(Pool *pool, Queue *selection, const char *name, int flags, int keyname, int marker, struct limiter *limiter)
{
  int li, i, j;
  char *rname, *r = 0;
  int rflags = 0;
  Id revr = 0;
  Id p;
  Queue q;

  queue_empty(selection);
  if (!limiter->end)
    return 0;

  flags |= SELECTION_REL;	/* XXX: remove */
  if ((flags & SELECTION_MATCH_DEPSTR) != 0)
    flags &= ~SELECTION_REL;

  rname = solv_strdup(name);
  if ((flags & SELECTION_REL) != 0)
    {
      if ((r = strpbrk(rname, "<=>")) != 0)
	{
	  if ((r = splitrel(rname, r, &rflags)) == 0)
	    {
	      solv_free(rname);
	      return 0;
	    }
	}
      revr = pool_str2id(pool, r, 1);
    }

  if ((flags & SELECTION_GLOB) != 0 && !strpbrk(rname, "[*?") != 0)
    flags &= ~SELECTION_GLOB;

  if ((flags & SELECTION_GLOB) == 0 && (flags & SELECTION_NOCASE) == 0 && (flags & SELECTION_MATCH_DEPSTR) == 0)
    {
      /* we can use the faster selection_make_matchdepid */
      Id dep = pool_str2id(pool, rname, 1);
      if (rflags)
	dep = pool_rel2id(pool, dep, revr, rflags, 1);
      solv_free(rname);
      return selection_make_matchdepid(pool, selection, dep, flags, keyname, marker);
    }

  queue_init(&q);
  for (li = limiter->start; li < limiter->end; li++)
    {
      Solvable *s;
      p = limiter->mapper ? limiter->mapper[li] : li;
      s = pool->solvables + p;
      if (!s->repo || (limiter->repofilter && s->repo != limiter->repofilter))
	continue;
      if (s->arch == ARCH_SRC || s->arch == ARCH_NOSRC)
	{
	  if (!(flags & SELECTION_SOURCE_ONLY) && !(flags & SELECTION_WITH_SOURCE))
	    continue;
	  if (!(flags & SELECTION_WITH_DISABLED) && pool_disabled_solvable(pool, s))
	    continue;
	}
      else
        {
	  if ((flags & SELECTION_SOURCE_ONLY) != 0)
	    continue;
	  if (s->repo != pool->installed)
	    {
	      if (!(flags & SELECTION_WITH_DISABLED) && pool_disabled_solvable(pool, s))
		continue;
	      if (!(flags & SELECTION_WITH_BADARCH) && pool_badarch_solvable(pool, s))
		continue;
	    }
	}
      if (keyname == SOLVABLE_NAME)			/* nevr match hack */
	{
	  if ((flags & SELECTION_MATCH_DEPSTR) != 0)	/* mis-use */
	    {
	      char *tmp = pool_tmpjoin(pool, pool_id2str(pool, s->name), " = ", pool_id2str(pool, s->evr));
	      if (!matchdep_str(rname, tmp, flags))
		continue;
	    }
	  else
	    {
	      if (!matchdep(pool, s->name, rname, rflags, revr, flags))
		continue;
	      if (rflags && !pool_intersect_evrs(pool, rflags, revr, REL_EQ, s->evr))
		continue;
	    }
	  queue_push(selection, p);
	  continue;
	}
      queue_empty(&q);
      repo_lookup_deparray(s->repo, p, keyname, &q, marker);
      for (i = 0; i < q.count; i++)
	{
	  Id id = q.elements[i];
	  if ((flags & SELECTION_MATCH_DEPSTR) != 0)
	    {
	      if (matchdep_str(rname, pool_dep2str(pool, id), flags))
		break;
	    }
	  else
	    {
	      if (matchdep(pool, id, rname, rflags, revr, flags))
		break;
	    }
	}
      if (i < q.count)
	queue_push(selection, p);
    }
  queue_free(&q);
  solv_free(rname);
  if (!selection->count)
    return 0;

  /* convert package list to selection */
  j = selection->count;
  queue_insertn(selection, 0, selection->count, 0);
  for (i = 0; i < selection->count; )
    {
      selection->elements[i++] = SOLVER_SOLVABLE | SOLVER_NOAUTOSET;
      selection->elements[i++] = selection->elements[j++];
    }

  if ((flags & SELECTION_FLAT) != 0)
    selection_flatten(pool, selection);
  return keyname == SOLVABLE_NAME ? SELECTION_NAME : SELECTION_PROVIDES;
}

/*
 *  select against the dependencies in keyname
 *  like SELECTION_REL and SELECTION_PROVIDES, but with the
 *  deps in keyname instead of provides.
 */
int
selection_make_matchdeps(Pool *pool, Queue *selection, const char *name, int flags, int keyname, int marker)
{
  struct limiter limiter;

  setup_limiter(pool, flags, &limiter);

  if ((flags & SELECTION_MODEBITS) != 0)
    {
      int ret;
      Queue q, qlimit;
      queue_init(&q);
      queue_init(&qlimit);
      if ((flags & SELECTION_MODEBITS) == SELECTION_SUBTRACT || (flags & SELECTION_MODEBITS) == SELECTION_FILTER)
	{
	  selection_solvables(pool, selection, &qlimit);
	  limiter.start = 0;
	  limiter.end = qlimit.count;
	  limiter.mapper = qlimit.elements;
	}
      ret = selection_make_matchdeps_limited(pool, &q, name, flags & ~SELECTION_MODEBITS, keyname, marker, &limiter);
      queue_free(&qlimit);
      if ((flags & SELECTION_MODEBITS) == SELECTION_ADD)
	selection_add(pool, selection, &q);
      else if ((flags & SELECTION_MODEBITS) == SELECTION_SUBTRACT)
	selection_subtract(pool, selection, &q);
      else if (ret || !(flags & SELECTION_FILTER_KEEP_IFEMPTY))
	selection_filter(pool, selection, &q);
      queue_free(&q);
      return ret;
    }
  return selection_make_matchdeps_limited(pool, selection, name, flags, keyname, marker, &limiter);
}

int
selection_make_matchdepid_limited(Pool *pool, Queue *selection, Id dep, int flags, int keyname, int marker, struct limiter *limiter)
{
  int i, j, li;
  Id p;
  Queue q;
  Reldep *rd = 0;

  queue_empty(selection);

  if (!limiter->end || !dep)
    return 0;
  if (keyname == SOLVABLE_NAME && (flags & SELECTION_MATCH_DEPSTR) != 0)
    {
      if (!ISRELDEP(dep))
	return 0;
      rd = GETRELDEP(pool, dep);
      if (rd->flags != REL_EQ)
	return 0;
    }
  queue_init(&q);
  for (li = limiter->start; li < limiter->end; li++)
    {
      Solvable *s;
      p = limiter->mapper ? limiter->mapper[li] : li;
      s = pool->solvables + p;
      if (!s->repo || (limiter->repofilter && s->repo != limiter->repofilter))
	continue;
      if (s->arch == ARCH_SRC || s->arch == ARCH_NOSRC)
	{
	  if (!(flags & SELECTION_SOURCE_ONLY) && !(flags & SELECTION_WITH_SOURCE))
	    continue;
	  if (!(flags & SELECTION_WITH_DISABLED) && pool_disabled_solvable(pool, s))
	    continue;
	}
      else
        {
	  if ((flags & SELECTION_SOURCE_ONLY) != 0)
	    continue;
	  if (s->repo != pool->installed)
	    {
	      if (!(flags & SELECTION_WITH_DISABLED) && pool_disabled_solvable(pool, s))
		continue;
	      if (!(flags & SELECTION_WITH_BADARCH) && pool_badarch_solvable(pool, s))
		continue;
	    }
	}
      if (keyname == SOLVABLE_NAME)			/* nevr match hack */
	{
	  if ((flags & SELECTION_MATCH_DEPSTR) != 0)	/* mis-use */
	    {
	      if (rd->name != s->name || rd->evr != s->evr)
		continue;
	    }
	  else
	    {
	      if (!pool_match_nevr(pool, s, dep))
		continue;
	    }
	  queue_push(selection, p);
	  continue;
	}
      queue_empty(&q);
      repo_lookup_deparray(s->repo, p, keyname, &q, marker);
      for (i = 0; i < q.count; i++)
	{
	  if ((flags & SELECTION_MATCH_DEPSTR) != 0)	/* mis-use */
	    {
	      if (q.elements[i] == dep)
		break;
	    }
	  else
	    {
	      if (pool_match_dep(pool, q.elements[i], dep))
		break;
	    }
	}
      if (i < q.count)
	queue_push(selection, p);
    }
  queue_free(&q);
  if (!selection->count)
    return 0;

  /* convert package list to selection */
  j = selection->count;
  queue_insertn(selection, 0, selection->count, 0);
  for (i = 0; i < selection->count; )
    {
      selection->elements[i++] = SOLVER_SOLVABLE | SOLVER_NOAUTOSET;
      selection->elements[i++] = selection->elements[j++];
    }

  if ((flags & SELECTION_FLAT) != 0)
    selection_flatten(pool, selection);
  return keyname == SOLVABLE_NAME ? SELECTION_NAME : SELECTION_PROVIDES;
}

/*
 *  select against the dependency id in keyname
 */
int
selection_make_matchdepid(Pool *pool, Queue *selection, Id dep, int flags, int keyname, int marker)
{
  struct limiter limiter;

  limiter.start = 2;
  limiter.end = pool->nsolvables;
  limiter.mapper = 0;
  limiter.repofilter = 0;

  if ((flags & SELECTION_MODEBITS) != 0)
    {
      int ret;
      Queue q, qlimit;
      queue_init(&q);
      queue_init(&qlimit);
      if ((flags & SELECTION_MODEBITS) == SELECTION_SUBTRACT || (flags & SELECTION_MODEBITS) == SELECTION_FILTER)
	{
	  selection_solvables(pool, selection, &qlimit);
	  limiter.start = 0;
	  limiter.end = qlimit.count;
	  limiter.mapper = qlimit.elements;
	}
      ret = selection_make_matchdepid_limited(pool, &q, dep, flags & ~SELECTION_MODEBITS, keyname, marker, &limiter);
      queue_free(&qlimit);
      if ((flags & SELECTION_MODEBITS) == SELECTION_ADD)
	selection_add(pool, selection, &q);
      else if ((flags & SELECTION_MODEBITS) == SELECTION_SUBTRACT)
	selection_subtract(pool, selection, &q);
      else if (ret || !(flags & SELECTION_FILTER_KEEP_IFEMPTY))
	selection_filter(pool, selection, &q);
      queue_free(&q);
      return ret;
    }
  return selection_make_matchdepid_limited(pool, selection, dep, flags, keyname, marker, &limiter);
}

static inline int
pool_is_kind(Pool *pool, Id name, Id kind)
{
  const char *n;
  if (!kind)
    return 1;
  n = pool_id2str(pool, name);
  if (kind != 1)
    {
      const char *kn = pool_id2str(pool, kind);
      int knl = strlen(kn);
      return !strncmp(n, kn, knl) && n[knl] == ':' ? 1 : 0;
    }
  else
    {
      if (*n == ':')
        return 1;
      while(*n >= 'a' && *n <= 'z')
        n++;
      return *n == ':' ? 0 : 1;
    }
}

static void
selection_filter_map(Pool *pool, Queue *sel, Map *m, int setflags)
{
  int i, j, miss;
  Queue q;
  Id p, pp;

  queue_init(&q);
  for (i = j = 0; i < sel->count; i += 2)
    {
      Id select = sel->elements[i] & SOLVER_SELECTMASK;
      queue_empty(&q);
      miss = 0;
      if (select == SOLVER_SOLVABLE_ALL)
	{
	  FOR_POOL_SOLVABLES(p)
	    {
	      if (map_tst(m, p))
	        queue_push(&q, p);
	      else
	        miss = 1;
	    }
	}
      else if (select == SOLVER_SOLVABLE_REPO)
	{
	  Solvable *s;
	  Repo *repo = pool_id2repo(pool, sel->elements[i + 1]);
	  if (repo)
	    {
	      FOR_REPO_SOLVABLES(repo, p, s)
		{
		  if (map_tst(m, p))
		    queue_push(&q, p);
		  else
		    miss = 1;
		}
	    }
	}
      else
	{
	  FOR_JOB_SELECT(p, pp, select, sel->elements[i + 1])
	    {
	      if (map_tst(m, p))
	        queue_pushunique(&q, p);
	      else
	        miss = 1;
	    }
	}
      if (!q.count)
	continue;
      if (!miss)
	{
	  sel->elements[j] = sel->elements[i] | setflags;
	  sel->elements[j + 1] = sel->elements[i + 1];
	}
      else if (q.count > 1)
	{
	  sel->elements[j] = (sel->elements[i] & ~SOLVER_SELECTMASK) | SOLVER_SOLVABLE_ONE_OF | setflags;
	  sel->elements[j + 1] = pool_queuetowhatprovides(pool, &q);
	}
      else
	{
	  sel->elements[j] = (sel->elements[i] & ~SOLVER_SELECTMASK) | SOLVER_SOLVABLE | SOLVER_NOAUTOSET | setflags;
	  sel->elements[j + 1] = q.elements[0];
	}
      j += 2;
    }
  queue_truncate(sel, j);
  queue_free(&q);
}

static void
selection_filter_int(Pool *pool, Queue *sel1, Queue *sel2, int invert)
{
  int i, j;
  Id p, pp, q1filled = 0;
  Queue q1;
  Map m2;
  Id setflags = 0;

  if (!sel1->count || !sel2->count)
    {
      if (invert && !sel2->count)
	return;
      queue_empty(sel1);
      return;
    }
  if (sel1->count == 2 && (sel1->elements[0] & SOLVER_SELECTMASK) == SOLVER_SOLVABLE_ALL && !invert)
    {
      /* XXX: not 100% correct, but very useful */
      p = sel1->elements[0] & ~(SOLVER_SELECTMASK | SOLVER_SETMASK);	/* job & jobflags */
      queue_free(sel1);
      queue_init_clone(sel1, sel2);
      for (i = 0; i < sel1->count; i += 2)
        sel1->elements[i] = (sel1->elements[i] & (SOLVER_SELECTMASK | SOLVER_SETMASK)) | p ;
      return;
    }

  /* convert sel2 into a map */
  queue_init(&q1);
  map_init(&m2, pool->nsolvables);
  for (i = 0; i < sel2->count; i += 2)
    {
      Id select = sel2->elements[i] & SOLVER_SELECTMASK;
      if (select == SOLVER_SOLVABLE_ALL)
	{
	  queue_free(&q1);
	  map_free(&m2);
	  if (invert)
	    queue_empty(sel1);
	  return;
	}
      if (select == SOLVER_SOLVABLE_REPO)
	{
	  Solvable *s;
	  Repo *repo = pool_id2repo(pool, sel2->elements[i + 1]);
	  if (repo)
	    {
	      FOR_REPO_SOLVABLES(repo, p, s)
	        map_set(&m2, p);
	    }
	}
      else
	{
	  if ((select == SOLVER_SOLVABLE_NAME || select == SOLVER_SOLVABLE_PROVIDES) && ISRELDEP(sel2->elements[i + 1]))
	    {
	      Reldep *rd = GETRELDEP(pool, sel2->elements[i + 1]);
	      if (rd->flags == REL_ARCH && rd->name == 0)
		{
		  /* special arch filter */
		  if (!q1filled++)
		    selection_solvables(pool, sel1, &q1);
		  for (j = 0; j < q1.count; j++)
		    {
		      Id p = q1.elements[j];
		      Solvable *s = pool->solvables + p;
		      if (s->arch == rd->evr || (rd->evr == ARCH_SRC && s->arch == ARCH_NOSRC))
		        map_set(&m2, p);
		    }
		  continue;
		}
	      else if (rd->flags == REL_KIND && rd->name == 0)
		{
		  /* special kind filter */
		  if (!q1filled++)
		    selection_solvables(pool, sel1, &q1);
		  for (j = 0; j < q1.count; j++)
		    {
		      Id p = q1.elements[j];
		      Solvable *s = pool->solvables + p;
		      if (pool_is_kind(pool, s->name, rd->evr))
		        map_set(&m2, p);
		    }
		  continue;
		}
	    }
	  FOR_JOB_SELECT(p, pp, select, sel2->elements[i + 1])
	    map_set(&m2, p);
	}
    }
  queue_free(&q1);

  /* now filter sel1 with the map */
  if (invert)
    map_invertall(&m2);
  if (sel2->count == 2)		/* XXX: AND all setmasks instead? */
    setflags = sel2->elements[0] & SOLVER_SETMASK & ~SOLVER_NOAUTOSET;
  selection_filter_map(pool, sel1, &m2, setflags);
  map_free(&m2);
}

void
selection_filter(Pool *pool, Queue *sel1, Queue *sel2)
{
  return selection_filter_int(pool, sel1, sel2, 0);
}

void
selection_add(Pool *pool, Queue *sel1, Queue *sel2)
{
  if (sel2->count)
    queue_insertn(sel1, sel1->count, sel2->count, sel2->elements);
}

void
selection_subtract(Pool *pool, Queue *sel1, Queue *sel2)
{
  return selection_filter_int(pool, sel1, sel2, 1);
}

const char *
pool_selection2str(Pool *pool, Queue *selection, Id flagmask)
{
  char *s;
  const char *s2;
  int i;
  s = pool_tmpjoin(pool, 0, 0, 0);
  for (i = 0; i < selection->count; i += 2)
    {
      Id how = selection->elements[i];
      if (*s)
	s = pool_tmpappend(pool, s, " + ", 0);
      s2 = solver_select2str(pool, how & SOLVER_SELECTMASK, selection->elements[i + 1]);
      s = pool_tmpappend(pool, s, s2, 0);
      pool_freetmpspace(pool, s2);
      how &= flagmask & SOLVER_SETMASK;
      if (how)
	{
	  int o = strlen(s);
	  s = pool_tmpappend(pool, s, " ", 0);
	  if (how & SOLVER_SETEV)
	    s = pool_tmpappend(pool, s, ",setev", 0);
	  if (how & SOLVER_SETEVR)
	    s = pool_tmpappend(pool, s, ",setevr", 0);
	  if (how & SOLVER_SETARCH)
	    s = pool_tmpappend(pool, s, ",setarch", 0);
	  if (how & SOLVER_SETVENDOR)
	    s = pool_tmpappend(pool, s, ",setvendor", 0);
	  if (how & SOLVER_SETREPO)
	    s = pool_tmpappend(pool, s, ",setrepo", 0);
	  if (how & SOLVER_NOAUTOSET)
	    s = pool_tmpappend(pool, s, ",noautoset", 0);
	  if (s[o + 1] != ',')
	    s = pool_tmpappend(pool, s, ",?", 0);
	  s[o + 1] = '[';
	  s = pool_tmpappend(pool, s, "]", 0);
	}
    }
  return s;
}

