/* configure.c - determine configuration

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

   Determine system page size and type sizes.
*/

#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <stdio.h> // rename
#include <time.h>

#define Sys_page 12 // default
#include "config.h"

#ifdef Inc_os
  #include "os.c"
#else
  #include "os.h"
#endif

#include "base.h"

#ifdef Miniprint_inc
  #include <errno.h> // %m
  #include "printf.c"
#else
  #include "printf.h"
#endif

extern Noret void exit(int);
#define L __LINE__

static const int Error_fd = 2;

#include "yalloc.h"

enum Loglvl { Fatal,Assert,Error,Warn,Info,Vrb,Debug,Nolvl };
static cchar * const lvlnames[Nolvl + 1] = { "Fatal","Assert","Error","Warn","Info","Verb","Debug"," " };
static enum Loglvl loglvl = Info;

static ub4 dolog(ub4 fln,enum Loglvl lvl,cchar *fmt,va_list ap)
{
  char buf[1024];
  ub4 pos = 0,len = 1022;

  if (lvl > loglvl) return 0;

  if (lvl < Info || loglvl > Info) pos = snprintf_mini(buf,0,len,"configure.c:%-3u %c - ",fln,*lvlnames[lvl]);
  pos += mini_vsnprintf(buf,pos,len,fmt,ap);
  buf[pos++] = '\n';
  oswrite(Error_fd,buf,pos,fln);
  return pos;
}

static Printf(2,3) Noret void fatal(ub4 fln,cchar *fmt,...)
{
  va_list ap;

  va_start(ap,fmt);
  dolog(fln,Fatal,fmt,ap);
  va_end(ap);
  oswrite(Error_fd,"\nexiting\n",9,fln);
  exit(1);
}

static Printf(2,3) ub4 error(ub4 fln,cchar *fmt,...)
{
  va_list ap;

  va_start(ap,fmt);
  dolog(fln,Error,fmt,ap);
  va_end(ap);
  return 0;
}

static Printf(2,3) ub4 warning(ub4 fln,cchar *fmt,...)
{
  va_list ap;
  ub4 pos;

  va_start(ap,fmt);
  pos = dolog(fln,Warn,fmt,ap);
  va_end(ap);
  return pos;
}

static Printf(2,3) ub4 info(ub4 fln,cchar *fmt,...)
{
  va_list ap;
  ub4 pos;

  va_start(ap,fmt);
  pos = dolog(fln,Info,fmt,ap);
  va_end(ap);
  return pos;
}

static Printf(2,3) ub4 verb(ub4 fln,cchar *fmt,...)
{
  va_list ap;
  ub4 pos;

  va_start(ap,fmt);
  pos = dolog(fln,Vrb,fmt,ap);
  va_end(ap);
  return pos;
}

