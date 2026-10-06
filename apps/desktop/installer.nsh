!macro customInstall
  SetRegView 64
  SetShellVarContext all
  CreateDirectory "$APPDATA\Kotaus\RemotePhone\logs"
  IfErrors 0 +2
    Abort "Не удалось создать каталог журнала виртуальной камеры в ProgramData."
  FileOpen $0 "$APPDATA\Kotaus\RemotePhone\logs\VirtualCameraMediaSource.log" a
  IfErrors 0 +2
    Abort "Не удалось создать журнал виртуальной камеры в ProgramData."
  FileClose $0

  ExecWait '"$SYSDIR\icacls.exe" "$APPDATA\Kotaus\RemotePhone\logs" /grant:r "*S-1-5-18:(OI)(CI)F" "*S-1-5-19:(OI)(CI)M" "*S-1-5-20:(OI)(CI)M" "*S-1-5-32-545:(OI)(CI)RX" /T /C /Q' $0
  StrCmp $0 0 +2
    Abort "Не удалось настроить права каталога журнала в ProgramData."
  ExecWait '"$SYSDIR\icacls.exe" "$APPDATA\Kotaus\RemotePhone\logs\VirtualCameraMediaSource.log" /grant:r "*S-1-5-18:F" "*S-1-5-19:M" "*S-1-5-20:M" "*S-1-5-32-545:(R,AD)" /C /Q' $0
  StrCmp $0 0 +2
    Abort "Не удалось настроить права записи службы на журнал в ProgramData."

  WriteRegStr HKLM "Software\Classes\CLSID\{7B89B92E-FE71-42D0-8A41-E137D06EA184}" "" "Видоискатель — источник камеры"
  WriteRegStr HKLM "Software\Classes\CLSID\{7B89B92E-FE71-42D0-8A41-E137D06EA184}\InprocServer32" "" "$INSTDIR\resources\native\VirtualCameraMediaSource.dll"
  WriteRegStr HKLM "Software\Classes\CLSID\{7B89B92E-FE71-42D0-8A41-E137D06EA184}\InprocServer32" "ThreadingModel" "Both"
!macroend

!macro customUnInstall
  SetRegView 64
  ExecWait '"$SYSDIR\taskkill.exe" /F /IM RemotePhone.VirtualCameraHost.exe' $0
  DeleteRegKey HKLM "Software\Classes\CLSID\{7B89B92E-FE71-42D0-8A41-E137D06EA184}"
  ; Keep ProgramData diagnostics after removal so a failed camera test can still be investigated.
!macroend
