@echo off
setlocal enabledelayedexpansion

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
  "%%i" wsldskmnt.sln %*
  exit /b !errorlevel!
)
echo msbuild not found
exit /b 1
