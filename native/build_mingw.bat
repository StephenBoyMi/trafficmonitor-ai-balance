@echo off
setlocal
set GXX=D:\Tools\w64devkit\w64devkit\bin\g++.exe
set RC=D:\Tools\w64devkit\w64devkit\bin\windres.exe
if not exist "%GXX%" set GXX=g++
if not exist "%RC%" set RC=windres

"%RC%" AIBalanceNative.rc -O coff -o AIBalanceNative.res.o
if errorlevel 1 exit /b 1
"%GXX%" -std=c++17 -O2 -shared -static-libgcc -static-libstdc++ -o AIBalanceNative.dll AIBalanceNative.cpp OptionsDialog.cpp Json.cpp Http.cpp AIBalanceNative.res.o -lwinhttp -lcrypt32 -lshell32 -luser32 -lgdi32 -lole32 -lcomctl32
if errorlevel 1 exit /b 1
echo Built AIBalanceNative.dll
