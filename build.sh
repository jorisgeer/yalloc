#!/bin/sh

# build.sh - build script for yalloc

#   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

#   SPDX-FileCopyrightText: © 2024 Joris van der Geer
#   SPDX-License-Identifier: GPL-3.0-or-later

set -f
set -eu

tool=guess
dbg=0
dev=0
osinc=0
printinc=1

usage()
{
  echo 'usage: build [options] [target]'
  echo
  echo '-a  - analyze'
  echo '-d  - development mode'
  echo '-o  - separate object files'
  echo '-q  - quick - build yalloc.o only'
  echo '-t  - also build test'
  echo '-m  - create map file'
  echo '-v  - verbose'
  echo 'V  - verify'
  echo '-h  - help'
}

error()
{
  echo $1
  exit 1
}

if [ $tool = 'guess' ]; then
  if which gcc-14 2>/dev/null; then tool=gcc-14
  elif which gcc 2>/dev/null; then tool=gcc
  elif which clang 2>/dev/null; then tool=clang
  elif which icx 2>/dev/null; then tool=icx
  elif which icc 2>/dev/null; then tool=icc
  elif which dmd 2>/dev/null; then tool=dmd
  elif which cc 2>/dev/null; then tool=cc
  else
    error 'unable to guess compiler'
  fi
fi

date=$(date -u '+%Y%m%d')
time=$(date -u '+%H%M')

# clang >= 8 aug 19  '-mbranch-protection=none'  aarch64 only
# clang >= 7 '-fcf-protection=none'

# refer: https://sourceware.org/glibc/manual/latest/html_node/Replacing-malloc.html

case $tool in
  'clang' | 'icx')
  cc=$tool
  cdiag='-Wall -Wextra -Wunused -Wno-unused-command-line-argument -Wsign-conversion -Wchar-subscripts -Werror=format -Werror=return-type -Wno-poison-system-directories'
  cfmt='-fno-caret-diagnostics -fno-color-diagnostics -fno-diagnostics-show-option -fno-diagnostics-fixit-info -fno-diagnostics-show-note-include-stack -fno-show-column'
  cxtra='-std=c11 -funsigned-char -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -fpic -ftls-model=initial-exec -fno-plt'
  cana="--analyze"
  if [ $dbg -eq 1 ]; then
    cdbg='-g -fsanitize=undefined,signed-integer-overflow,bounds -fno-sanitize-recover=all -ftrapv -fstack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3'
    UBSAN_OPTIONS=print_stacktrace=1
    libs=
  else
    cdbg='-gline-tables-only -fno-stack-protector -fwrapv -fcf-protection=none -fno-asynchronous-unwind-tables -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0' # -fno-stack-clash-protection
    libs=
  fi
  copt='-O2'
  lflags="-g"
  ;;

# a64 gcc >= 8 2018 -fcf-protection  -fno-stack-clash-protection'

  'gcc' | 'icc' | 'gcc-14')
  cc=$tool
  cdiag='-Wall -Wextra -Wshadow -Wundef -Wunused -Wformat-overflow=2 -Wformat-truncation=2 -Winline -Werror=stack-usage=35000'
  cfmt='-fmax-errors=60 -fno-diagnostics-show-caret -fno-diagnostics-show-option -fno-diagnostics-color -fcompare-debug-second'
  cxtra='-std=c11 -funsigned-char -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -fpic -ftls-model=initial-exec -fno-plt'
  cana="-fanalyzer"
  if [ $dbg -eq 1 ]; then
    cdbg='-g -fsanitize=address,undefined,signed-integer-overflow,bounds -fno-sanitize-recover=all -ftrapv -fstack-protector'
    UBSAN_OPTIONS=print_stacktrace=1
  else
    cdbg='-g -fno-stack-protector -fcf-protection=none -fno-stack-clash-protection -fno-asynchronous-unwind-tables -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0'
  fi
  copt='-O2 -fwrapv -fgcse-after-reload -ftree-partial-pre -fsplit-paths'
  lflags="-g"
    libs=
  ;;

  'dmd')
  cc=dmd
  cdiag=
  cfmt=
  cxtra='-betterC'
  cana=
  cdbg='-g'
  copt='-O'
  lflags=
  ;;

# unknown aka standard
  'cc')
  cc=cc
  cdiag=
  cfmt=
  cxtra=
  cana=
  cdbg='-g'
  copt='-O'
  lflags=
  ;;
esac

cflags="-I. $copt $cdiag $cfmt $cdbg $cxtra"

asmcflags='-fverbose-asm -frandon-seed=0'

