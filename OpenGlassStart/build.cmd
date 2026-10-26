@echo off
rem Builds the Start menu probe into Build\start: starttap.dll (loaded into StartMenuExperienceHost).
setlocal
call D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat >nul 2>nul || exit /b 1
set out=%~dp0..\Build\start
if not exist "%out%\obj" mkdir "%out%\obj"
cd /d "%~dp0"
cl /nologo /std:c++20 /EHsc /O2 /W3 /LD starttap.cpp /Fe"%out%\starttap.dll" /Fo"%out%\obj\\" /link /EXPORT:DllGetClassObject,PRIVATE /EXPORT:DllCanUnloadNow,PRIVATE oleaut32.lib user32.lib ole32.lib windowsapp.lib || exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W3 startinject.cpp /Fe"%out%\startinject.exe" /Fo"%out%\obj\\" advapi32.lib || exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W3 startglass.cpp /Fe"%out%\startglass.exe" /Fo"%out%\obj\\" user32.lib dwmapi.lib advapi32.lib /link /SUBSYSTEM:WINDOWS || exit /b 1
