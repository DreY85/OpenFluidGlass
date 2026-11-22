# Registers, or removes, the task that checks GitHub for a newer Open Fluid Glass release.
# Run by the installer as an administrator. The task is for the Users group, so it runs for whoever is signed in,
# with normal rights. It asks api.github.com for the latest release tag and shows a notice when that is newer.
# Nothing is downloaded or installed. Each user can turn the check off in Fluid Glass (System tab, Updates).
param(
	[string]$GuiPath,
	[switch]$Remove,
	[switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$name = 'FluidGlassUpdateCheck'

if ($Remove) {
	Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
	exit 0
}
if (-not $GuiPath -or -not (Test-Path -LiteralPath $GuiPath)) { throw "OpenGlassGUI.exe was not found: $GuiPath" }

$action = New-ScheduledTaskAction -Execute $GuiPath -Argument '--check-updates'
# A short delay keeps the check out of the busy first moments after sign-in.
$logon = New-ScheduledTaskTrigger -AtLogOn
$logon.Delay = 'PT2M'
$daily = New-ScheduledTaskTrigger -Daily -At '12:00' -RandomDelay (New-TimeSpan -Minutes 30)
$principal = New-ScheduledTaskPrincipal -GroupId 'S-1-5-32-545' -RunLevel Limited
# A missed daily run starts when the computer is next on. The check needs the network and stops after 5 minutes at most.
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -RunOnlyIfNetworkAvailable -AllowStartIfOnBatteries `
	-DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 5) -MultipleInstances IgnoreNew
$task = New-ScheduledTask -Action $action -Trigger @($logon, $daily) -Principal $principal -Settings $settings

if ($DryRun) {
	$task.Actions | Format-List Execute, Arguments
	$task.Principal | Format-List GroupId, LogonType, RunLevel
	$task.Settings | Format-List StartWhenAvailable, RunOnlyIfNetworkAvailable, ExecutionTimeLimit, MultipleInstances, DisallowStartIfOnBatteries
	$task.Triggers | Format-List CimClass, Delay, StartBoundary, RandomDelay
	exit 0
}
Register-ScheduledTask -TaskName $name -InputObject $task -Force | Out-Null
