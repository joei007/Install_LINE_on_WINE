#!/bin/bash
set -e
x86_64-w64-mingw32-gcc -shared -O2 -o line_shadow_hook.dll \
    line_shadow_hook.c ../minhook/src/buffer.c ../minhook/src/hook.c \
    ../minhook/src/trampoline.c ../minhook/src/hde/hde64.c \
    -I../minhook/include -I../minhook/src \
    -static-libgcc \
    -Wl,--out-implib,line_shadow_hook.lib \
    -luser32 -lkernel32
x86_64-w64-mingw32-gcc -O2 -o injector.exe injector.c -luser32
echo "Built line_shadow_hook.dll and injector.exe"
