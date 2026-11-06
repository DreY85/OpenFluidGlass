# Run by the uninstaller as an administrator. Restarts File Explorer for the signed-in users, with normal rights, so
# the glass effects it still has loaded go away now instead of at the next sign-in.
# An Explorer started from the elevated uninstaller would run elevated too. So a one-off task for the Users group
# runs the same steps as Restart-Explorer.cmd in each user's own session, and is removed once it has run.
param([switch]$DryRun)
$ErrorActionPreference = 'Stop'
$name = 'FluidGlassRestartExplorer'
$sys = Join-Path $env:SystemRoot 'System32'
$steps = 'taskkill /F /IM explorer.exe /FI "USERNAME eq %USERDOMAIN%\%USERNAME%" >nul 2>nul' +
	' & ping -n 2 127.0.0.1 >nul' +
	' & tasklist /FI "IMAGENAME eq explorer.exe" /FI "USERNAME eq %USERDOMAIN%\%USERNAME%" | find /I "explorer.exe" >nul || start "" explorer.exe'
# conhost --headless keeps a console window from showing while the steps run.
$action    = New-ScheduledTaskAction -Execute (Join-Path $sys 'conhost.exe') -Argument "--headless $sys\cmd.exe /c $steps"
$principal = New-ScheduledTaskPrincipal -GroupId 'S-1-5-32-545' -RunLevel Limited
$settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
	-ExecutionTimeLimit (New-TimeSpan -Minutes 1) -MultipleInstances Parallel
$task = New-ScheduledTask -Action $action -Principal $principal -Settings $settings

if ($DryRun) {
	$task.Actions | Format-List Execute, Arguments
	$task.Principal | Format-List GroupId, RunLevel
	exit 0
}
Register-ScheduledTask -TaskName $name -InputObject $task -Force | Out-Null
try {
	Start-ScheduledTask -TaskName $name
	# Explorer is back about a second after it closes. Wait for that, at most 15 seconds, before removing the task.
	Start-Sleep -Milliseconds 500
	for ($i = 0; $i -lt 30 -and (Get-ScheduledTask -TaskName $name).State -eq 'Running'; $i++) {
		Start-Sleep -Milliseconds 500
	}
	$result = (Get-ScheduledTaskInfo -TaskName $name).LastTaskResult
} finally {
	Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
}
# 0 when the steps ran. 267011 means the task never started, so Explorer keeps the effects until the next sign-in.
exit $result