map=0
docfg=1
vrb=0
bldtst=0
quick=0
verify=0
target=''
objs=''

verbose()
{
  if [ $vrb -eq 0 ]; then
    echo $1
  else
    echo $2
  fi
}

cc()
{
  local src
  local tgt

  tgt="$1"
  src="$2"

  verbose "$cc -c $src" "$cc -c $cflags $src"
  $cc -c $cflags -DDate=$date -DTime=1$time $src
  if [ "$tgt" = "$target" ]; then
    exit 0
  fi
}

ld()
{
  local tgt
  local obj

  tgt="$1"
  obj="$2"

  verbose "ld -o $tgt $obj" "$cc -o $tgt $lflags $obj"
  $cc $lflags -o "$tgt" "$lflags" $obj $libs
  if [ $map -eq 1 ]; then
    nm --line-numbers -S -r --size-sort $tgt > $tgt.map
  fi

  if [ "$tgt" = "$target" ]; then
    exit 0
  fi
}

while [ $# -ge 1 ]; do
  case "$1" in
  '-a') cflags="$cflags $cana" ;;
  '-b') cflags="$cflags -DBacktrace" ;;
  '-d') cflags="$cflags -DYal_dev" ;;
  '-h'|'-?') usage ;;
  '-m') map=1 ;;
  '-o') osinc=0; printinc=0 ;;
  '-q') quick=1; docfg=0; ;;
  '-Q') quick=2; docfg=0; ;;
  '-t') bldtst=2 ;;
  '-T') bldtst=1 ;;
  '-v') vrb=1 ;;
  '-V') verify=1; bldtst=2 ;;
  '-i') osinc=1 ;;
  *) target="$1" ;;
  esac
  shift
done

if [ $osinc -eq 0 ]; then
  objs="$objs os.o"
  if [ $quick -eq 0 ]; then
    cc os.o os.c
  fi
fi
if [ $printinc -eq 0 ]; then
  objs="$objs printf.o"
  if [ $quick -eq 0 ]; then
    cc printf.o printf.c
  fi
fi

if [ $docfg -eq 1 ]; then
  cc configure.o configure.c
  ld  configure  "configure.o $objs"
  verbose './configure' './configure'
  if ./configure "layout.h"; then
    echo "configure returned OK"
  else
    error "configure returned error code $?"
  fi
fi

cc yalloc.o yalloc.c

if [ $bldtst -ge 2 ]; then
  cc printf.o printf.c
  cc test.o test.c
#  cc yaldum.o yaldum.c
fi

if [ $bldtst -ge 1 ]; then
  ld test "test.o yalloc.o os.o printf.o"
  # ld test_libc "test.o yaldum.o $objs"
fi

if [ $quick -eq 2 ]; then
  exit 0
fi

platform=''
cmd="$(which uname)"
if [ -n "$cmd" ]; then
  platform="$($cmd)"
  if [ "$platform" = "Darwin" ]; then
    verbose "ld -dyn yalloc" "$cc $lflags -dynamiclib -o yalloc.dylib yalloc.o $objs -flat_namespace"
    $cc -dynamiclib $lflags -o yalloc.dylib yalloc.o $objs -flat_namespace
  elif [ "$platform" = "Linux" -o "$platform" = "Haiku" -o "$platform" = "FreeBSD"  ]; then
    verbose "ld -dyn yalloc" "$cc $lflags -shared -o yalloc.so yalloc.o $objs"
    $cc -g $lflags -shared -o yalloc.so yalloc.o $objs
    echo 'yalloc.so built successfully'
  fi
fi

if [ $verify -eq 0 ]; then
  exit 0
fi

# aligned_alloc
verbose 'test align small' 'test align alloc 4k 32"'
if ./test -s A 1 4096 32; then
  echo "test 1 ok"
else
  error "test 1 failed"
fi

verbose 'test align large' 'test align alloc 64k 32"'
if ./test -s A 0x10000 0x10001 32; then
  echo "test 1 ok"
else
  error "test 1 failed"
fi

# slab
verbose 'test slab' 'test slab 1 .. 4k * 3 iters 2"'
if ./test -s s 1 0x1000 3 2; then
  echo "test 2 ok"
else
  error "test 2 failed"
fi

# allfree
verbose 'test alloc-free' 'test alloc-free"'
if ./test -s a 1 100 32 1000; then
  echo "test 3 ok"
else
  error "test 3 failed"
fi

# realloc
verbose 'test realloc' 'test realloc"'
if ./test -s R 1 0x20000 10000; then
  echo "test 4 ok"
else
  error "test 4 failed"
fi
