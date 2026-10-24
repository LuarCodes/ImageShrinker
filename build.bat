@echo off
rem Build ImageShrinker.exe with MinGW-w64 (MSYS2: pacman -S mingw-w64-ucrt-x86_64-gcc)
gcc -O2 -c third\zlib\adler32.c third\zlib\crc32.c third\zlib\deflate.c third\zlib\trees.c third\zlib\zutil.c third\zlib\compress.c
cd shrinker
echo 1 ICON "app.ico" > app.rc
echo 1 24 "../common/app.manifest" >> app.rc
windres app.rc -O coff -o app.res.o
g++ -O2 -std=c++17 -municode -mwindows -static -s main.cpp app.res.o ..\adler32.o ..\crc32.o ..\deflate.o ..\trees.o ..\zutil.o ..\compress.o -ldwmapi -luxtheme -lcomctl32 -lcomdlg32 -lgdi32 -lshell32 -o ImageShrinker.exe
cd ..
echo Built shrinker\ImageShrinker.exe