static ub4 write_cfg(char *buf,ub4 pos,ub4 len)
{
  pos += snprintf_mini(buf,pos,len," log_fd %d \\n\\\n",Yal_log_fd);
  pos += snprintf_mini(buf,pos,len," err_fd %d \\n\\\n",Yal_err_fd);
  pos += snprintf_mini(buf,pos,len," Err_fd %d \\n\\\n",Yal_Err_fd);
  pos += snprintf_mini(buf,pos,len," stats_fd %d \\n\\\n",Yal_stats_fd);
  pos += snprintf_mini(buf,pos,len," log_file %s \\n\\\n",*Yal_log_file);
  pos += snprintf_mini(buf,pos,len," err_file %s \\n\\\n",*Yal_err_file);
  pos += snprintf_mini(buf,pos,len," stats_file %s \\n\\\n",*Yal_stats_file);

  pos += snprintf_mini(buf,pos,len," check_default %u \\n\\\n",Yal_check_default);
  pos += snprintf_mini(buf,pos,len," enable_stats %u \\n\\\n",Yal_enable_stats);
  pos += snprintf_mini(buf,pos,len," trigger_stats %u \\n\\\n",Yal_trigger_stats_threads);
  pos += snprintf_mini(buf,pos,len," enable_trace %u \\n\\\n",Yal_enable_trace);
  pos += snprintf_mini(buf,pos,len," trace_default %u \\n\\\n",Yal_trace_default);
  pos += snprintf_mini(buf,pos,len," enable_tag %u \\n\\\n",Yal_enable_tag);
  pos += snprintf_mini(buf,pos,len," enable_check %u \\n\\\n",Yal_enable_check);
  pos += snprintf_mini(buf,pos,len," dbg_level %u \\n\\\n",Yal_dbg_level);
  pos += snprintf_mini(buf,pos,len," enable_stack %u \\n\\\n",Yal_enable_stack);
  pos += snprintf_mini(buf,pos,len," enable_valgrind %u \\n\\\n",Yal_enable_valgrind);
  pos += snprintf_mini(buf,pos,len," signal %u \\n\\\n",Yal_signal);
  pos += snprintf_mini(buf,pos,len," errno %u \\n\\\n",Yal_errno);
  pos += snprintf_mini(buf,pos,len," regfree_interval %u \\n\\\n",regfree_interval);
  pos += snprintf_mini(buf,pos,len," Trim_ages %u %u %u \\n\\\n",Trim_ages[0],Trim_ages[1],Trim_ages[2]);
  pos += snprintf_mini(buf,pos,len," Trim_Ages %u %u %u \\n\\\n",Trim_Ages[0],Trim_Ages[1],Trim_Ages[2]);
  pos += snprintf_mini(buf,pos,len," Trim_scan %u \\n\\\n",Trim_scan);
  pos += snprintf_mini(buf,pos,len," Vmbits64 %u \\n\\\n",Vmbits64);
  pos += snprintf_mini(buf,pos,len," Minregion %u \\n\\\n",Minregion);
  pos += snprintf_mini(buf,pos,len," Mmap_threshold %u \\n\\\n",Mmap_threshold);
  pos += snprintf_mini(buf,pos,len," Mmap_max_threshold %u \\n\\\n",Mmap_max_threshold);
  pos += snprintf_mini(buf,pos,len," Xclas_threshold %u \\n\\\n",Xclas_threshold);
  pos += snprintf_mini(buf,pos,len," Clas_threshold %u \\n\\\n",Clas_threshold);
  pos += snprintf_mini(buf,pos,len," Cel_nolen %u \\n\\\n",Cel_nolen);
  pos += snprintf_mini(buf,pos,len," Bumplen %u \\n\\\n",Bumplen);
  pos += snprintf_mini(buf,pos,len," Bumpmax %u \\n\\\n",Bumpmax);
  pos += snprintf_mini(buf,pos,len," Bumpregions %u \\n\\\n",Bumpregions);
  pos += snprintf_mini(buf,pos,len," Minilen %u \\n\\\n",Minilen);
  pos += snprintf_mini(buf,pos,len," Minimax %u \\n\\\n",Minimax);
  pos += snprintf_mini(buf,pos,len," Regmem_inc %u \\n\\\n",Regmem_inc);
  pos += snprintf_mini(buf,pos,len," Xregmem_inc %u \\n\\\n",Xregmem_inc);
  pos += snprintf_mini(buf,pos,len," Dirmem_init %u \\n\\\n",Dirmem_init);
  pos += snprintf_mini(buf,pos,len," Dirmem %u \\n\\\n",Dirmem);
  pos += snprintf_mini(buf,pos,len," L1line %u \\n\\\n",L1line);
  pos += snprintf_mini(buf,pos,len," thread_exit %u \\n\\\n",Yal_thread_exit);
  pos += snprintf_mini(buf,pos,len," Contention %u \\n\\\n",Contention);
  pos += snprintf_mini(buf,pos,len," prep_TLS %u \\n\\\n",Yal_prep_TLS);
  pos += snprintf_mini(buf,pos,len," Bootmem %u \\n\\\n",Bootmem);
  pos += snprintf_mini(buf,pos,len," Basealign %u \\n\\\n",Basealign);
  pos += snprintf_mini(buf,pos,len," Stdalign %u \\n\\\n",Stdalign);
  pos += snprintf_mini(buf,pos,len," log_level %u \\n\\\n",Yal_log_level);
  pos += snprintf_mini(buf,pos,len," enable_extensions %u \\n\\\n\\n",Yal_enable_extensions);

  return pos;
}

