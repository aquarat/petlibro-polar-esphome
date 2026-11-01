#!/bin/bash
# print decompiled function(s) whose header matches a name or address regex
awk -v pat="$1" '/^\/\/ ===== /{p = ($0 ~ pat)} p' "$(dirname "$0")/../work/out/decomp_all.c"
