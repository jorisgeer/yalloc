/* stats.h - statistics

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

    accumulate and print stats
*/

#define Logfile Fstat

#define Statbuf 8192

static ub4 slabstats(region *reg,yalstats *sp,char *buf,ub4 pos,ub4 len,bool print,ub4 opts,ub4 cnt)
{
  ub4 cellen = reg->cellen;
  ub4 frecnt,fresiz,inuse,inusecnt,bincnt,rbincnt,inicnt,celcnt = reg->celcnt;
  ub4 inipos;
  ub4 rid = reg->id;
  char status;
  ub4 hdpos;
  char albuf[256];
  ub4 dostate = opts & Yal_stats_state;
  struct regstat *rp = &reg->stat;
  ub4 a;

  ub4 claseq = 0;
  ub2 class = (ub2)reg->clas;

  size_t rlen = reg->len;
  size_t ip = reg->user;

  size_t ac;
  size_t Allocs = rp->Allocs;
  size_t callocs = rp->callocs;
  size_t binallocs = rp->binallocs;
  size_t iniallocs = rp->iniallocs;
  size_t xallocs = rp->xallocs;
  size_t reallocles = rp->reallocles;
  size_t reallocgts = rp->reallocgts;
  size_t frees = rp->frees;
  size_t rfrees = rp->rfrees;

  size_t allocs = iniallocs + binallocs + callocs + xallocs + Allocs;

  ycheck(pos,Lstats,allocs < callocs,"region %.01llu alloc %zu calloc %zu",reg->uid,allocs,callocs)
  allocs -= callocs;

  // add to per-heap

  sp->slaballocs += allocs;
  sp->slabAllocs += Allocs;
  sp->callocs += callocs;
  sp->reallocles += reallocles;
  sp->reallocgts += reallocgts;
  sp->slabxfrees += rfrees;
  sp->slabfrees += frees;

  sp->minlen = min(sp->minlen,cellen);
  sp->maxlen = max(sp->maxlen,cellen);

  sp->minclass = min(sp->minclass,class);
  sp->maxclass = max(sp->maxclass,class);

  sp->loadr = min(sp->loadr,ip);
  sp->hiadr = max(sp->hiadr,ip + rlen);

  switch (reg->aged) {
  case 0: status = '+';   sp->region_cnt++; claseq = reg->claseq; break;
  case 1: status = '~';   sp->freeregion_cnt++; break;
  case 2: status = '-';   sp->freeregion_cnt++; break;
  case 3: status = 'x';   claseq = 0; sp->delregion_cnt++; break;
  default: status = '?';
  }

  inipos = reg->inipos;
  inicnt = celcnt - inipos;
  bincnt = reg->binpos;
  rbincnt = reg->rbinpos;
  frecnt = bincnt + rbincnt + inicnt;
  fresiz = frecnt * cellen;
  inusecnt = celcnt - frecnt;
  inuse = inusecnt * cellen;
  sp->frecnt += frecnt; // not accumulated
  sp->fresiz += fresiz;
  sp->inuse += inuse;
  sp->inusecnt += inusecnt;
  sp->slabmem += rlen + reg->metalen;

  if (print == 0 || (opts & Yal_stats_detail) == 0) return pos;

  if ( (cnt & 0x1f) == 0) {

    hdpos = snprintf_mini(albuf,0,255,
      "\n  %-5s %-3s %-4s %-7s %-7s %-7s %-7s %-23s %-7s %-7s %-7s %-7s %-7s",
      "id","seq","gen","len","cellen","alloc ","calloc","","free","rfree","Alloc","realloc","Realloc");
     if (dostate) hdpos += snprintf_mini(albuf,hdpos,255," %-7s %-7s %-7s %-7s %-4s","cnt","free","ini","bin","rbin");
     albuf[hdpos++] = '\n';
     pos += underline(buf + pos,len - pos,albuf,hdpos);
  }
  snprintf_mini(albuf,0,256,"%-7zu` %-7zu` %-7zu`",iniallocs,binallocs,xallocs);

  pos += snprintf_mini(buf,pos,len,"%c %-5u %-3u %-4u %-7zu` %-7u` %-7zu` %-7zu` %-23s %-7zu` %-7zu`",status,rid,claseq,reg->gen,rlen,reg->cellen,allocs,callocs,albuf,frees,rfrees);
  if (dostate || (Allocs | reallocles | reallocgts)) pos += snprintf_mini(buf,pos,len," %-7zu` %-7zu` %-7zu`",Allocs,reallocles,reallocgts);
  if (dostate) {
    pos += snprintf_mini(buf,pos,len," %-7u %-7u %-7u %-7u %-7u",celcnt,frecnt,inipos,bincnt,rbincnt);
  }

  if (Allocs) {
    for (a = 0; a < 32; a++) {
      ac = rp->aligns[a];
      if (ac) pos += snprintf_mini(buf,pos,len," %2u.%-7zu",a,ac);
    }
  }
  buf[pos++] = '\n';

  return pos;
}