static ub4 header(char *buf,ub4 len,cchar *name,cchar *desc,char *timestr)
{
  int cfd;
  struct osstat st;
  char timebuf[256];
  struct tm *cfgtm;
  ub4 pos;

  cfd = osopen("config.h",&st);
  if (cfd == -1) return error(L,"cannot open config.h for %s: %m",desc);
  osclose(cfd);
  if (st.len < 64) return error(L,"config.h is empty(%lu)",st.len);
  else if (st.len > Hi16) warning(L,"config.h has size %lu",st.len);
  cfgtm = gmtime((time_t *)&st.mtime);
  strftime(timebuf,256,"%a %e %b %R UTC",cfgtm);

  pos =  snprintf_mini(buf,0,len,"/* %s - %s for yalloc %s\n\n  Generated by configure at %s\n\n",name,desc,yal_version,timestr);
  pos +=  snprintf_mini(buf,pos,len,"  based on config.h(%u) %s\n",(ub4)st.len,timebuf);
  pos +=  snprintf_mini(buf,pos,len," */\n\n");
  return pos;
}

static ub4 genclasses(char *buf,ub4 pos,ub4 blen)
{
  ub4 len,ord,cord,alen,clen,clasal;
  ub4 clas,hiclas = 0,basclas = 0;
  ub4 grain = class_grain;
  ub4 grain1 = class_grain1;
  char comma;
  static ub4 clas2len[Clascnt];

  clas = 0; // reserve for len 0
  pos += snprintf_mini(buf,pos,blen,"\nstatic const unsigned char len2clas[%u] = { // %u \n  %3u,",Smalclas,grain,clas);

  for (len = 1; len <  64; len++) {
    switch(len) {
    case 1: case 2: alen = 2; clas = 1; break;
    case 3: case 4: alen = 4; clas = 2; break;
    case 5: case 6: case 7: case 8: alen = 8; clas = 3; break;
    case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16: alen = 16; clas = 4; break;
    default:
#if Stdalign <= 8
      if (len <= 16 + 8) { alen = 16 + 8; clas = 5; }
      else if (len <= 16 + 2 * 8) { alen = 16 + 2 * 8; clas = 6; }
      else if (len <= 16 + 3 * 8) { alen = 16 + 3 * 8; clas = 7; }
      else if (len <= 16 + 4 * 8) { alen = 16 + 4 * 8; clas = 8; }
      else { alen = 64; clas = basclas = 9; }
#elif Stdalign <= 16
      if (len <= 32) { alen = 32; clas = 5; }
      else if (len <= 32 + 16) { alen = 32 + 16; clas = 6; }
      else { alen = 64; clas = basclas = 7; }
#else
  #error "Stdalign > 16 not supported"
#endif
    }
    // info(L,"class %u len %u / %u",clas,alen,clas2len[clas]);
    if (clas2len[clas] == 0) clas2len[clas] = alen;
    else if (alen != clas2len[clas]) fatal(L,"class %u len %u vs %u",clas,alen,clas2len[clas]);

    pos += snprintf_mini(buf,pos,blen,"%3u,",clas);
    if  ( (len & 3) == 3) pos += snprintf_mini(buf,pos,blen," // %3u - %3u\n  ",len & ~3u,len);
  }

  for (len = 64; len <  Smalclas; len++) {
    if (len & (len - 1)) {
      ord = 32 - clz(len);
      cord = ord - grain;
      clasal = 1u << cord;
      alen = doalign4(len,clasal);
      clen = (alen >> cord) & grain;
      if (clen == 0) clen = 4;
      clas = ord * (grain + 1) + clen + basclas - 7 * grain1;
      verb(L,"clas %2u for len %5u ord %u.%u alen %u clen %x",clas,len,ord,cord,alen,clen);
      if (clas2len[clas] == 0) clas2len[clas] = alen;
      else if (alen != clas2len[clas]) fatal(L,"class %u len %u vs %u",clas,alen,clas2len[clas]);
    } else { // pwr2
      ord = ctz(len);
      clas = (ord + 1) * grain1 + basclas - 7 * grain1; // smal uses 8 and covers 6 * (grain + 1)
      if (clas2len[clas] == 0) clas2len[clas] = len;
      else if (len != clas2len[clas]) fatal(L,"class %u len %u vs %u",clas,len,clas2len[clas]);
      verb(L,"clas %2u for len %5u ord %u",clas,len,ord);
    }
    hiclas = max(clas,hiclas);

    comma = (len < Smalclas - 1) ? ',' : ' ';
    pos += snprintf_mini(buf,pos,blen,"%3u%c",clas,comma);
    if  ( (len & 7) == 7) pos += snprintf_mini(buf,pos,blen," // %3u - %3u\n  ",len & ~7u,len);
  }

  pos += snprintf_mini(buf,pos,blen,"}; // len2clas max %u \n\n",hiclas);

  pos += snprintf_mini(buf,pos,blen,"\nstatic const unsigned char clas2len[%u] = { ",basclas + 1);

  for (clas = 0; clas <=  basclas; clas++) {
    pos += snprintf_mini(buf,pos,blen,"%s%2u",clas ? "," : "",clas2len[clas]);
  }
  pos += snprintf_mini(buf,pos,blen," };\n\n");

  pos += snprintf_mini(buf,pos,blen,"#define Baseclass %u\n\n",basclas);

  return pos;
}

