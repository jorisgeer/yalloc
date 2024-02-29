/* genadm.c - generate meta tables

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <stdarg.h>
#include <time.h>

#include <stdio.h>

#include "base.h"

#include "config.h"

#include "printf.h"

#define Buffer 1024

static char lineorgbuf[Buffer];
static char lineorgsbuf[Buffer];
static char accorgbuf[Buffer];
static char accorgsbuf[Buffer];

static ub4 lineorgspos,accorgspos;

static ub4 buddy_linesizes[Maxorder];
static ub4 buddy_accsizes[Maxorder];

static ub4 linesizes[Maxorder];
static ub4 accAsizes[Maxorder];

static void genmap(ub2 order,FILE *fp)
{
  ub2 ord;

  ub4 linorg = 0;
  ub2 acc,accorg = 0;
  ub4 len;
  ub4 acclen;
  ub4 lineorgpos,accorgpos;
  char *comma,*newline;

  if (order > 6) {
    len = 1U << (order - 6);
    acclen = max(len >> 6,1);
  } else len = acclen = 1;

  // Prepare line origins
  lineorgpos = mini_snprintf(lineorgbuf,0,Buffer,"static const ub4 _linorg%u[] = {",order);
  accorgpos = mini_snprintf(accorgbuf,0,Buffer,"static const ub2 _accorg%u[] = {",order);

  comma = order > Minregion ? "," : "";
  lineorgspos += mini_snprintf(lineorgsbuf,lineorgspos,Buffer,"%s_linorg%u",comma,order);
  accorgspos += mini_snprintf(accorgsbuf,accorgspos,Buffer,"%s_accorg%u",comma,order);

  for (ord = Minorder; ord < min(order,Maxorder); ord++) {
    comma = ord > Minorder ? "," : "";
    newline = ((ord - Minorder) & 7) == 7 ? "\n  " : "";
    if (order - ord < 4) newline = "";
    lineorgpos += mini_snprintf(lineorgbuf,lineorgpos,Buffer,"%s%s%#x",comma,newline,linorg);
    accorgpos += mini_snprintf(accorgbuf,accorgpos,Buffer,"%s%s%#x",comma,newline,accorg);

    linorg += max(1,len); len >>= 1;
    for (acc = 0; acc < 4; acc++) {
        accorg += max(acclen,1);
        acclen >>= 6;
     }
  }
  if (order < Maxorder) {
    linesizes[order] = linorg;
    accsizes[order] = accorg;
  }
  mini_snprintf(lineorgbuf,lineorgpos,Buffer,"}; // %u`\n",linorg);
  mini_snprintf(accorgbuf,accorgpos,Buffer,"};\n");
  // fprintf(fp,"%s\n%s\n\n",lineorgbuf,accorgbuf);
  fprintf(fp,"%s\n\n",accorgbuf);
}

static unsigned char snipinit[] = "\
";

static unsigned char dirsnip1[] = "\n\
  o1 = org >> Page1;\n\
  e1 = end >> Page1;\n\
  dir1 = hb->rootdir;\n\
\n\
  do { // while o1 < e1\n\
    dp1 = dir1 + o1++;\n\
    if ( (o1 & m1) == 0) {\n\
      dp1->reg = reg;\n\
      continue;\n\
    }\n\
    dp1->reg = 0;\n\
    dir2 = dp1->dir;\n\
    if (dir2 == nil) {\n\
      dir2 = newdir(hb);\n\
      dp1->dir = dir2;\n\
    }\n\
\n\
    pg2 = Maxvm - 2 * Dir;\n\
    e2 = (end >> pg2) & ((`1ul << Page2) - `1);\n\n";

static unsigned char leafsnip1[] = "\n\
  o1 = org >> Page1;\n\
\n\
  do { // while o1 < e1\n\
    dp1 = dir1 + o1++;\n\
    o2 = org & m1;\n\
    if (o2 == 0) {\n\
      dp1->reg = reg;\n\
      continue;\n\
    }\n\
    dp1->reg = 0;\n";

static unsigned char snipwhile[] = "} while (o1 < e1);\n\n";

static unsigned char *patchsnip(unsigned char *snip,unsigned  char lvl)
{
  static unsigned char dst[4096];
  unsigned char c;
  ub4 i = 0,o = 0;;

  do {
    c = snip[i++];
    if (c == '`') { // literal
      c = snip[i++];
      dst[o++] = c;
      continue;
    }
    if (c == '1' ) c = lvl + '1';
    else if  (c == '2') c = lvl + '2';
    dst[o++] = c;
  } while (c && i < sizeof(dst) - 1);
  dst[i] = 0;
  return dst;
}

static int _Printf(2,3) error(ub4 line,cchar *fmt,...)
{
  va_list ap;
  ub4 n = 0;
  char buf[256];

  if (line) n = mini_snprintf(buf,0,250,"Error genadm.c:%u - ",line);

  va_start(ap,fmt);
  n += mini_vsnprintf(buf,n,250,fmt,ap);
  va_end(ap);

  buf[n++] = '\n';
  fwrite(buf,1,n,stderr);
  return 1;
}

static int gendir(FILE *fp)
{
  char buf[4096];
  ub4 len = 4096;
  ub4 n,pos,lvl;
  ub4 bits = Maxvm - Minregion;
  unsigned char *snip;
  unsigned char c;

  snip = patchsnip(dirsnip1,0);
  pos = mini_snprintf(buf,0,len,"\n// +++ dirsnip lvl 0 +++\n%s\n",snip);
  lvl = 0;

  do {
    bits -= Dir;
    lvl++;
    snip = patchsnip(dirsnip1,(ub1)lvl);
    pos += mini_snprintf(buf,pos,len,"// +++ dirsnip lvl %u +++\n%s\n",lvl,snip);
  } while (bits > Dir);

  fprintf(stderr,"%u levels\n",lvl);

  // leaf
  snip = patchsnip(leafsnip1,(ub1)lvl + 1);
  pos += mini_snprintf(buf,pos,len,"// +++ leaf  +++\n%s\n",snip);

  snip = patchsnip(snipwhile,(ub1)lvl + 1);
  pos += mini_snprintf(buf,pos,len,"%*s  %s\n",lvl * 2 + 2," ",snip);

  for (n = 0; n < lvl + 2; n++) {
    c = (ub1)n + '1';
    fprintf(fp,"  #define m%u 0x%zx\n",n+1,(1ul << (Maxvm - n * Dir)) - 1);
    fprintf(fp,"  #define Page%u %u\n",n+1,Maxvm - n * Dir);
  }
  fprintf(fp,"  #define Page%u %u\n",n+1,Maxvm - n * Dir);

  for (n = 0; n < lvl + 3; n++) {
    c = (ub1)n + '1';
    fprintf(fp,"  ub8 o%c,e%c;\n",c,c);
  }
  for (n = 0; n < lvl + 2; n++) {
    c = (ub1)n + '1';
    fprintf(fp,"  ub4 pg%c;\n",c);
  }
  for (n = 0; n < lvl + 2; n++) {
    fprintf(fp,"  struct direntry *dir%c,*dp%c;\n%s",n + '1',n + '1',n == lvl - 1 ? "\n" : "");
  }

  // fprintf(fp,"%s\n\n",snipinit);

  do {
    snip = patchsnip(snipwhile,(ub1)lvl);
    pos += mini_snprintf(buf,pos,len,"%*s  %s\n",lvl * 2," ",snip);
  } while (lvl--);

  if (pos  > len - 4) return error(__LINE__,"gendir buffer overflow");;
  fwrite(buf,1,pos,fp);
  return 0;
}

static void header(FILE *fp,cchar *name,cchar *desc,char *timestr)
{
  fprintf(fp,"/* %s - %s for yalloc\n\n  Generated by genadm at %s/\n\n",name,desc,timestr);
  fprintf(fp,"  Based on config.h Minorder %u Maxorder %u Minregion %u */\n\n ",Minorder,Maxorder,Minregion);
}