static void Cold bumpstats(int fd,yalstats *sp,bregion *regs,ub4 regcnt,bool print)
{
  bregion *reg;
  enum Rtype typ = regs->typ;
  ub4 allocs,frees,afs = 0;
  ub4 r,rpos;
  ub4 pos = 0,len = 4094;
  char buf[4096];

  for (r = 0; r < regcnt; r++) {
    reg = regs + r;
    if (reg->len == 0) continue;
    allocs = reg->allocs;
    frees = Atomget(reg->frees,Moacq);
    afs += allocs + frees;
  }
  if (afs == 0) print = 0;

  if (print) {
    if (typ == Rmini) pos += snprintf_mini(buf,pos,len,"\n  -- yalloc mini region stats for heap base %u --\n",regs->hid);
    else pos += snprintf_mini(buf,pos,len,"\n  -- yalloc bump region stats for heap %u --\n",regs->hid);
    pos += snprintf_mini(buf,pos,len,"\nr %-6s %-6s %-6s\n","alloc","free","used");
  }
  sp->bumpallocs = 0;
  sp->bumpfrees = 0;

  for (r = 0; r < regcnt; r++) {
    reg = regs + r;
    if (reg->len == 0) continue;
    allocs = reg->allocs;
    frees = Atomget(reg->frees,Moacq);
//    freed = reg->freebytes;

    if (typ == Rmini) {
      sp->miniallocs = allocs;
      // sp->minialbytes += reg->albytes;
      sp->minifrees = frees;
    } else {
      sp->bumpallocs += allocs;
      // sp->bumpalbytes += reg->albytes;
      sp->bumpfrees += frees;
    }
    rpos = reg->pos;
    if (print) pos += snprintf_mini(buf,pos,len,"%u %-6u %-6u %-6u\n",r,allocs,frees,rpos);
    if (pos > 3800) { oswrite(fd,buf,pos,Fln); pos = 0; }
  }
  if (pos) { buf[pos++] = '\n'; oswrite(fd,buf,pos,Fln); }
}

static void Cold mmapstats(int fd,heap *hb,bool print)
{
  mpregion *reg;
  yalstats *sp = &hb->stat;
  ub4 rid;
  size_t ip,len;
  char status;
  ub4 pos = 0,blen = 4094;
  char buf[4096];

  reg = hb->mpreglst;
  if (reg == nil || hb->stat.newmpregions == 0) return;

  if (print) pos += snprintf_mini(buf,pos,blen,"\n  - yalloc mmap region stats for heap %u -\n",hb->id);

  sp->mapminlen = Size_max;
  sp->mapmaxlen = 0;

  do {
    rid = reg->id;
    len = reg->len;

    switch (reg->aged) {
    case 0: status = '+';  sp->xregion_cnt++; sp->inmapuse += len; sp->inmapusecnt++; break;
    case 1: status = '~';  break;
    case 2: status = '-';  break;
    case 3: status = 'x';  break;
    default: status = '?';
    }

    if (len) {
      sp->mapminlen = min(sp->mapminlen,len);
      sp->mapmaxlen = max(sp->mapmaxlen,len);
    }
    ip = (size_t)reg->user;

    if (print) {
      if ( (rid & 0x1f) == 1) pos += snprintf_mini(buf,pos,blen,"\n  %-4s %-4s %-9s %-9s\n","id","gen","adr","len");

      pos += snprintf_mini(buf,pos,blen,"%c %-4u %-4u %-9zx %-9zu`\n",status,reg->id,reg->gen,ip,len);
      if (pos > 3800) { oswrite(fd,buf,pos,Fln); pos = 0; }
    }
  } while ((reg = reg->nxt));
  if (pos) oswrite(fd,buf,pos,Fln);
}

