SetCompress auto
SetCompressor lzma
SetCompressorDictSize 64 ; Sets the dictionary size in megabytes
FileSize 4000 ; Sets the max single file size in megabytes, 0 means no limit.
;SetDataFile force ; it can be off/auto/force, default is auto.
; if SetDataFile set to auto,and FileSize set to be 0, it means once the total length reaches 4GB, it will use data file ,and the data file is single
; if SetDataFile set to auto,and FileSize set to be none-zero, it means once the total length reaches FileSize, it will use data file, and the data file is stored per FileSize.

!include "MUI.nsh"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE_3LINES

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_RESERVEFILE_INSTALLOPTIONS

Name "TestApp"
OutFile "TestInstaller.exe"
InstallDir "$PROGRAMFILES\TestApp\"
ShowInstDetails show
ShowUnInstDetails show

RequestExecutionLevel admin

Section "-LogSetOn"
  Delete $INSTDIR\install.log
  LogSet on
SectionEnd

Section "MainSection" SEC01
 SetOverwrite on
 SetOutPath "$INSTDIR"
 File /r "D:\Downloads\test\*.*"
; File /r "D:\test\boost_1_49_0\*.*"
; File /r "D:\Downloads\test1\*.*"
 WriteUninstaller "$INSTDIR\uninst.exe"

SectionEnd

Section Uninstall
 SetAutoClose true

SectionEnd

Function .onInit
FunctionEnd

Function un.onInit
FunctionEnd
