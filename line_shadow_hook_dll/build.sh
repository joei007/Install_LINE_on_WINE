#!/bin/bash
# Rebuild line_shadow_hook.dll from source (needs mingw-w64 on Linux,
# or MSVC/MinGW on Windows with matching flags).
set -e
x86_64-w64-mingw32-gcc -shared -O2 -o line_shadow_hook.dll \
    line_shadow_hook.c minhook/src/buffer.c minhook/src/hook.c \
    minhook/src/trampoline.c minhook/src/hde/hde64.c \
    -Iminhook/include -Iminhook/src \
    -static-libgcc \
    -Wl,--out-implib,line_shadow_hook.lib \
    -luser32 -lkernel32
echo "Built line_shadow_hook.dll"