static ub4 genconfig(cchar *name,ub4 pagebits,char *nowtim)
{
  ub4 len = 0x4000;
  char buf[0x4000];
  char bck[2048];
  int fd;
  ub4 pos;
  ub4 pagesize,vmbits,dirbits,dir[3],dirone=0;
  size_t vmsize;
  cchar *byte8,*sizet;

  ub4 intsiz = (ub4)sizeof(int);
  ub4 longsiz = (ub4)sizeof(long);
  ub4 llongsiz = (ub4)sizeof(long long);
  ub4 ptrsiz = (ub4)sizeof(void *);

  info(L,"  int %u long %u llong %u ptr %u",intsiz,longsiz,llongsiz,ptrsiz);

  pos = snprintf_mini(bck,0,len,"%s.bak",name);
  if (pos) {
    if (rename(name,bck) && errno != ENOENT) warning(L,"cannot rename %s to %s: %m",name,bck);
  }
  if (sizeof(int) == 8) byte8 = "int";
  else if (sizeof(long) == 8) byte8 = "long";
  else if (sizeof(long long) == 8) byte8 = "long long";
  else return error(L,"cannot determine 8-byte integer type");

  if (sizeof(void *) == 4) {
    sizet = "0xffffffffull";
    vmbits = 32;
    vmsize = 0xfffffffful; // -1 to use regular long for limit
  } else if (sizeof(void *) == 8) {
    sizet = "0xfffffffffffffffful";
    vmbits = Vmbits64;
    vmsize = 1ul << min(vmbits,63); // -V547
  } else return error(L,"unsupported platform, sizeof(void *) %zu",sizeof(void *));

  if (pagebits) {
    pagesize = 1u << pagebits; // user override
    info(L,"  pagesize %u (overridden),, VMsize %u bits",pagesize,vmbits);
  } else { // get from system
    pagesize = ospagesize();
    if (pagesize) {
      pagebits = ctz(pagesize);
      info(L,"  pagesize %u (from os), VMsize %u bits",pagesize,vmbits);
    } else {
      pagebits = Sys_page;
      pagesize = 1u << Sys_page;
      warning(L,"Cannot get pagesize, using default: %m");
      info(0,"  pagesize %u, VMsize %u bits",pagesize,vmbits);
    }
  }

  if (pagebits >= vmbits) fatal(L,"page bits %u above Vmbits %u",pagebits,vmbits);
  dirbits = vmbits - pagebits;
  if (dirbits < 3) fatal(L,"Vmsize  %u page bits %u",vmbits,pagebits);

  dir[0] = dir[1] = dir[2] = 0;
  //coverity[INFINITE_LOOP]
  while (dir[0] + dir[1] + dir[2] != dirbits) {
    dir[dirone]++;
    if (dirone == 2) dirone = 0;
    else dirone++;
  }

  pos = header(buf,len,name,"generated config",nowtim);
  if (pos == 0) return 0;

  pos += snprintf_mini(buf,pos,len,"#define Sys_page %u // %u`\n\n",pagebits,pagesize);
  pos += snprintf_mini(buf,pos,len,"#define Vmbits %u\n",vmbits);
  pos += snprintf_mini(buf,pos,len,"#define Vmsize 0x%zxul // %zu`\n\n",vmsize,vmsize);

  for (dirone = 0; dirone < 3; dirone++) {
    dirbits = dir[dirone];
    pos += snprintf_mini(buf,pos,len,"#define Dir%u %u // %u`\n",dirone + 1,dirbits,1u << dirbits);
  }
  pos +=  snprintf_mini(buf,pos,len,"typedef unsigned %s Ub8;\n\n",byte8);

  pos +=  snprintf_mini(buf,pos,len,"#define Size_max %s\n\n",sizet);

  pos += snprintf_mini(buf,pos,len,"static const unsigned int global_sizes[4] = { %u,%u,%u,%u }; // int long llong ptr\n\n",intsiz,longsiz,llongsiz,ptrsiz);

  pos = genclasses(buf,pos,len);

  pos += snprintf_mini(buf,pos,len,"static const char global_cfgtxt[] = \"\\\n");
  pos = write_cfg(buf,pos,len);
  pos += snprintf_mini(buf,pos,len,"\";\n");

  fd = oscreate(name);
  if (fd == -1) return error(L, "cannot create '%s': %m",name);
  oswrite(fd,buf,pos,L);
  osclose(fd);

  info(L,"  created %s(%u) based on config.h %s",name,pos,nowtim);
  info(L,"  configured for a %u-bit system",ptrsiz * 8);
  return pos;
}

static int usage(void) {
  info(0,"usage: configure <layout_file>");
  return 1;
}

int main(int argc,char *argv[])
{
  char timebuf[256];
  struct tm *nowtm;
  time_t now;
  ub4 pagebits = 0;
  cchar *arg;
  char c;

  if (argc > 1) {
    arg = argv[1];
    if (arg[0] == '-') {
      c  = arg[1];
      switch (c) {
      case  'h': case '?': return usage();
      case 'v': if (loglvl < Nolvl) loglvl++; break;
      case '-':
        if (strcmp(arg + 1,"help") == 0) return usage();
        break;
      }
    }
  }

  info(0,"  generating config for yalloc %s",yal_version);
  now = time(NULL);
  nowtm = gmtime(&now);
  strftime(timebuf,256,"%a %e %b %R UTC",nowtm);

#ifdef Page_override
  pagebits = Sys_page; // fallback
  if (Page_override >= 32) error(L,"Page_override (%u) is a power of two",Page_override);
  if (Page_override < 8) pagebits = Sys_page;
  else pagebits = Page_override;
#endif
  if (genconfig("config_gen.h",pagebits,timebuf) == 0) return 1;

  return 0;
}
