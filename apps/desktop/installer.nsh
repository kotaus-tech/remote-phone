!macro customInstall
  SetRegView 64
  WriteRegStr HKLM "Software\Classes\CLSID\{7B89B92E-FE71-42D0-8A41-E137D06EA184}" "" "Видоискатель — источник камеры"
  WriteRegStr HKLM "Software\Classes\CLSID\{7B89B92E-FE71-42D0-8A41-E137D06EA184}\InprocServer32" "" "$INSTDIR\resources\native\VirtualCameraMediaSource.dll"
  WriteRegStr HKLM "Software\Classes\CLSID\{7B89B92E-FE71-42D0-8A41-E137D06EA184}\InprocServer32" "ThreadingModel" "Both"
!macroend

!macro customUnInstall
  SetRegView 64
  ExecWait '"$SYSDIR\taskkill.exe" /F /IM RemotePhone.VirtualCameraHost.exe' $0
  DeleteRegKey HKLM "Software\Classes\CLSID\{7B89B92E-FE71-42D0-8A41-E137D06EA184}"
!macroend
