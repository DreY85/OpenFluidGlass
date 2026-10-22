@echo off
rem Restarts the signed-in user's Explorer so ExplorerBlurMica and the taskbar apply everywhere.
rem Windows starts the shell again by itself (AutoRestartShell), so explorer.exe is started here only when it has
rem not come back after a few seconds. Starting it at once as well opened an extra, empty File Explorer window.
rem ping waits instead of timeout, which refuses to run without a console input.
taskkill /F /IM explorer.exe /FI "USERNAME eq %USERDOMAIN%\%USERNAME%" >nul 2>nul
ping -n 6 127.0.0.1 >nul
tasklist /FI "IMAGENAME eq explorer.exe" /FI "USERNAME eq %USERDOMAIN%\%USERNAME%" | find /I "explorer.exe" >nul || start "" explorer.exe
