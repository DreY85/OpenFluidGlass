@echo off
rem Builds the Fluid Glass taskbar into Build\taskbar: pilltap.dll (loaded into explorer), starttap.dll (loaded into Start and Search), itemglass.dll (loaded into explorer) and pillhost.exe.
setlocal
rem "build.cmd logs" writes the diagnostic logs to %TEMP%. Released builds leave them out.
set logs=
if /i "%~1"=="logs" set logs=/DFLUIDGLASS_LOGS
rem Visual Studio or its Build Tools, with the C++ tools, found by vswhere. A shell that already has cl skips this.
where cl >nul 2>nul && goto tools
set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set vs=
if exist "%vswhere%" for /f "usebackq delims=" %%p in (`"%vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "vs=%%p"
if not defined vs (
	echo Visual Studio with the C++ tools was not found. Run this from a Developer Command Prompt.
	exit /b 1
)
call "%vs%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
:tools
set out=%~dp0..\Build\taskbar
if not exist "%out%\obj" mkdir "%out%\obj"
rem A log build leaves logs.flag next to the modules. Only the dev setup accepts them.
if defined logs type nul > "%out%\logs.flag"
cd /d "%~dp0"
cl /nologo %logs% /std:c++20 /EHsc /O2 /W3 /LD pilltap.cpp /Fe"%out%\pilltap.dll" /Fo"%out%\obj\\" /link /EXPORT:DllGetClassObject,PRIVATE /EXPORT:DllCanUnloadNow,PRIVATE oleaut32.lib user32.lib ole32.lib advapi32.lib windowsapp.lib || exit /b 1
cl /nologo %logs% /std:c++20 /EHsc /O2 /W3 /LD starttap.cpp /Fe"%out%\starttap.dll" /Fo"%out%\obj\\" /link /EXPORT:DllGetClassObject,PRIVATE /EXPORT:DllCanUnloadNow,PRIVATE oleaut32.lib user32.lib ole32.lib windowsapp.lib || exit /b 1
cl /nologo %logs% /std:c++20 /EHsc /O2 /W3 /LD itemglass.cpp /I"%~dp0..\vcpkg_installed\x64-windows-static\include" /Fe"%out%\itemglass.dll" /Fo"%out%\obj\\" /link /EXPORT:DllGetClassObject,PRIVATE /EXPORT:DllCanUnloadNow,PRIVATE /LIBPATH:"%~dp0..\vcpkg_installed\x64-windows-static\lib" KNSoft.SlimDetours.lib ntdll.lib uxtheme.lib user32.lib gdi32.lib msimg32.lib dwmapi.lib advapi32.lib uuid.lib ole32.lib || exit /b 1
rc /nologo /fo"%out%\obj\pillhost.res" pillhost.rc || exit /b 1
cl /nologo %logs% /std:c++20 /EHsc /O2 /W3 pillhost.cpp "%out%\obj\pillhost.res" /Fe"%out%\pillhost.exe" /Fo"%out%\obj\\" user32.lib gdi32.lib advapi32.lib dwmapi.lib winmm.lib /link /SUBSYSTEM:WINDOWS || exit /b 1
if not defined logs if exist "%out%\logs.flag" del "%out%\logs.flag"
