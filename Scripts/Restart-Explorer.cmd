@echo off
rem Restarts the signed-in user's Explorer so ExplorerBlurMica and the taskbar apply everywhere.
rem Windows may start the shell again by itself (AutoRestartShell), and starting it here as well would open an extra,
rem empty File Explorer window. So explorer.exe is started only when it has not come back after a second; in testing
rem Windows never brought it back, so a longer wait only kept the desktop empty for longer.
rem ping waits (one second between its two echoes) instead of timeout, which refuses to run without a console input.
taskkill /F /IM explorer.exe /FI "USERNAME eq %USERDOMAIN%\%USERNAME%" >nul 2>nul
ping -n 2 127.0.0.1 >nul
tasklist /FI "IMAGENAME eq explorer.exe" /FI "USERNAME eq %USERDOMAIN%\%USERNAME%" | find /I "explorer.exe" >nul || start "" explorer.exe
