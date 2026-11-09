[Setup]
AppId=com.marcsoul.cronocam-sync-meter
AppName=Cronocam Sync Meter
AppVersion={#PluginVersion}
AppPublisher=MarcSoul
DefaultDirName={commonappdata}\obs-studio\plugins\cronocam-sync-meter
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=cronocam-sync-meter-{#PluginVersion}-windows-x64
UninstallDisplayName=Cronocam Sync Meter

[Files]
Source: "{#PluginDll}"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "..\data\locale\*.ini"; DestDir: "{app}\data\locale"; Flags: ignoreversion

[Messages]
SetupAppTitle=Cronocam Sync Meter
