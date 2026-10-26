# Probe only: restarts the Start and Search processes (Windows starts them again), loads starttap.dll into both,
# then runs startglass.exe for five minutes. Killing those processes again undoes the TAP.
$build = Join-Path $PSScriptRoot '..\Build\start'
Stop-Process -Name startglass -Force -ErrorAction SilentlyContinue
Remove-Item "$env:TEMP\starttap.txt" -ErrorAction SilentlyContinue
foreach ($exe in 'StartMenuExperienceHost', 'SearchHost') {
	Stop-Process -Name $exe -Force -ErrorAction SilentlyContinue
	$deadline = (Get-Date).AddSeconds(20)
	do { Start-Sleep -Milliseconds 500; $p = Get-Process $exe -ErrorAction SilentlyContinue } until ($p -or (Get-Date) -gt $deadline)
	if (-not $p) { "$exe did not come back by itself: open it once, then run this again."; continue }
	Start-Sleep -Seconds 2
	& (Join-Path $build 'startinject.exe') "$exe.exe"
}
Start-Process (Join-Path $build 'startglass.exe')
'startglass runs for five minutes. Open and close Start and Search a few times.'