int main(int argc,char *argv[])
{
  ub2 ord,reg;
  FILE *fp,*dirfp;
  char *layoutname,*dirname;
  char timebuf[256];
  char bck[4096];
  struct tm *nowtm;
  time_t now;

  if (argc < 3) return error(0,"usage: genadm <layout_file> <dir_code>");

  layoutname = argv[1];
  dirname = argv[2];

  mini_snprintf(bck,0,4096,"%s.bak",layoutname);
  rename(layoutname,bck);
  fp = fopen(layoutname,"w");
  if (!fp) return error(__LINE__, "cannot create '%s': %m",layoutname);

  mini_snprintf(bck,0,4096,"%s.bak",dirname);
  rename(dirname,bck);
  dirfp = fopen(dirname,"w");
  if (!dirfp) return error(__LINE__, "cannot create '%s': %m",dirname);

  now = time(NULL);
  nowtm = gmtime(&now);
  strftime(timebuf,256,"%a %e %b %R UTC",nowtm);

  header(fp,layoutname,"admin layout",timebuf);
  header(dirfp,dirname,"region directory",timebuf);

  if (gendir(dirfp)) return 1;

  lineorgspos = mini_snprintf(lineorgsbuf,0,Buffer,"static const ub4 *lineorgs = {");
  accorgspos = mini_snprintf(accorgsbuf,0,Buffer,"static const ub2 accelorgs = {");

  for (reg = Minregion; reg < Maxregion; reg++) {
    genmap(reg,fp);
  }

  lineorgspos += mini_snprintf(lineorgsbuf,lineorgspos,Buffer,"};\n");
  accorgspos += mini_snprintf(accorgsbuf,accorgspos,Buffer,"}'\n");
  // fprintf(fp,"%s\n%s\n",lineorgsbuf,accorgsbuf);
  fprintf(fp,"%s\n",accorgsbuf);

  fprintf(fp,"static const ub4 buddy_linesizes[] = {%u",buddy_linesizes[Minorder]);
  for (ord = Minorder+1; ord < Maxorder; ord++) {
    fprintf(fp,",%u",buddy_linesizes[ord]);
  }
  fputs("};\n",fp);
  fprintf(fp,"static const ub4 buddy_accsizes[] = {%u",buddy_accsizes[Minorder]);
  for (ord = Minorder+1; ord < Maxorder; ord++) {
    fprintf(fp,",%u",buddy_accsizes[ord]);
  }
  fputs("};\n",fp);

  fclose(fp);
  fclose(dirfp);

  printf("generated %s and %s\n",layoutname,dirname);
  return 0;
}
