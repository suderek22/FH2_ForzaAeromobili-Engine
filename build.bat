@echo off
rem Build ForzaAeromobiliEngine.dll with MSVC. Set FH2MOD_INCLUDE to the FH2 Mod Platform "include" folder
rem (fh2mod.h, fh2game.h) if it is not at the default path below.
if "%FH2MOD_INCLUDE%"=="" set FH2MOD_INCLUDE=E:\forza2port_win\project_data\Forza\FH2-Mod-Platform-v2.8.0\include
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %~dp0
if not exist build mkdir build
cl /nologo /LD /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /I"%FH2MOD_INCLUDE%" src\ForzaAeromobiliEngine.cpp /Fe:ForzaAeromobiliEngine.dll /Fo:build\ user32.lib