// hb may be nil
static Cold void regstats(int fd,heap *hb,bool print,ub4 opts)
{
  region *nxt,*reg = nil;
  enum Rtype typ;
  yalstats sp0,*sp;
  struct regstat *rp;
  size_t alloc0s,free0s,af0,freenils;
  ub4 cnt = 0;
  region reg0;
  char buf[4096];
  ub4 pos = 0,blen = 4095;
  ub4 iter = 2000;

  if (hb) {
    reg = hb->reglst;
    sp = &hb->stat;
  } else {
    sp = &sp0;
    memset(&sp0,0,sizeof(sp0));
  }
  sp->loadr = Size_max;
  sp->minclass = Hi16;
  sp->minlen = Hi32;

  // region 0 = zero allocs / frees
  memset(&reg0,0,sizeof(reg0));
  rp = &reg0.stat;
  alloc0s = rp->iniallocs = sp->alloc0s;
  free0s = rp->frees = sp->free0s;
  freenils = sp->freenils;
  af0 = alloc0s | free0s | freenils;
  if (freenils) pos = snprintf_mini(buf,pos,blen,"  nil free %zu`\n",freenils);
  if (reg == nil && af0 == 0) return;
  if (af0) pos = slabstats(&reg0,sp,buf,pos,blen,print,opts,cnt++);

  while (reg && --iter) {
    nxt = reg->nxt;
    typ = reg->typ;
    if (typ != Rslab) { sp->noregion_cnt++; reg = nxt; continue; }
    pos = slabstats(reg,sp,buf,pos,blen,print,opts,cnt++);
    if (pos > 3096) { oswrite(fd,buf,pos,Fln); pos = 0; }
    reg = nxt;
  }
  if (pos) oswrite(fd,buf,pos,Fln);
}

// print table-like
static ub4 table(char *buf,ub4 pos,ub4 len,ub4 nwid,ub4 vwid,...)
{
  va_list ap;
  ub4 n;
  cchar *nam=nil;
  size_t val=0;

  va_start(ap,vwid);

  do {
    nam = va_arg(ap,cchar *);
    if (nam == nil) break;

    val = va_arg(ap,size_t);
    if (val == 0) continue;

    n = snprintf_mini(buf,pos,len,"%*s %zu` ",-nwid,nam,val);
    pos += n;
    while (n++ < nwid + vwid && pos + 2 < len) buf[pos++] = ' ';

  } while (1);
  buf[pos] = 0;

  va_end(ap);

  return pos;
}

