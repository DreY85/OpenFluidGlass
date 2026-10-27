@echo off
rem Builds the Fluid Glass taskbar into Build\taskbar: pilltap.dll (loaded into explorer), starttap.dll (loaded into Start and Search) and pillhost.exe.
setlocal
call D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat >nul 2>nul || exit /b 1
set out=%~dp0..\Build\taskbar
if not exist "%out%\obj" mkdir "%out%\obj"
cd /d "%~dp0"
cl /nologo /std:c++20 /EHsc /O2 /W3 /LD pilltap.cpp /Fe"%out%\pilltap.dll" /Fo"%out%\obj\\" /link /EXPORT:DllGetClassObject,PRIVATE /EXPORT:DllCanUnloadNow,PRIVATE oleaut32.lib user32.lib ole32.lib advapi32.lib windowsapp.lib || exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W3 /LD starttap.cpp /Fe"%out%\starttap.dll" /Fo"%out%\obj\\" /link /EXPORT:DllGetClassObject,PRIVATE /EXPORT:DllCanUnloadNow,PRIVATE oleaut32.lib user32.lib ole32.lib windowsapp.lib || exit /b 1
rc /nologo /fo"%out%\obj\pillhost.res" pillhost.rc || exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W3 pillhost.cpp "%out%\obj\pillhost.res" /Fe"%out%\pillhost.exe" /Fo"%out%\obj\\" user32.lib gdi32.lib advapi32.lib dwmapi.lib /link /SUBSYSTEM:WINDOWS || exit /b 1
