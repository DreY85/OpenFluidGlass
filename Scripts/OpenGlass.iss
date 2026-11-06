; Keep the product identity in one place. The outer doubled braces preserve
; the literal GUID braces after the preprocessor expands MyAppId.
#define MyAppId "{923303B0-A0A8-4374-9F46-F2A657BFBFAE}"
; OpenGlass, which this fork started from, installs under this one, and so did the first Fluid Glass builds.
#define UpstreamAppId "{D3D1BC7D-5E24-4B33-9383-7934271A3B05}"
#define MyAppName "Open Fluid Glass"
#ifndef MyAppVersion
#error MyAppVersion must be supplied by the packaging target
#endif
#define MyAppPublisher "Open Fluid Glass (based on OpenGlass by ALTaleX)"
#define MyAppExeName "OpenGlassGUI.exe"
#ifndef SetupName
#define SetupName "OpenFluidGlassSetup"
#endif
; The dev setup carries the taskbar modules built with logs. It installs over the public one, same AppId.
#ifdef DevBuild
#define DevSuffix "-dev"
#else
#define DevSuffix ""
#endif
#define MyAppSetupName SetupName + DevSuffix
#define MyProjPath ".."
#ifndef BuildPath
#define BuildPath MyProjPath + "\Build\x64\Release"
#endif
#define MyAppBuildPath BuildPath
#ifndef OutputPath
#define OutputPath MyAppBuildPath
#endif
#define MyAppOutputPath OutputPath

; The taskbar and ExplorerBlurMica are not MSBuild projects. OpenGlassTaskbar\build.cmd builds the first,
; ThirdParty\ExplorerBlurMica holds the second (unmodified, with its notice).
#define TaskbarPath MyProjPath + "\Build\taskbar"
#define EbmPath MyProjPath + "\ThirdParty\ExplorerBlurMica"
#if !FileExists(SourcePath + "\" + TaskbarPath + "\pillhost.exe") || !FileExists(SourcePath + "\" + TaskbarPath + "\pilltap.dll") || !FileExists(SourcePath + "\" + TaskbarPath + "\starttap.dll") || !FileExists(SourcePath + "\" + TaskbarPath + "\itemglass.dll")
#error The taskbar is not built. Run OpenGlassTaskbar\build.cmd first.
#endif
#if FileExists(SourcePath + "\" + TaskbarPath + "\logs.flag") && !Defined(DevBuild)
#error The taskbar was built with logs. Run OpenGlassTaskbar\build.cmd without logs, or build the dev setup.
#endif
#if !FileExists(SourcePath + "\" + EbmPath + "\ExplorerBlurMica.dll")
#error ThirdParty\ExplorerBlurMica\ExplorerBlurMica.dll is missing.
#endif

