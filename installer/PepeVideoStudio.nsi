; ============================================================================
;  PepeVideo Studio - instalador NSIS (Fase 6)
;
;  Se compila desde la raíz del repositorio con:
;     make installer
;  (el objetivo prepara build/mingw/installer-stage con windeployqt + las DLL
;   de FFmpeg y luego invoca makensis sobre este script).
;
;  Compilación manual:
;     makensis /DSTAGE=..\build\mingw\installer-stage installer\PepeVideoStudio.nsi
; ============================================================================

!ifndef VERSION
  !define VERSION "0.1.0"
!endif
!ifndef STAGE
  !define STAGE "..\build\mingw\installer-stage"
!endif

!define APPNAME  "PepeVideo Studio"
!define COMPANY  "Pepe"
!define EXENAME  "PepeVideoStudio.exe"
!define UNINST   "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"

Unicode true
Name "${APPNAME}"
OutFile "PepeVideoStudio-${VERSION}-setup.exe"
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "Software\${COMPANY}\${APPNAME}" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "Instalar"
  SetOutPath "$INSTDIR"
  ; Árbol autocontenido preparado por "make installer" (exe + DLLs de Qt y
  ; FFmpeg + plugins/QML de windeployqt).
  File /r "${STAGE}\*"

  ; Accesos directos (menú Inicio) y desinstalador.
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\${EXENAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Desinstalar ${APPNAME}.lnk" "$INSTDIR\uninstall.exe"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Registro: ruta de instalación y entrada de "Aplicaciones instaladas".
  WriteRegStr HKLM "Software\${COMPANY}\${APPNAME}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINST}" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST}" "Publisher" "${COMPANY}"
  WriteRegStr HKLM "${UNINST}" "DisplayIcon" "$INSTDIR\${EXENAME}"
  WriteRegStr HKLM "${UNINST}" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegDWORD HKLM "${UNINST}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  ; El instalador solo escribe dentro de $INSTDIR: borrado recursivo seguro.
  RMDir /r "$INSTDIR"
  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\Desinstalar ${APPNAME}.lnk"
  RMDir "$SMPROGRAMS\${APPNAME}"
  DeleteRegKey HKLM "${UNINST}"
  DeleteRegKey HKLM "Software\${COMPANY}\${APPNAME}"
SectionEnd