// returns errorcnt
static Cold size_t yal_mstats_heap(int fd,heap *hb,yalstats *ret,bool print,ub4 opts,ub4 tag,cchar *desc,ub4 fln)
{
  yalstats *sp = nil;
  yalstats dummy;
  size_t errs;
  ub4 hid = 0;
  ub4 pos = 0,tpos;
  ub4 len = 1022;
  char buf[1024];
  ub4 tlen = 510;
  char tbuf[512];
  ub4 issum = opts & 0x80;
  ub4 detail = opts & Yal_stats_detail;

  if (issum) { // for summing over heaps
    sp = ret;
  } else if (hb) {
    sp = &hb->stat;
    hid = hb->id;
  }
  if (sp == nil) {
    sp = &dummy;
    memset(sp,0,sizeof(yalstats));
  }
  sp->tag = tag;

  // tallied outside of heap
  size_t xmapfrees = sp->xmapfrees;
  // size_t xslabfrees = sp->xslabfrees;
  size_t xfreebuf = sp->xfreebuf;
  size_t xfreebatch = sp->xfreebatch;
  size_t xfreebatch1 = sp->xfreebatch1;

  size_t invalid_frees = sp->invalid_frees;
  size_t xmaxbin = sp->xmaxbin;

  size_t locks = sp->locks;
  size_t clocks = sp->clocks;
  size_t newheaps = sp->newheaps;
  size_t useheaps = sp->useheaps;

  if (print) {
    if (issum == 0) {
      buf[pos++] = '\n';
      pos = diagfln(buf,pos,len,Fln);
      pos += snprintf_mini(buf,pos,len,"0 3    stats   --- yalloc %s stats for %s heap %u --- %s tag %.01u\n",yal_version,hb ? "" : "base ",hid,desc,tag);
      oswrite(fd,buf,pos,fln);// detail above
    }
  }

  if (issum == 0) {
    // sp->callocs = sp->reallocles = sp->reallocgts = 0;
    // sp->slaballocs = sp->slabxfrees = 0;

    sp->minlen = Hi32;
    sp->mapminlen = Size_max;
    sp->maxlen = 0;

    sp->delregion_cnt = sp->freeregion_cnt = sp->region_cnt = 0;

    sp->frecnt = sp->fresiz = sp->inuse = sp->inusecnt = 0;

    regstats(fd,hb,print,opts);
    if (hb) {
      if (detail) {
        mmapstats(fd,hb,print);
        bumpstats(fd,&hb->stat,hb->bumpregs,Bumpregions,print);
      }
    }
  }

  size_t slaballocs = sp->slaballocs;
  size_t slabAllocs = sp->slabAllocs;
  size_t mapallocs = sp->mapallocs;
  size_t mapAllocs = sp->mapAllocs;
  size_t callocs = sp->callocs;
  size_t alloc0s = sp->alloc0s;
  size_t reallocles = sp->reallocles;
  size_t reallocgts = sp->reallocgts;
  size_t slabfrees = sp->slabfrees;
  size_t free0s = sp->free0s;
  size_t freenils = sp->freenils;
  size_t slabxfrees = sp->slabxfrees;
  size_t mapfrees = sp->mapfrees;
  size_t mapxfrees = sp->mapxfrees;
  size_t mapreallocs = sp->mreallocles + sp->mreallocgts;
  size_t miniallocs = sp->miniallocs;
  size_t minifrees = sp->minifrees;
  size_t bumpallocs = sp->bumpallocs;
  size_t bumpfrees = sp->bumpfrees;

  size_t fresiz = sp->fresiz;
  ub4 maxlen = sp->maxlen;
  ub4 minlen = maxlen ? sp->minlen : 0;
  size_t maxrelen = sp->maxrelen;
  size_t minrelen = maxrelen ? sp->minrelen : 0;
  size_t mapmaxlen = sp->mapmaxlen;
  size_t mapminlen = mapmaxlen ? sp->mapminlen : 0;
  size_t newregs = sp->newregions;
  ub4 minclass = sp->maxclass ? sp->minclass : 0;

  // size_t xfreeavg = xfreebuf / max(xfreebatch,1);

  double clockperc;

  ub4 cnt,clas,*clascnts;

  errs = invalid_frees + sp->errors;

  sp->allocs = slaballocs + mapallocs;
  sp->frees = slabfrees + mapfrees;

  if (ret && ret != sp) {
    memcpy(ret,sp,sizeof(yalstats));
  }

  if (print) {
    pos = 0;

#if Yal_enable_stats
    if (newregs) {

      tpos = table(tbuf,0,tlen,7,8,"alloc",slaballocs,"alloc0",alloc0s,"calloc",callocs,"free",slabfrees,"free0",free0s,"freenil",freenils,"rfree",slabxfrees,"realloc",reallocles,"Realloc",reallocgts,"Alloc",slabAllocs,"size",sp->sizes,nil);
      pos += snprintf_mini(buf,pos,len,"\n-- slab summary --\n  counts  %.*s\n",tpos,tbuf);

      tpos = table(tbuf,0,tlen,7,8,"new",sp->newregions,"reuse",sp->useregions,"del",sp->delregions,"inuse",
        sp->region_cnt,"free",sp->freeregion_cnt,"del",sp->delregion_cnt,"no",sp->noregion_cnt,"mem",sp->slabmem,nil);
      pos += snprintf_mini(buf,pos,len,"  regions %.*s\n ",tpos,tbuf);

      tpos = table(tbuf,0,tlen,6,7,"mark",sp->trimregions[0],"unlist",sp->trimregions[1],"undir",sp->trimregions[2],"unmap",sp->trimregions[3],nil);
      if (tpos) pos += snprintf_mini(buf,pos,len,"  trim %.*s\n ",tpos,tbuf);

      pos += snprintf_mini(buf,pos,len,"  clas %3u-%-3u len %3u - %-3u real %3zu - %-3zu",minclass,sp->maxclass,minlen,maxlen,minrelen,maxrelen);
      pos += snprintf_mini(buf,pos,len,"  avail %zu` inuse %zu` in %'zu %s` adr %zx .. %zx\n",fresiz,sp->inuse,sp->inusecnt,"block",sp->loadr,sp->hiadr);
      pos += snprintf_mini(buf,pos,len,"  mmap %zu` unmap %zu`\n\n",sp->mmaps,sp->munmaps - sp->delmpregions);

      if (hb && detail) {
        pos += snprintf_mini(buf,pos,len,"clas size  count\n");
        clascnts = hb->clascnts;
        clascnts[0] = (ub4)min(alloc0s,Hi32);
        for (clas = 0; clas < Xclascnt; clas++) {
          cnt = clascnts[clas];
          if (cnt) {
            pos += snprintf_mini(buf,pos,len,"  %-2u %-6u %u`\n",clas,hb->claslens[clas],cnt);
          }
        }
        buf[pos++] = '\n';
        buf[pos++] = '\n';
      }
    }
    if (xfreebuf | xfreebatch | xfreebatch1 | xmapfrees) {
      pos += snprintf_mini(buf,pos,len,"  inter-thread free slab %-7zu` map %-7zu` buffer %zu` max %zu` batch %zu` + %zu` - %zu = %zu`b mmap %zu\n",
        xfreebuf + xfreebatch1,xmapfrees,xfreebuf,xmaxbin,xfreebatch,xfreebatch1,sp->xfreedropped,sp->xbufbytes,sp->rbinallocs);
    }
    if (bumpallocs | bumpfrees) pos += snprintf_mini(buf,pos,len,"  bump alloc %-3zu free %-3zu\n",bumpallocs,bumpfrees);
    if (miniallocs | minifrees) pos += snprintf_mini(buf,pos,len,"  mini alloc %-3zu free %-3zu\n",miniallocs,minifrees);

    if (sp->newmpregions) {
      tpos = table(tbuf,0,tlen,7,7,"alloc",mapallocs,"Allocs",mapAllocs,"realloc",mapreallocs,"free",mapfrees,"rfree",mapxfrees,"minlen",mapminlen,"maxlen",mapmaxlen,nil);
      pos += snprintf_mini(buf,pos,len,"\n-- mmap summary --\n  counts  %.*s\n",tpos,tbuf);
      tpos = table(tbuf,0,tlen,7,7,"new",sp->newmpregions,"use",sp->usempregions,"del",sp->delmpregions,"used",sp->xregion_cnt,"inuse",sp->inmapuse,nil);
      pos += snprintf_mini(buf,pos,len,"  regions %.*s\n",tpos,tbuf);
      tpos = table(tbuf,0,tlen,6,7,"mark",sp->trimregions[4],"unlist",sp->trimregions[5],"undir",sp->trimregions[6],"unmap",sp->trimregions[7],nil);
      if (tpos) pos += snprintf_mini(buf,pos,len,"  trim %.*s\n ",tpos,tbuf);
    }
      if (issum) pos += snprintf_mini(buf,pos,len,"  heaps new %2zu  used %2zu get %4zu` noget %4zu`,%-4zu`\n\n",newheaps,useheaps,sp->getheaps,sp->nogetheaps,sp->nogetheap0s);
    // pos += snprintf_mini(buf,pos,len,"  mmap %zu unmap %zu\n\n",sp->mmaps,sp->munmaps);

#endif
    if (locks) {
      clockperc = clocks ? (100.0 * (double)clocks / (double)locks) : 0.0;
      pos += snprintf_mini(buf,pos,len,"  lock %zu` clock %zu` = %.2f%%\n",locks,clocks,clockperc);
    }
    if (errs) pos += snprintf_mini(buf,pos,len,"  invalid-free %-4zu error %-3zu\n",invalid_frees,sp->errors);

    buf[pos++] = '\n';
    oswrite(fd,buf,pos,Fln);
  } // print

  return errs;
}