[Setup]
AppId={{#MyAppId}}
AppName={#MyAppName}
VersionInfoVersion={#MyAppVersion}
AppVersion={#MyAppVersion}{#DevSuffix}
AppVerName={#MyAppName} v{#MyAppVersion}{#DevSuffix}
AppPublisher={#MyAppPublisher}
AppendDefaultDirName=yes
DefaultDirName={commonpf}\{#MyAppName}
DisableDirPage=auto
DirExistsWarning=auto
AlwaysShowDirOnReadyPage=yes
UsePreviousAppDir=yes
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=auto
UsePreviousGroup=yes
AllowNoIcons=yes
LicenseFile={#MyProjPath}\LICENSE.txt
OutputDir={#MyAppOutputPath}
OutputBaseFilename={#MyAppSetupName}
Compression=lzma
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Mandatory administrative privileges
PrivilegesRequired=admin
; Support Win10 1809 (Build 17763) and newer
MinVersion=10.0.17763
SetupLogging=yes
CloseApplications=yes
RestartApplications=no
CloseApplicationsFilter=OpenGlassGUI.exe,OpenGlassHost.exe,pillhost.exe
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupIconFile={#MyProjPath}\OpenGlassGUI\Assets\OpenGlass.ico
UninstallDisplayName=Open Fluid Glass v{#MyAppVersion}{#DevSuffix}
Uninstallable=yes
UpdateUninstallLogAppName=yes
UsePreviousLanguage=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
LaunchOpenGlassGUI=Launch Fluid Glass
UninstallOptionsTitle=Fluid Glass uninstall options
UninstallOptionsDescription=Choose the Fluid Glass data to remove.
UninstallDeleteCurrentAndMachineConfig=Delete current-user and machine configuration and its resources
OtherUserConfigNotice=Settings in other Windows user profiles are preserved and must be removed manually while signed in as that user.
UninstallDeletePresetPackages=Delete the preset library and retained legacy packages (keep current resources)
ContinueUninstall=Uninstall
ServiceDescription=This service injects DLL into DWM for you and also maintains that user settings are correctly loaded.
RestartExplorer=Restart Windows Explorer now to apply the glass effects (the screen flickers for a moment)
StartTaskbar=Start the Fluid Glass taskbar now
InstallingTaskbar=Registering the Fluid Glass taskbar...
RemovingTaskbar=Removing the Fluid Glass taskbar...
UpstreamInstalled=OpenGlass, or an early Open Fluid Glass build, is already installed on this PC. The two cannot run together. Uninstall it from Settings > Apps, restart, then run this setup again. To keep your settings, leave the uninstaller's delete options unticked.
TaskbarTaskFailed=The Fluid Glass taskbar could not be set to start at sign-in. Everything else is installed. You can still start the taskbar from the Taskbar tab in Fluid Glass. The setup log has the details.

[Files]
Source: "{#MyAppBuildPath}\legacy\OpenGlass.dll"; DestDir: "{app}"; DestName: "OpenGlass.dll"; Flags: ignoreversion restartreplace; Check: ShouldInstallLegacyDll
Source: "{#MyAppBuildPath}\milcomp\OpenGlass.dll"; DestDir: "{app}"; DestName: "OpenGlass.dll"; Flags: ignoreversion restartreplace; Check: ShouldInstallMILCompDll
Source: "{#MyAppBuildPath}\OpenGlassHost.exe"; DestDir: "{app}"; Flags: ignoreversion restartreplace
Source: "{#MyAppBuildPath}\OpenGlassGUI.exe"; DestDir: "{app}"; Flags: ignoreversion restartreplace

; Fluid Glass taskbar. pillhost.exe starts at sign-in and has explorer load a copy of pilltap.dll and of itemglass.dll
; (File Explorer's hover and selection glass), and Start and Search a copy of starttap.dll.
; It works on the XAML taskbar of Windows 11 only, so Windows 10 gets neither the files nor the logon task.
Source: "{#TaskbarPath}\pillhost.exe"; DestDir: "{app}\taskbar"; Flags: ignoreversion restartreplace uninsrestartdelete; Check: IsWindows11
Source: "{#TaskbarPath}\pilltap.dll"; DestDir: "{app}\taskbar"; Flags: ignoreversion restartreplace uninsrestartdelete; Check: IsWindows11
Source: "{#TaskbarPath}\starttap.dll"; DestDir: "{app}\taskbar"; Flags: ignoreversion restartreplace uninsrestartdelete; Check: IsWindows11
; itemglass.dll and ExplorerBlurMica.dll are folder extensions, so every File Explorer window loads them from here.
; MoveAsideInUse renames the loaded copy first, so an update needs an Explorer restart, not a Windows restart.
Source: "{#TaskbarPath}\itemglass.dll"; DestDir: "{app}\taskbar"; Flags: ignoreversion restartreplace uninsrestartdelete; BeforeInstall: MoveAsideInUse; Check: IsWindows11
Source: "{#MyProjPath}\Scripts\Register-TaskbarTask.ps1"; DestDir: "{app}\taskbar"; Flags: ignoreversion
Source: "{#MyProjPath}\Scripts\Restart-Explorer.cmd"; DestDir: "{app}"; Flags: ignoreversion

; ExplorerBlurMica (LGPL-3.0), unmodified. Registered below; its settings file sits beside the DLL.
Source: "{#EbmPath}\ExplorerBlurMica.dll"; DestDir: "{app}\ExplorerBlurMica"; Flags: ignoreversion restartreplace uninsrestartdelete; BeforeInstall: MoveAsideInUse
Source: "{#EbmPath}\config.ini"; DestDir: "{app}\ExplorerBlurMica"; Flags: onlyifdoesntexist
Source: "{#EbmPath}\ReadMe.txt"; DestDir: "{app}\ExplorerBlurMica"; Flags: ignoreversion
Source: "{#EbmPath}\NOTICE.txt"; DestDir: "{app}\ExplorerBlurMica"; Flags: ignoreversion
Source: "{#EbmPath}\LICENSE-LGPL-3.0.txt"; DestDir: "{app}\ExplorerBlurMica"; Flags: ignoreversion
; The GPL v3 of Open Fluid Glass, which the LGPL above also builds on.
Source: "{#MyProjPath}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion

[INI]
; The look is set in the Fluid Glass tab, so ExplorerBlurMica only clears File Explorer's background (the GUI keeps
; this in line too). Its Mica effect (2) makes File Explorer glass match other windows, but Mica exists only on
; Windows 11; Windows 10 keeps its Blur effect (0).
Filename: "{app}\ExplorerBlurMica\config.ini"; Section: "config"; Key: "effect"; String: "2"; Check: IsWindows11
Filename: "{app}\ExplorerBlurMica\config.ini"; Section: "config"; Key: "effect"; String: "0"; Check: not IsWindows11

[Registry]
; What "regsvr32 ExplorerBlurMica.dll" registers: a folder extension that explorer loads into every File Explorer window.
; The item glass is also a folder extension, so Open and Save dialogs load it. It stays only when the user turns
; FluidGlassDialogs on in the settings, and explorer refuses the second copy beside the one pillhost loads.
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{31DCA04E-200A-45C4-8969-9FEA9673B3D9}"; ValueType: string; ValueName: ""; ValueData: "Open Fluid Glass item glass"; Flags: uninsdeletekey; Check: IsWindows11
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{31DCA04E-200A-45C4-8969-9FEA9673B3D9}\InProcServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\taskbar\itemglass.dll"; Flags: uninsdeletekey; Check: IsWindows11
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{31DCA04E-200A-45C4-8969-9FEA9673B3D9}\InProcServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"; Check: IsWindows11
Root: HKLM; Subkey: "SOFTWARE\Classes\Drive\shellex\FolderExtensions\{{31DCA04E-200A-45C4-8969-9FEA9673B3D9}"; ValueType: dword; ValueName: "DriveMask"; ValueData: "255"; Flags: uninsdeletekey; Check: IsWindows11
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{887D3A6A-502E-4AF5-9CE6-D515E12AFE89}"; ValueType: string; ValueName: ""; ValueData: "ExplorerBlurMica DLL"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{887D3A6A-502E-4AF5-9CE6-D515E12AFE89}\InProcServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\ExplorerBlurMica\ExplorerBlurMica.dll"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{887D3A6A-502E-4AF5-9CE6-D515E12AFE89}\InProcServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
Root: HKLM; Subkey: "SOFTWARE\Classes\Drive\shellex\FolderExtensions\{{887D3A6A-502E-4AF5-9CE6-D515E12AFE89}"; ValueType: dword; ValueName: "DriveMask"; ValueData: "255"; Flags: uninsdeletekey; Check: ShouldEnableExplorerBlurMica
; The standard look, as machine values: DWM and the GUI read HKCU first, so whatever a user sets wins.
; Existing machine values are kept, so an update never resets them.
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlass"; ValueData: "1"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassBezel"; ValueData: "10000"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassThickness"; ValueData: "4500"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassScale"; ValueData: "150"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassIor"; ValueData: "150"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassGap"; ValueData: "3500"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassRadius"; ValueData: "800"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassPillRadius"; ValueData: "1200"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassSoftness"; ValueData: "150"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassClarity"; ValueData: "10"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassChroma"; ValueData: "33"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassSaturation"; ValueData: "120"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassReadability"; ValueData: "35"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassReadabilityLight"; ValueData: "80"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassRim"; ValueData: "140"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassShine"; ValueData: "300"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassLight"; ValueData: "58500"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassProfile"; ValueData: "0"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "FluidGlassDialogs"; ValueData: "1"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "GlassOpacity"; ValueData: "31"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "ColorizationColorOverride"; ValueData: "$C4000000"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "ColorizationAfterglowOverride"; ValueData: "$C4000000"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "ColorizationColorBalanceOverride"; ValueData: "5"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "ColorizationAfterglowBalanceOverride"; ValueData: "32"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "ColorizationBlurBalanceOverride"; ValueData: "63"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "TextGlowMode"; ValueData: "3"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "CaptionTextBold"; ValueData: "1"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "GlassOverrideAccent"; ValueData: "1"; Flags: createvalueifdoesntexist
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\DWM"; ValueType: dword; ValueName: "UseDirect3DRendering"; ValueData: "1"; Flags: createvalueifdoesntexist

[Dirs]
; Mutable application data and immutable preset deployments live outside the installation directory.
Name: "{commonappdata}\OpenGlass"
Name: "{commonappdata}\OpenGlass\symbols"
; Full DWM dumps can contain sensitive process memory; do not grant ordinary users access.
Name: "{commonappdata}\OpenGlass\dumps"
Name: "{commonappdata}\OpenGlass\Presets"
Name: "{commonappdata}\OpenGlass\Configuration"

[UninstallDelete]
Type: filesandordirs; Name: "{commonappdata}\OpenGlass\symbols"
Type: filesandordirs; Name: "{commonappdata}\OpenGlass\dumps"
; Clean the defaults used by older installers without touching any user-selected custom path.
Type: filesandordirs; Name: "{app}\symbols"
Type: filesandordirs; Name: "{app}\dumps"
; Copies an update moved aside while File Explorer still had them loaded (see MoveAsideInUse).
Type: files; Name: "{app}\taskbar\itemglass.dll.old*"
Type: files; Name: "{app}\ExplorerBlurMica\ExplorerBlurMica.dll.old*"

[InstallDelete]
; Shortcuts left by an OpenGlass setup from before the rename; they point at the same GUI.
Type: files; Name: "{commondesktop}\Configure OpenGlass.lnk"
Type: files; Name: "{group}\Configure OpenGlass.lnk"
Type: files; Name: "{group}\Uninstall OpenGlass.lnk"
; And from before it was renamed Open Fluid Glass.
Type: files; Name: "{commondesktop}\Open Liquid Glass.lnk"
Type: files; Name: "{group}\Open Liquid Glass.lnk"
Type: files; Name: "{group}\Uninstall Open Liquid Glass.lnk"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkablealone

[Icons]
Name: "{group}\Open Fluid Glass"; Filename: "{app}\OpenGlassGUI.exe"
Name: "{commondesktop}\Open Fluid Glass"; Filename: "{app}\OpenGlassGUI.exe"; Tasks: desktopicon
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"

[Run]
; NOTE: Users can choose an arbitrary install path. If it is a world-writable directory (e.g. C:\OpenGlass),
; we must explicitly harden ACLs so the binary directory is not writable by non-admin users.
;
; SIDs used (language-independent):
;   *S-1-5-18      = LOCAL SYSTEM
;   *S-1-5-32-544  = Administrators
;   *S-1-5-32-545  = Users
;   *S-1-5-90-0    = Window Manager Group (DWM)
;
; Harden {app} (binaries): SYSTEM/Admins = Full, Users = RX, Window Manager = RX
Filename: "{sys}\icacls.exe"; Parameters: """{app}"" /inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F *S-1-5-32-545:(OI)(CI)RX *S-1-5-90-0:(OI)(CI)RX"; Flags: runhidden waituntilterminated
; Harden OpenGlassHost.exe (service binary): SYSTEM only for execution, but allow Administrators to delete (for uninstallation)
Filename: "{sys}\icacls.exe"; Parameters: """{app}\OpenGlassHost.exe"" /inheritance:r /grant:r *S-1-5-18:F *S-1-5-32-544:D"; Flags: runhidden waituntilterminated
; ExplorerBlurMica's settings file only: Users = Modify, so the OpenGlass GUI can change it without elevation.
; It holds visual settings alone; the DLL and its folder stay read-only for users.
Filename: "{sys}\icacls.exe"; Parameters: """{app}\ExplorerBlurMica\config.ini"" /grant *S-1-5-32-545:M"; Flags: runhidden waituntilterminated
; Harden the shared data root before applying narrower permissions to each child.
Filename: "{sys}\icacls.exe"; Parameters: """{commonappdata}\OpenGlass"" /inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F *S-1-5-32-545:(OI)(CI)RX *S-1-5-90-0:(OI)(CI)RX"; Flags: runhidden waituntilterminated
; Symbols: SYSTEM/Admins = Full, Users = Read, Window Manager = Read/Write/Delete.
Filename: "{sys}\icacls.exe"; Parameters: """{commonappdata}\OpenGlass\symbols"" /inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F *S-1-5-32-545:(OI)(CI)R *S-1-5-90-0:(OI)(CI)RWD"; Flags: runhidden waituntilterminated
; Dumps: SYSTEM/Admins = Full, Window Manager = Modify; no ordinary-user access to dump contents.
Filename: "{sys}\icacls.exe"; Parameters: """{commonappdata}\OpenGlass\dumps"" /inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F *S-1-5-90-0:(OI)(CI)M"; Flags: runhidden waituntilterminated
; Presets: SYSTEM/Admins = Full, Users/Window Manager = Read/Execute. Package directories retain the same protected ACL.
Filename: "{sys}\icacls.exe"; Parameters: """{commonappdata}\OpenGlass\Presets"" /inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F *S-1-5-32-545:(OI)(CI)RX *S-1-5-90-0:(OI)(CI)RX"; Flags: runhidden waituntilterminated
Filename: "{sys}\icacls.exe"; Parameters: """{commonappdata}\OpenGlass\Configuration"" /inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F *S-1-5-32-545:(OI)(CI)RX *S-1-5-90-0:(OI)(CI)RX"; Flags: runhidden waituntilterminated

; Create the service
Filename: "{sys}\sc.exe"; Parameters: "delete OpenGlassHost"; Flags: runhidden waituntilterminated
Filename: "{sys}\sc.exe"; Parameters: "create OpenGlassHost binPath= ""{app}\OpenGlassHost.exe"" displayName= ""OpenGlass Host Service"" start= auto type= own"; Flags: runhidden
; Set description
Filename: "{sys}\sc.exe"; Parameters: "description OpenGlassHost ""{cm:ServiceDescription}"""; Flags: runhidden
; Start the service (Automatic start, no checkbox)
Filename: "{sys}\sc.exe"; Parameters: "start OpenGlassHost"; Flags: runhidden nowait
; Logon task for the taskbar host (Users group, normal priority). The script runs from the hardened install folder.
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File ""{app}\taskbar\Register-TaskbarTask.ps1"" -HostPath ""{app}\taskbar\pillhost.exe"""; StatusMsg: "{cm:InstallingTaskbar}"; Flags: runhidden waituntilterminated; AfterInstall: CheckTaskbarTask; Check: IsWindows11
; Both of these start as the signed-in user, never elevated: explorer could not post to an elevated host.
Filename: "{app}\Restart-Explorer.cmd"; Description: "{cm:RestartExplorer}"; Flags: postinstall nowait runasoriginaluser runhidden skipifsilent
Filename: "{app}\taskbar\pillhost.exe"; Description: "{cm:StartTaskbar}"; Flags: postinstall nowait runasoriginaluser skipifsilent; Check: IsWindows11
; Start as the original user; the GUI performs its own authenticated elevation.
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchOpenGlassGUI}"; Flags: postinstall nowait runasoriginaluser; Check: not WizardSilent

[UninstallRun]
Filename: "{sys}\sc.exe"; Parameters: "stop OpenGlassHost"; Flags: runhidden waituntilterminated; RunOnceId: "StopService"
Filename: "{sys}\taskkill.exe"; Parameters: "/IM ""{#MyAppExeName}"" /F"; Flags: runhidden; RunOnceId: "KillGUI"
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File ""{app}\taskbar\Register-TaskbarTask.ps1"" -Remove"; StatusMsg: "{cm:RemovingTaskbar}"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveTaskbarTask"
Filename: "{sys}\taskkill.exe"; Parameters: "/IM pillhost.exe /F"; Flags: runhidden; RunOnceId: "KillTaskbarHost"
Filename: "{sys}\sc.exe"; Parameters: "delete OpenGlassHost"; Flags: runhidden; RunOnceId: "DeleteService"

[Code]

var
  InstallMILCompDll: Boolean;
  ExplorerBlurMicaTurnedOff: Boolean;
  WindowsBuild: Cardinal;

// Windows 11 starts at build 22000: the XAML taskbar the floating taskbar needs, and the Mica backdrop.
function IsWindows11(): Boolean;
begin
  Result := WindowsBuild >= 22000;
end;

// The GUI turns ExplorerBlurMica off by removing only its folder extension key and keeping the class.
// Read before anything is written, so an update keeps it off, and a fresh install (no class yet) turns it on.
function ShouldEnableExplorerBlurMica(): Boolean;
begin
  Result := not ExplorerBlurMicaTurnedOff;
end;

const
  MOVEFILE_DELAY_UNTIL_REBOOT = 4;

function MoveFileExNoTarget(lpExistingFileName: String; lpNewFileName: Cardinal; dwFlags: Cardinal): Boolean;
  external 'MoveFileExW@kernel32.dll stdcall';

// File Explorer keeps itemglass.dll and ExplorerBlurMica.dll loaded, so they cannot be overwritten. A loaded DLL can
// still be renamed: the running Explorer keeps using the renamed file, the new one is copied in its place, and the
// next Explorer start loads it. Old copies are deleted here once nothing has them loaded, or at the next restart.
// If the rename fails, restartreplace still replaces the file at the next restart, as before.
procedure MoveAsideInUse();
var
  FilePath, Target: String;
  Index: Integer;
begin
  FilePath := ExpandConstant(CurrentFileName);
  if not FileExists(FilePath) then
    Exit;
  for Index := 0 to 9 do
    if FileExists(FilePath + '.old' + IntToStr(Index)) then
      DeleteFile(FilePath + '.old' + IntToStr(Index));
  if DeleteFile(FilePath) then
    Exit;
  Target := '';
  for Index := 0 to 9 do
    if (Target = '') and not FileExists(FilePath + '.old' + IntToStr(Index)) then
      Target := FilePath + '.old' + IntToStr(Index);
  if Target = '' then
  begin
    Log(Format('No free name to move %s aside; it is replaced at the next restart.', [FilePath]));
    Exit;
  end;
  if RenameFile(FilePath, Target) then
  begin
    Log(Format('Moved the loaded %s aside to %s.', [FilePath, Target]));
    if not MoveFileExNoTarget(Target, 0, MOVEFILE_DELAY_UNTIL_REBOOT) then
      Log(Format('Could not schedule %s for deletion at restart.', [Target]));
  end
  else
    Log(Format('Could not move %s aside; it is replaced at the next restart.', [FilePath]));
end;

// The logon task is what starts the taskbar at sign-in, and a [Run] entry cannot tell whether registering it worked.
procedure CheckTaskbarTask();
var
  ResultCode: Integer;
begin
  if Exec(ExpandConstant('{sys}\schtasks.exe'), '/Query /TN FluidGlassTaskbar', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
    Log('The Fluid Glass taskbar logon task is registered.')
  else
  begin
    Log(Format('The Fluid Glass taskbar logon task is missing after registration (schtasks exit code %d).', [ResultCode]));
    SuppressibleMsgBox(CustomMessage('TaskbarTaskFailed'), mbError, MB_OK, IDOK);
  end;
end;

function InitializeSetup(): Boolean;
var
  Version: TWindowsVersion;
begin
  if RegKeyExists(HKLM64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{#UpstreamAppId}_is1') or
     RegKeyExists(HKLM32, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{#UpstreamAppId}_is1') then
  begin
    Log('OpenGlass or an early Fluid Glass build is installed under the upstream AppId; setup stops.');
    SuppressibleMsgBox(CustomMessage('UpstreamInstalled'), mbError, MB_OK, IDOK);
    Result := False;
    Exit;
  end;
  GetWindowsVersionEx(Version);
  ExplorerBlurMicaTurnedOff :=
    RegKeyExists(HKLM64, 'SOFTWARE\Classes\CLSID\{887D3A6A-502E-4AF5-9CE6-D515E12AFE89}') and
    not RegKeyExists(HKLM64, 'SOFTWARE\Classes\Drive\shellex\FolderExtensions\{887D3A6A-502E-4AF5-9CE6-D515E12AFE89}');
  WindowsBuild := Version.Build;
  InstallMILCompDll := Version.Build >= 28000;
  if InstallMILCompDll then
    Log(Format('Detected Windows build %d; selected the MILComp DWM architecture.', [Version.Build]))
  else
    Log(Format('Detected Windows build %d; selected the Legacy DWM architecture.', [Version.Build]));
  Result := True;
end;

function ShouldInstallLegacyDll(): Boolean;
begin
  Result := not InstallMILCompDll;
end;

function ShouldInstallMILCompDll(): Boolean;
begin
  Result := InstallMILCompDll;
end;

// Open Fluid Glass was called Open Liquid Glass, and its settings were LiquidGlass* DWORDs.
// Moves them to their FluidGlass* names; a value already under the new name wins.
procedure MoveLegacyValues(const RootKey: Integer; const FromKey, ToKey: String);
var
  Names: TArrayOfString;
  i: Integer;
  Value: Cardinal;
  NewName: String;
begin
  if not RegGetValueNames(RootKey, FromKey, Names) then Exit;
  for i := 0 to GetArrayLength(Names) - 1 do
    if (Pos('LiquidGlass', Names[i]) = 1) and RegQueryDWordValue(RootKey, FromKey, Names[i], Value) then
    begin
      NewName := 'FluidGlass' + Copy(Names[i], Length('LiquidGlass') + 1, Length(Names[i]));
      if not RegValueExists(RootKey, ToKey, NewName) then
        RegWriteDWordValue(RootKey, ToKey, NewName, Value);
      if FromKey = ToKey then
        RegDeleteValue(RootKey, FromKey, Names[i]);
    end;
end;

// Like DeleteConfig, HKEY_CURRENT_USER is the account setup runs as.
procedure MigrateLegacySettings;
var
  Presets: TArrayOfString;
  i: Integer;
begin
  MoveLegacyValues(HKEY_CURRENT_USER, 'SOFTWARE\Microsoft\Windows\DWM', 'SOFTWARE\Microsoft\Windows\DWM');
  MoveLegacyValues(HKLM64, 'SOFTWARE\Microsoft\Windows\DWM', 'SOFTWARE\Microsoft\Windows\DWM');
  if RegGetSubkeyNames(HKEY_CURRENT_USER, 'Software\OpenGlass\LiquidGlassPresets', Presets) then
  begin
    for i := 0 to GetArrayLength(Presets) - 1 do
      MoveLegacyValues(HKEY_CURRENT_USER, 'Software\OpenGlass\LiquidGlassPresets\' + Presets[i],
        'Software\OpenGlass\FluidGlassPresets\' + Presets[i]);
    RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, 'Software\OpenGlass\LiquidGlassPresets');
  end;
end;

// Before the service starts in [Run], so OpenGlass reads the settings under their new names.
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    MigrateLegacySettings;
end;

type
  IMMERSIVE_COLOR_PREFERENCE = record
    color1: LongWord;
    color2: LongWord;
  end;

function GetUserColorPreference(var pcpPreference: IMMERSIVE_COLOR_PREFERENCE; fForceReload: Boolean): HRESULT;
external 'GetUserColorPreference@uxtheme.dll stdcall';

var
  DeleteCurrentAndMachineConfigOnUninstall: Boolean;
  DeletePresetsOnUninstall: Boolean;

procedure DeleteConfigValues(const RootKey: Integer; const SubKey: String; const OpenGlassKeys: TArrayOfString);
var
  i: Integer;
begin
  for i := 0 to GetArrayLength(OpenGlassKeys) - 1 do
  begin
    if RegValueExists(RootKey, SubKey, OpenGlassKeys[i]) and
       (not RegDeleteValue(RootKey, SubKey, OpenGlassKeys[i])) then
      Log(Format('Unable to delete OpenGlass registry value %s\%s', [SubKey, OpenGlassKeys[i]]));
  end;
end;

// "Current user" here means the account the elevated uninstaller runs as.
// When uninstall is started from a non-admin account (UAC prompt / run-as),
// HKEY_CURRENT_USER is that admin account, not the launching user; that
// user's settings then remain and are only reachable by logging in as them.
// Inno Setup 6 runs its Pascal code in a 32-bit process, even in x64 install mode.
function GetUserNameExW(NameFormat: Integer; Buffer: String; var Size: LongWord): Boolean;
  external 'GetUserNameExW@secur32.dll stdcall';
function LookupAccountNameW(System: LongWord; Account: String; var Sid: Byte;
  var SidSize: LongWord; Domain: String; var DomainSize: LongWord; var Use: LongWord): Boolean;
  external 'LookupAccountNameW@advapi32.dll stdcall';
function ConvertSidToStringSidW(var Sid: Byte; var Text: LongWord): Boolean;
  external 'ConvertSidToStringSidW@advapi32.dll stdcall';
function CopySidText(Buffer: String; Source: LongWord; Count: Integer): LongWord;
  external 'lstrcpynW@kernel32.dll stdcall';
function FreeSidText(Address: LongWord): LongWord;
  external 'LocalFree@kernel32.dll stdcall';

function ResourceAttributes(Name: String): LongWord;
  external 'GetFileAttributesW@kernel32.dll stdcall';

function CurrentUninstallSid: String;
var
  Account, Domain, Text: String;
  AccountSize, DomainSize, SidSize, Use, Address: LongWord;
  Sid: array of Byte;
begin
  Result := '';
  AccountSize := 512; DomainSize := 512; SidSize := 68;
  SetLength(Account, AccountSize); SetLength(Domain, DomainSize); SetArrayLength(Sid, SidSize);
  if not GetUserNameExW(2, Account, AccountSize) then Exit;
  SetLength(Account, Pos(#0, Account) - 1);
  if not LookupAccountNameW(0, Account, Sid[0], SidSize, Domain, DomainSize, Use) then Exit;
  if not ConvertSidToStringSidW(Sid[0], Address) then Exit;
  try
    SetLength(Text, 256);
    CopySidText(Text, Address, 256);
    SetLength(Text, Pos(#0, Text) - 1);
    if (Pos('S-1-', Text) = 1) and (Pos('\', Text) = 0) and (Pos('/', Text) = 0) then Result := Text;
  finally
    FreeSidText(Address);
  end;
end;

procedure DeleteConfigurationResources(const Folder: String);
var
  Names, Suffixes: TArrayOfString;
  i, j: Integer;
  Parent: String;
begin
  Parent := Folder;
  while Length(Parent) > 3 do
  begin
    if (ResourceAttributes(Parent) <> $FFFFFFFF) and ((ResourceAttributes(Parent) and $400) <> 0) then
    begin
      Log('Preserving configuration resource path containing a reparse point: ' + Folder);
      Exit;
    end;
    Parent := ExtractFileDir(Parent);
  end;
  // Delete only known fixed slots, never recurse into a user-selected asset path.
  Names := ['CustomThemeReflection.png', 'CustomThemeMaterial.png', 'CustomThemeAtlas.png'];
  Suffixes := ['', '.source.json', '.pending', '.restore', '.source.json.pending', '.source.json.restore'];
  for i := 0 to GetArrayLength(Names) - 1 do
    for j := 0 to GetArrayLength(Suffixes) - 1 do
      DeleteFile(Folder + '\' + Names[i] + Suffixes[j]);
  DeleteFile(Folder + '\CustomThemeAtlas.png.layout');
  DeleteFile(Folder + '\CustomThemeAtlas.png.layout.pending');
  DeleteFile(Folder + '\CustomThemeAtlas.png.layout.restore');
  RemoveDir(Folder);
end;

function SafeResourceTree(const Folder: String): Boolean;
var
  Item: TFindRec;
  Parent: String;
begin
  Parent := Folder;
  while Length(Parent) > 3 do
  begin
    if (ResourceAttributes(Parent) <> $FFFFFFFF) and ((ResourceAttributes(Parent) and $400) <> 0) then
    begin
      Result := False;
      Exit;
    end;
    Parent := ExtractFileDir(Parent);
  end;
  Result := (ResourceAttributes(Folder) and $400) = 0;
  if not Result then Exit;
  if FindFirst(Folder + '\*', Item) then
  try
    repeat
      if (Item.Name <> '.') and (Item.Name <> '..') then
      begin
        if (Item.Attributes and $400) <> 0 then Result := False
        else if (Item.Attributes and $10) <> 0 then
          Result := SafeResourceTree(Folder + '\' + Item.Name);
        if not Result then Exit;
      end;
    until not FindNext(Item);
  finally
    FindClose(Item);
  end;
end;

procedure DeleteOwnedRecovery(const Sid: String);
var
  Folder, Marker: String;
  Owner: AnsiString;
begin
  Folder := ExpandConstant('{commonappdata}\OpenGlass\Configuration\.operation-') + Sid;
  if DirExists(Folder) and SafeResourceTree(Folder) then
    if not DelTree(Folder, True, True, True) then Log('Configuration recovery cleanup incomplete: ' + Folder);
  Marker := ExpandConstant('{commonappdata}\OpenGlass\Configuration\operation-owner');
  if LoadStringFromFile(Marker, Owner) and (String(Owner) = Sid) then DeleteFile(Marker);
end;

procedure DeleteConfig;
var
  OpenGlassKeys: array of string;
  pref: IMMERSIVE_COLOR_PREFERENCE;
  Sid: String;
begin
  OpenGlassKeys := [
    'ColorizationColor', 'ColorizationColorOverride', 'ColorizationColorInactive',
    'ColorizationAfterglow', 'ColorizationColorBalance', 'ColorizationAfterglowBalance', 'ColorizationBlurBalance',
    'ColorizationAfterglowOverride', 'ColorizationColorBalanceOverride', 'ColorizationAfterglowBalanceOverride', 'ColorizationBlurBalanceOverride',
    'GlassOpacity', 'GlassOpacityInactive',
    'ColorizationColorCaption', 'ColorizationColorCaptionInactive', 'ColorizationColorCaptionMaximized', 'ColorizationColorCaptionInactiveMaximized',
    'ColorizationOpaqueBlend',
    'ColorizationBaseTransparent', 'ColorizationBaseMaximized', 'ColorizationBaseOpaque',
    'ColorizationOpaqueBlendPriority',
    'ColorizationOpacity', 'ColorizationOpacityInactive', 'ColorizationOpacityMaximized', 'ColorizationOpacityInactiveMaximized',
    'GlassType', 'GlassOverrideAccent', 'CustomThemeReflection',
    'ColorizationGlassReflectionIntensity',
    'ColorizationGlassReflectionOpacity', 'ColorizationGlassReflectionOpacityInactive', 'ColorizationGlassReflectionOpacityMaximized', 'ColorizationGlassReflectionOpacityInactiveMaximized',
    'ColorizationGlassReflectionParallaxIntensity', 'ColorizationGlassReflectionPolicy',
    'BlurDeviation', 'BlurOptimization', 'RoundRectRadius',
    'CustomThemeMaterial', 'MaterialOpacity', 'UseDirect3DRendering',
    'CaptionButtons', 'CenterCaption', 'CaptionTextBold', 'TextGlowMode', 'CustomThemeAtlas', 'DisableModernBorders',
    'DisableGlassOnBattery', 'DisabledHooks', 'GlassSafetyZoneMode',
    'MINMAXBUTTONGLOWid', 'CLOSEBUTTONGLOWid', 'TOOLCLOSEBUTTONGLOWid',
    'FluidGlass', 'FluidGlassIor', 'FluidGlassBezel', 'FluidGlassThickness', 'FluidGlassGap', 'FluidGlassScale',
    'FluidGlassChroma', 'FluidGlassSaturation', 'FluidGlassReadability', 'FluidGlassReadabilityLight', 'FluidGlassRim', 'FluidGlassShine',
    'FluidGlassLight', 'FluidGlassRadius', 'FluidGlassPillRadius', 'FluidGlassProfile', 'FluidGlassClarity',
    'FluidGlassSoftness', 'FluidGlassItems', 'FluidGlassDialogs'
  ];

  DeleteConfigValues(HKEY_CURRENT_USER, 'SOFTWARE\Microsoft\Windows\DWM', OpenGlassKeys);
  DeleteConfigValues(HKEY_LOCAL_MACHINE, 'SOFTWARE\Microsoft\Windows\DWM', OpenGlassKeys);

  DeleteConfigurationResources(ExpandConstant('{commonappdata}\OpenGlass\Configuration\Machine'));
  Sid := CurrentUninstallSid;
  if Sid <> '' then
  begin
    DeleteConfigurationResources(ExpandConstant('{commonappdata}\OpenGlass\Configuration\Users\') + Sid);
    DeleteOwnedRecovery(Sid);
  end
  else
    Log('Could not determine the uninstall account SID; user resource directories were preserved.');

  // Refresh DWM by calling GetUserColorPreference
  GetUserColorPreference(pref, True);
end;

procedure InitializeUninstallProgressForm;
var
  OptionsPage: TNewNotebookPage;
  DeleteCurrentAndMachineConfigCheck: TNewCheckBox;
  OtherUserConfigNotice: TNewStaticText;
  DeletePresetsCheck: TNewCheckBox;
  ContinueButton: TNewButton;
  OriginalPageName: String;
  OriginalPageDescription: String;
  OriginalCancelEnabled: Boolean;
  OriginalCancelModalResult: Integer;
  DialogResult: Integer;
begin
  // Interactive uninstalls present the options page inside the progress
  // window; silent (/SILENT or /VERYSILENT) uninstalls stay fully silent.
  if UninstallSilent then
    Exit;

  OptionsPage := TNewNotebookPage.Create(UninstallProgressForm);
  OptionsPage.Notebook := UninstallProgressForm.InnerNotebook;
  OptionsPage.Align := alClient;

  DeleteCurrentAndMachineConfigCheck := TNewCheckBox.Create(UninstallProgressForm);
  DeleteCurrentAndMachineConfigCheck.Parent := OptionsPage;
  DeleteCurrentAndMachineConfigCheck.Left := UninstallProgressForm.StatusLabel.Left;
  DeleteCurrentAndMachineConfigCheck.Top := UninstallProgressForm.StatusLabel.Top;
  DeleteCurrentAndMachineConfigCheck.Width := UninstallProgressForm.StatusLabel.Width;
  DeleteCurrentAndMachineConfigCheck.Height := ScaleY(20);
  DeleteCurrentAndMachineConfigCheck.Caption :=
    CustomMessage('UninstallDeleteCurrentAndMachineConfig');
  DeleteCurrentAndMachineConfigCheck.Checked :=
    DeleteCurrentAndMachineConfigOnUninstall;

  OtherUserConfigNotice := TNewStaticText.Create(UninstallProgressForm);
  OtherUserConfigNotice.Parent := OptionsPage;
  OtherUserConfigNotice.Left :=
    DeleteCurrentAndMachineConfigCheck.Left + ScaleX(22);
  OtherUserConfigNotice.Top :=
    DeleteCurrentAndMachineConfigCheck.Top + ScaleY(24);
  OtherUserConfigNotice.Width :=
    DeleteCurrentAndMachineConfigCheck.Width - ScaleX(22);
  OtherUserConfigNotice.Height := ScaleY(34);
  OtherUserConfigNotice.AutoSize := False;
  OtherUserConfigNotice.WordWrap := True;
  OtherUserConfigNotice.Caption := CustomMessage('OtherUserConfigNotice');

  DeletePresetsCheck := TNewCheckBox.Create(UninstallProgressForm);
  DeletePresetsCheck.Parent := OptionsPage;
  DeletePresetsCheck.Left := DeleteCurrentAndMachineConfigCheck.Left;
  DeletePresetsCheck.Top :=
    DeleteCurrentAndMachineConfigCheck.Top + ScaleY(62);
  DeletePresetsCheck.Width := DeleteCurrentAndMachineConfigCheck.Width;
  DeletePresetsCheck.Height := ScaleY(20);
  DeletePresetsCheck.Caption :=
    CustomMessage('UninstallDeletePresetPackages');
  DeletePresetsCheck.Checked := DeletePresetsOnUninstall;

  ContinueButton := TNewButton.Create(UninstallProgressForm);
  ContinueButton.Parent := UninstallProgressForm;
  ContinueButton.Caption := CustomMessage('ContinueUninstall');
  ContinueButton.Left :=
    UninstallProgressForm.CancelButton.Left -
    ScaleX(8) -
    UninstallProgressForm.CancelButton.Width;
  ContinueButton.Top := UninstallProgressForm.CancelButton.Top;
  ContinueButton.Width := UninstallProgressForm.CancelButton.Width;
  ContinueButton.Height := UninstallProgressForm.CancelButton.Height;
  ContinueButton.Anchors := [akRight, akBottom];
  ContinueButton.ModalResult := mrOk;
  ContinueButton.Default := True;

  OriginalPageName := UninstallProgressForm.PageNameLabel.Caption;
  OriginalPageDescription :=
    UninstallProgressForm.PageDescriptionLabel.Caption;
  OriginalCancelEnabled := UninstallProgressForm.CancelButton.Enabled;
  OriginalCancelModalResult :=
    UninstallProgressForm.CancelButton.ModalResult;

  UninstallProgressForm.PageNameLabel.Caption :=
    CustomMessage('UninstallOptionsTitle');
  UninstallProgressForm.PageDescriptionLabel.Caption :=
    CustomMessage('UninstallOptionsDescription');
  UninstallProgressForm.InnerNotebook.ActivePage := OptionsPage;
  UninstallProgressForm.CancelButton.Enabled := True;
  UninstallProgressForm.CancelButton.ModalResult := mrCancel;
  UninstallProgressForm.ActiveControl := ContinueButton;

  DialogResult := UninstallProgressForm.ShowModal;
  if DialogResult = mrOk then
  begin
    DeleteCurrentAndMachineConfigOnUninstall :=
      DeleteCurrentAndMachineConfigCheck.Checked;
    DeletePresetsOnUninstall := DeletePresetsCheck.Checked;
  end;

  ContinueButton.Visible := False;
  UninstallProgressForm.PageNameLabel.Caption := OriginalPageName;
  UninstallProgressForm.PageDescriptionLabel.Caption :=
    OriginalPageDescription;
  UninstallProgressForm.CancelButton.Enabled := OriginalCancelEnabled;
  UninstallProgressForm.CancelButton.ModalResult :=
    OriginalCancelModalResult;
  UninstallProgressForm.InnerNotebook.ActivePage :=
    UninstallProgressForm.InstallingPage;

  if DialogResult = mrCancel then
    Abort;
end;

function InitializeUninstall(): Boolean;
var
  CmdTail: String;
begin
  CmdTail := LowerCase(GetCmdTail);
  DeleteCurrentAndMachineConfigOnUninstall :=
    Pos('/nodeleteconfig', CmdTail) = 0;
  DeletePresetsOnUninstall := Pos('/deletepresets', CmdTail) > 0;
  Result := True;
end;

procedure CurUninstallStepChanged(
  CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    // Not always in the uninstall log: the GUI can turn ExplorerBlurMica on after an install that left it off.
    RegDeleteKeyIncludingSubkeys(HKLM64, 'SOFTWARE\Classes\Drive\shellex\FolderExtensions\{887D3A6A-502E-4AF5-9CE6-D515E12AFE89}');
    if DeleteCurrentAndMachineConfigOnUninstall then
      DeleteConfig;

    // Preset packages are user-created/downloaded content and are preserved by default.
    if DeletePresetsOnUninstall then
      DelTree(
        ExpandConstant('{commonappdata}\OpenGlass\Presets'),
        True,
        True,
        True);

    // Remove the shared root only when no preserved package or undeleted runtime data remains.
    RemoveDir(ExpandConstant('{commonappdata}\OpenGlass'));
  end;
end;
