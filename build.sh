#!/bin/bash

# build.sh - build script for yalloc

#   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

#   SPDX-FileCopyrightText: © 2024 Joris van der Geer
#   SPDX-License-Identifier: GPL-3.0-or-later

set -f
set -eu

tool=guess
dbg=0

usage()
{
  echo 'usage: build [options] [target]'
  echo
  echo '-a  - analyze'
  echo '-q  - quick - build yalloc.o only'
  echo '-t   - also build test'
  echo '-m  - create map file'
  echo '-v  - verbose'
  echo 'V - verify'
  echo '-h  - help'
}

error()
{
  echo $1
  exit 1
}

if [ $tool = 'guess' ]; then
  if which gcc; then tool=gcc
  elif which clang; then tool=clang
  elif which icx; then tool=icx
  elif which dmd; then tool=dmd
  elif which cc; then tool=cc
  else
    error 'unable to guess compiler'
  fi
fi

date=$(date -u '+%Y%m%d')
time=$(date -u '+%H%M')

case $tool in
  'clang' | 'icx')
  cc=clang
  cdiag='-Wall -Wextra -Wunused -Wsign-conversion -Wchar-subscripts -Werror=format -Werror=return-type -Wno-c2x-compat -Wno-poison-system-directories'
  cfmt='-fno-caret-diagnostics -fno-color-diagnostics -fno-diagnostics-show-option -fno-diagnostics-fixit-info -fno-diagnostics-show-note-include-stack -fno-show-column'
  cxtra='-std=c11 -funsigned-char -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -fpic -ftls-model=local-dynamic -fno-plt'
  cana="--analyze"
  if [ $dbg -eq 1 ]; then
    cdbg='-g -fsanitize=undefined,signed-integer-overflow,bounds -fno-sanitize-recover=all -ftrapv -fstack-protector -D_FORTIFY_SOURCE=3'
    UBSAN_OPTIONS=print_stacktrace=1
    libs=
  else
    cdbg='-gline-tables-only -fno-stack-protector -fwrapv -fcf-protection=none -mbranch-protection=none -fno-asynchronous-unwind-tables -D_FORTIFY_SOURCE=0' # -fno-stack-clash-protection
    libs=
  fi
  copt='-O2 -march=native'
  lflags="-O2 -g $cdbg -static"
  ;;

  'gcc')
  cc=gcc
  cdiag='-Wall -Wextra -Wshadow -Wundef -Wunused -Wformat-overflow=2 -Wformat-truncation=2 -Wpadded -Winline -Werror=stack-usage=35000'
  cfmt='-fmax-errors=60 -fno-diagnostics-show-caret -fno-diagnostics-show-option -fno-diagnostics-color -fcompare-debug-second'
  cxtra='-std=c11 -funsigned-char -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -fpic -ftls-model=local-dynamic -fno-plt'
  cana="-fanalyzer"
  if [ $dbg -eq 1 ]; then
    cdbg='-g -fsanitize=address,undefined,signed-integer-overflow,bounds -fno-sanitize-recover=all -ftrapv -fstack-protector'
    UBSAN_OPTIONS=print_stacktrace=1
  else
    cdbg='-g -fno-stack-protector -fcf-protection=none -fno-stack-clash-protection -fno-asynchronous-unwind-tables -D_FORTIFY_SOURCE=0'
  fi
  copt='-O2 -march=native -fwrapv -fgcse-after-reload -ftree-partial-pre -fsplit-paths'
  lflags="-O2 -fuse-ld=gold $cdbg -static"
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
osinc=0
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
  $cc -o "$tgt" "$lflags" $obj $libs
  if [ $map -eq 1 ]; then
    nm --line-numbers -S -r --size-sort $tgt > $tgt.map
  fi

  if [ "$tgt" = "$target" ]; then
    exit 0
  fi
}

while [ $# -ge 1 ]; do
  case "$1" in
  '-a') cflag+="$cana" ;;
  '-b') cflags+=" -DBacktrace" ;;
  '-h'|'-?') usage ;;
  '-m') map=1 ;;
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

if [ $osinc -eq 1 ]; then
  cflags="$cflags -DInc_os=1"
  objs='printf.o'
else
  objs='printf.o os.o'
fi

if [ $quick -eq 0 ]; then
  cc printf.o printf.c
  cc os.o os.c
fi

if [ $docfg -eq 1 ]; then
  cc configure.o configure.c
  ld  configure  "configure.o $objs"
  verbose './configure' './configure'
  if ./configure "layout.h"; then
    echo "configure returned OK"
  else
    error "configure returned error code $?"
    exit 1
  fi
fi

cc yalloc.o yalloc.c

if [ $bldtst -ge 2 ]; then
  cc test.o test.c
#  cc yaldum.o yaldum.c
fi

if [ $bldtst -ge 1 ]; then
  ld test "test.o yalloc.o $objs"
#  ld test_libc "test.o yaldum.o $objs"
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
    $cc -dynamiclib -o yalloc.dylib yalloc.o $objs -flat_namespace
  elif [ "$platform" = "Linux" ]; then
    verbose "ld -dyn yalloc" "$cc $lflags -shared -o yalloc.so yalloc.o $objs"
    $cc -shared -o yalloc.so yalloc.o $objs
  fi
fi

if [ $verify -eq 0 ]; then
  exit 0
fi

# aligned_alloc
verbose 'test align small' 'test align alloc 4k 32"'
./test -s A 1 4096 32

verbose 'test align large' 'test align alloc 64k 32"'
./test -s A 0x10000 0x10000 32

# slab
verbose 'test slab' 'test slab 64k 3 10"'
./test -s s 1 0x10000 3 10

# allfree
verbose 'test alloc-free' 'test alloc-free"'
./test -s a 1 100 32 1000