static void sumup(yalstats *sum,yalstats *one)
{
  ub4 i;

  sum->slaballocs += one->slaballocs;
  sum->slabAllocs += one->slabAllocs;
  sum->mapallocs += one->mapallocs;
  sum->mapAllocs += one->mapAllocs;
  sum->allocs += one->allocs;
  sum->alloc0s += one->alloc0s;
  sum->callocs += one->callocs;
  sum->bumpallocs += one->bumpallocs;
  sum->reallocles += one->reallocles;
  sum->reallocgts += one->reallocgts;
  sum->frees += one->frees;
  sum->free0s += one->free0s;
  sum->freenils += one->freenils;
  sum->bumpfrees += one->bumpfrees;
  sum->slabfrees += one->slabfrees;
  sum->slabxfrees += one->slabxfrees;
  sum->mapfrees += one->mapfrees;
  sum->mapxfrees += one->mapxfrees;
  sum->mreallocles += one->mreallocles;
  sum->mreallocgts += one->mreallocgts;

  sum->fresiz += one->fresiz;
  sum->frecnt += one->frecnt;
  sum->inuse += one->inuse;
  sum->inusecnt += one->inusecnt;
  sum->inmapuse += one->inmapuse;
  sum->mmaps += one->mmaps;
  sum->fremapsiz += one->fremapsiz;

  sum->newregions += one->newregions;
  sum->useregions += one->useregions;
  sum->delregions += one->delregions;
  sum->region_cnt += one->region_cnt;
  sum->xregion_cnt += one->xregion_cnt;
  sum->freeregion_cnt += one->freeregion_cnt;
  sum->delregion_cnt += one->delregion_cnt;
  sum->newmpregions += one->newmpregions;
  sum->usempregions += one->usempregions;
  sum->delmpregions += one->delmpregions;

  for (i = 0; i < 8; i++) sum->trimregions[i] += one->trimregions[i];

  sum->xslabfrees += one->xslabfrees;
  sum->xmapfrees += one->xmapfrees;
  sum->xfreebuf += one->xfreebuf;
  sum->xfreebatch += one->xfreebatch;
  sum->xfreebatch1 += one->xfreebatch1;
  sum->xfreedropped += one->xfreedropped;
  sum->rbinallocs += one->rbinallocs;
  sum->xbufbytes += one->xbufbytes;

  sum->locks += one->locks;
  sum->clocks += one->clocks;

  sum->newheaps += one->newheaps;
  sum->useheaps += one->useheaps;
  sum->nogetheaps += one->nogetheaps;
  sum->nogetheap0s += one->nogetheap0s;

  sum->maxlen = max(sum->maxlen,one->maxlen);
  sum->minlen = min(sum->minlen,one->minlen);
  sum->mapmaxlen = max(sum->mapmaxlen,one->mapmaxlen);
  sum->mapminlen = min(sum->mapminlen,one->mapminlen);
  sum->loadr = min(sum->loadr,one->loadr);
  sum->hiadr = max(sum->hiadr,one->hiadr);
  sum->minclass = min(sum->minclass, one->minclass);
  sum->maxclass = max(sum->maxclass, one->maxclass);

  sum->xmaxbin = max(sum->xmaxbin,one->xmaxbin);
  sum->invalid_frees += one->invalid_frees;
  sum->errors += one->errors;
}

