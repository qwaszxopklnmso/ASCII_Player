@echo off
setlocal
set "VSLANG=1033"
set "VSWHere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHere%" (
  echo vswhere.exe not found. Install Visual Studio with the C++ toolchain.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo Visual C++ toolchain not found.
  exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist build mkdir build
echo Building ASCIIPlayer.exe ...
cl /nologo /std:c++17 /EHsc /O2 /MT /W3 /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 src\*.cpp /Fo:build\ /Fe:build\ASCIIPlayer.exe /link winmm.lib user32.lib
if errorlevel 1 exit /b 1
echo Built: build\ASCIIPlayer.exe
