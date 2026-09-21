#!/bin/bash
set -e
x86_64-w64-mingw32-gcc -shared -O2 -o dwmapi.dll \
    dwmapi_proxy.c dwmapi_proxy.def \
    ../minhook/src/buffer.c ../minhook/src/hook.c \
    ../minhook/src/trampoline.c ../minhook/src/hde/hde64.c \
    -I../minhook/include -I../minhook/src \
    -static-libgcc \
    -Wl,--out-implib,dwmapi.lib \
    -luser32 -lkernel32
echo "Built dwmapi.dll (run from inside dwmapi_proxy_method/, needs ../minhook/)"