// get and/or print stats from all heaps ( = threads )
// only one thread will print them
size_t Cold yal_mstats(yalstats *ret,ub4 opts,ub4 tag,const char *desc)
{
  size_t errs;
  heapdesc *hd = thread_heap;
#if Yal_enable_stats
  bool allthreads = (opts & Yal_stats_totals);
  ub4 print = opts & Yal_stats_print;

  heap *hb;
  bregion *mhb;
  unsigned long pid = Atomget(global_pid,Monone);
  size_t invfrees;
  ub4 hnew,huse;
  ub4 tidcnt,heapcnt = 0,mheapcnt = 0;
  bool didopen = 0;
  ub4 zero = 0;
  bool didcas = 0;
  yalstats sum,one;
  struct hdstats *ds;
  ub4 b;
  heapdesc dummyhd,*xhd;
  int fd = -1;
  ub4 pos = 0;
  char buf[4096];
  ub4 len = 4094;
  ub4 iter;

  static _Atomic ub4 oneprint; // Let only one thread print all heaps

  errs = 0;

  if (ret) {
    memset(ret,0,sizeof(yalstats));
    ret->version = yal_version;
  }

  if (hd == nil) {
    hd = &dummyhd;
    memset(hd,0,sizeof(heapdesc));
  }

  tidcnt = Atomget(global_tid,Monone);

  if (print) { // prevent mutiple threads printing simultaneoulsy : redirect to file if needed
    didcas = Cas(oneprint,zero,1);
    if (didcas) {
      fd = Yal_stats_fd;
      if (fd == -1) {
        fd = newlogfile(Yal_stats_file,allthreads ? "-all" : "",hd->id,pid);
        if (fd != 2) didopen = 1;
      }
    }
    if (fd == -1) print = 0;
  } // print

  if (allthreads == 0) {
    errs = yal_mstats_heap(fd,hd->hb,ret,print != 0,opts,tag,desc,Fln);
    if (print && didopen) osclose(fd);
    if (didcas) Atomset(oneprint,0,Morel);
    return errs;
  }
  memset(&sum,0,sizeof(sum));
  memset(&one,0,sizeof(one));
  sum.minlen = Hi32;
  sum.mapminlen = sum.loadr = Size_max;
  sum.minclass = Hi16;
  sum.version = yal_version;

  // mini heaps and heapdesc stats
  if (print) {
    *buf = '\n';
    pos = diagfln(buf,1,len,Fln);
    pos += snprintf_mini(buf,pos,len,"%-2u                yalloc stats for %u %s` and %u %s`\n\n",hd->id,tidcnt,"thread",Atomget(global_hid,Monone) - 1u,"heap");
    oswrite(fd,buf,pos,Fln);
    pos = 0;
  }

  xhd = Atomget(global_heapdescs,Monone);
  while (xhd) {
    ds = &xhd->stat;

    hnew = ds->newheaps;
    huse = ds->useheaps;

  // tallied outside of heap
    sum.munmaps += ds->munmaps;
    sum.xmapfrees += ds->xmapfrees;
    sum.xfreebatch1 += ds->xfreebatch;

    sum.alloc0s += ds->alloc0s;
    sum.free0s += ds->free0s;
    sum.freenils += ds->freenils;

    invfrees = ds->invalid_frees;
    sum.invalid_frees += invfrees;

    sum.newheaps += hnew;
    sum.useheaps += huse;

    sum.getheaps += ds->getheaps;
    sum.nogetheaps += ds->nogetheaps;
    sum.nogetheap0s += ds->nogetheap0s;

    if (print && (opts & Yal_stats_detail) ) {
      if (hnew | huse) pos += snprintf_mini(buf,pos,len,"heap base %u new %u  used %u get %zu noget %zu,%zu\n",xhd->id,hnew,huse,ds->getheaps,ds->nogetheaps,ds->nogetheap0s);
      if (pos > 2048) { oswrite(fd,buf,pos,Fln); pos = 0; }
    }
    if (invfrees) pos += snprintf_mini(buf,pos,len,"  invalid-free %-4zu error %-3zu\n",invfrees,ds->errors);

    mhb = xhd->mhb;

    if (mhb) { // mini
      mheapcnt++;
      bumpstats(fd,&one,mhb,1,print != 0);
      sum.miniallocs += one.miniallocs;
      sum.minifrees += one.minifrees;
    }
    xhd = xhd->nxt;
  }
  if (print && pos) {
    buf[pos++] = '\n';
    oswrite(fd,buf,pos,Fln);
    pos = 0;
  }

  // full heaps
  iter = 1000;
  hb = Atomget(global_heaps,Monone);
  while (hb && --iter) {
    heapcnt++;

    errs += yal_mstats_heap(fd,hb,&one,(print) && (opts & Yal_stats_sum),opts,tag,desc,Fln); // print and sum over regions

    sumup(&sum,&one); // sum over heaps

    hb = hb->nxt;
  }

  size_t slaballocs = sum.slaballocs;
  size_t slabAllocs = sum.slabAllocs;
  size_t mapallocs = sum.mapallocs;
  size_t mapAllocs = sum.mapAllocs;
  size_t slabfrees = sum.slabfrees;
  size_t slabxfrees = sum.slabxfrees;

  size_t mapfrees = sum.mapfrees;
  size_t mapxfrees = sum.mapxfrees;

  size_t mmaps,munmaps;
  struct bootmem *bootp;
  ub4 bootallocs,bootnolocks = 0;

  if (print) minidiag(Fln,Lstats,Info,hd->id,"\n--- yalloc %s stats totals over %u %s` and %u %s` in %u %s` --- %s tag %.01u\n",yal_version,heapcnt,"heap",mheapcnt,"miniheap",tidcnt,"thread",desc,tag);

  yal_mstats_heap(fd,nil,&sum,print != 0,opts | 0x80,tag,desc,Fln); // totals

  if (slabfrees + slabxfrees > slaballocs + slabAllocs) {
    error2(Lnone,Fln,"allocs %zu + %zu frees %zu + %zu",slaballocs,slabAllocs,slabfrees,slabxfrees)
  }

  if (mapfrees + mapxfrees > mapallocs + mapAllocs) {
    error2(Lnone,Fln,"map allocs %zu + %zu frees %zu + %zu",mapallocs,mapAllocs,mapfrees,mapxfrees)
  }

  if (print) {

    mmaps = Atomget(global_mapadd,Monone);
    munmaps = Atomget(global_mapdel,Monone);

    mmaps += sum.mmaps;
    munmaps += sum.munmaps;

    pos += snprintf_mini(buf,pos,len,"  boot allocs  ");
    for (b = 0; b < Bootcnt; b++) {
      bootp = bootmems + b;
      bootallocs = Atomget(bootp->allocs,Monone);
      bootnolocks |= Atomget(bootp->nolocks,Monone);
      mmaps += Atomget(bootp->mmaps,Monone);
      pos += snprintf_mini(buf,pos,len,"%-3u ",bootallocs);
    }

    mmaps += Atomget(global_hid,Monone);

    mmaps += sum.rbinallocs;

    pos += snprintf_mini(buf,pos,len,"  mmap %zu munmap %zu\n\n",mmaps,munmaps);

    buf[pos++] = '\n';
    if (bootnolocks) {
      pos += snprintf_mini(buf,pos,len,"     nolock ");
      for (b = 0; b < Bootcnt; b++) {
        bootp = bootmems + b;
        bootnolocks = Atomget(bootp->nolocks,Monone);
        pos += snprintf_mini(buf,pos,len,"%-3u ",bootnolocks);
      }
    }

    // add rusage(2)
    struct osrusage usg;
    char tbuf[256];
    ub4 tpos;

    memset(&usg,0,sizeof(usg));
    osrusage(&usg);

    tpos = table(tbuf,0,256,7,8,"user msec",usg.utime,"sys msec",usg.stime,"max rss",usg.maxrss,"spage",usg.minflt,"page",usg.maxflt,"vol cswitch",usg.volctx,"cswitch",usg.ivolctx,nil);
    pos += snprintf_mini(buf,pos,len,"\n  %.*s\n",tpos,tbuf);

    pos += snprintf_mini(buf,pos,len,"\n  -- end of yalloc stats -- \n\n");
    oswrite(fd,buf,pos,Fln);

    if (opts & Yal_stats_cfg) oswrite(fd,global_cfgtxt,sizeof(global_cfgtxt) - 1,Fln);
  }

  if (didopen) osclose(fd);
  if (didcas) Atomset(oneprint,0,Morel);

  if (ret) {
    memcpy(ret,&sum,sizeof(sum));
    ret->version = yal_version;
  }
#else
  errs = hd ? hd->stat.invalid_frees : 0;
#endif
  return errs;
}

static void yal_trigger_stats(size_t size)
{
  size_t trig = size >> 16;
  ub4 tag = size & Hi16;
  if (trig == Yal_trigger_stats || trig == Yal_trigger_stats_threads) {
    ub4 opt = Yal_stats_print | Yal_stats_detail;
    if (trig == Yal_trigger_stats_threads) opt |= Yal_stats_totals;
    yal_mstats(nil,opt,tag,"calloc(0,Yal_trigger_stats)");
  }
}

#undef Logfile
