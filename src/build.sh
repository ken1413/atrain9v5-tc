#!/bin/bash
# 建散布版 DLL。需要 mingw-w64（Debian/Ubuntu: apt install gcc-mingw-w64-x86-64）
set -e
cd "$(dirname "$0")/.."
x86_64-w64-mingw32-gcc -shared -O2 -Wall -static -static-libgcc \
    -o dinput8.dll a9tc_dist.c -Wl,--kill-at -lgdi32 -luser32
echo "--- 相依 DLL（應只有 KERNEL32 / msvcrt / GDI32 / USER32）---"
x86_64-w64-mingw32-objdump -p dinput8.dll | grep -i "DLL Name"
ls -l dinput8.dll
