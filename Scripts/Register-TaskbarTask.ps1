# Registers, or removes, the logon task that starts the Fluid Glass taskbar host (pillhost.exe).
# Run by the installer as an administrator. The task is for the Users group, so it starts the host for whoever
# signs in, with normal rights (explorer could not post to an elevated window) and at normal priority.
# Task Scheduler's default priority is below normal, which made the host start late and the old taskbar show first.
param(
	[string]$HostPath,
	[switch]$Remove,
	[switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$name = 'FluidGlassTaskbar'
# The task's name from before Open Fluid Glass was renamed. The new task replaces it.
if (-not $DryRun) { Unregister-ScheduledTask -TaskName 'LiquidGlassTaskbar' -Confirm:$false -ErrorAction SilentlyContinue }

if ($Remove) {
	Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
	exit 0
}
if (-not $HostPath -or -not (Test-Path -LiteralPath $HostPath)) { throw "pillhost.exe was not found: $HostPath" }

$action    = New-ScheduledTaskAction -Execute $HostPath
$trigger   = New-ScheduledTaskTrigger -AtLogOn
$principal = New-ScheduledTaskPrincipal -GroupId 'S-1-5-32-545' -RunLevel Limited
$settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
	-ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -Priority 4
$task = New-ScheduledTask -Action $action -Trigger $trigger -Principal $principal -Settings $settings

if ($DryRun) {
	$task.Principal | Format-List GroupId, LogonType, RunLevel
	$task.Settings | Format-List Priority, ExecutionTimeLimit, MultipleInstances, DisallowStartIfOnBatteries
	$task.Triggers | Format-List CimClass, UserId
	exit 0
}
Register-ScheduledTask -TaskName $name -InputObject $task -Force | Out-Null
