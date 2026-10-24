@echo off
rem Configure, compile and test one preset in the MSVC x64 environment.
rem Usage: tools\preset.bat <preset> [configure|compile|test|all]
setlocal
set PRESET=%~1
if "%PRESET%"=="" set PRESET=x64-release
set STEP=%~2
if "%STEP%"=="" set STEP=all

rem Use the newest Visual Studio that has the C++ tools and a complete vcvars64.bat.
set VSDIR=
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -all -sort -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
  if not defined VSDIR if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%%i"
)
if "%VSDIR%"=="" (
  echo Visual Studio with the C++ tools is not installed.
  exit /b 1
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%

cd /d "%~dp0.."
if "%STEP%"=="configure" goto configure
if "%STEP%"=="all" goto configure
if "%STEP%"=="compile" goto compile
if "%STEP%"=="test" goto test
echo Unknown step: %STEP%
exit /b 1

:configure
cmake --preset %PRESET% || exit /b 1
if "%STEP%"=="configure" exit /b 0
:compile
cmake --build --preset %PRESET% || exit /b 1
if "%STEP%"=="compile" exit /b 0
:test
ctest --preset %PRESET% || exit /b 1
exit /b 0
