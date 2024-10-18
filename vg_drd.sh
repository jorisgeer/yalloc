#!/bin/sh

set -ex

valgrind --tool=drd --show-below-main=yes --suppressions=vg.sup --exit-on-first-error=no --first-race-only=yes --read-var-info=yes --fair-sched=try --segment-merging=no --show-stack-usage=yes --soname-synonyms=somalloc=nouserintercept "$@"
