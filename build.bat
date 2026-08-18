@echo off
rem ============================================================
rem  build.bat - one-click build for 7-Zip Password Vault fork
rem  Builds (x86): 7zFM.exe, 7zG.exe, 7z.dll
rem  Output      : .\bin\7zFM.exe, .\bin\7zG.exe, .\bin\7z.dll
rem
rem  Usage:
rem    build.bat          build all three targets
rem    build.bat clean    delete all build output (o\, bin\) first
rem ============================================================
setlocal

set "ROOT=%~dp0"
rem (x86) inside a variable name breaks cmd block parsing, so resolve it here:
set "PF86=%ProgramFiles(x86)%"
set "PF=%ProgramFiles%"

if /i "%~1"=="clean" (
  echo Cleaning build output...
  if exist "%ROOT%CPP\7zip\Bundles\Fm\o"      rmdir /s /q "%ROOT%CPP\7zip\Bundles\Fm\o"
  if exist "%ROOT%CPP\7zip\UI\GUI\o"          rmdir /s /q "%ROOT%CPP\7zip\UI\GUI\o"
  if exist "%ROOT%CPP\7zip\Bundles\Format7zF\o" rmdir /s /q "%ROOT%CPP\7zip\Bundles\Format7zF\o"
  if exist "%ROOT%bin" rmdir /s /q "%ROOT%bin"
)

rem ---- make sure we are in an x86 MSVC environment ----
where cl >nul 2>&1
if not errorlevel 1 goto :env_ok

echo cl.exe not found in PATH. Locating Visual Studio...
set "VSROOT="
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%PF%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSROOT=%%i"
if not defined VSROOT for %%p in ("D:\Program Files\Microsoft Visual Studio\18\Community" "%PF%\Microsoft Visual Studio\2022\Community" "%PF%\Microsoft Visual Studio\2022\Professional" "%PF%\Microsoft Visual Studio\2022\Enterprise" "%PF86%\Microsoft Visual Studio\2022\BuildTools") do if not defined VSROOT if exist "%%~p\VC\Auxiliary\Build\vcvars32.bat" set "VSROOT=%%~p"
if not defined VSROOT (
  echo ERROR: cannot locate Visual Studio VC tools.
  echo        Install VS with the "Desktop development with C++" workload,
  echo        or run this script from an "x86 Native Tools Command Prompt".
  exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars32.bat" || exit /b 1

:env_ok

echo ============================================================
echo [1/3] Building 7zFM.exe ...
echo ============================================================
pushd "%ROOT%CPP\7zip\Bundles\Fm"
nmake /nologo O=o
if errorlevel 1 goto :err
popd

echo ============================================================
echo [2/3] Building 7zG.exe ...
echo ============================================================
pushd "%ROOT%CPP\7zip\UI\GUI"
nmake /nologo O=o
if errorlevel 1 goto :err
popd

echo ============================================================
echo [3/3] Building 7z.dll ...
echo ============================================================
pushd "%ROOT%CPP\7zip\Bundles\Format7zF"
nmake /nologo O=o
if errorlevel 1 goto :err
popd

rem ---- stage the three binaries into .\bin ----
if not exist "%ROOT%bin" mkdir "%ROOT%bin"
copy /Y "%ROOT%CPP\7zip\Bundles\Fm\o\7zFM.exe"      "%ROOT%bin\" >nul
copy /Y "%ROOT%CPP\7zip\UI\GUI\o\7zG.exe"          "%ROOT%bin\" >nul
copy /Y "%ROOT%CPP\7zip\Bundles\Format7zF\o\7z.dll" "%ROOT%bin\" >nul

echo.
echo ============================================================
echo Build OK. Run:  "%ROOT%bin\7zFM.exe"
echo ============================================================
exit /b 0

:err
echo.
echo BUILD FAILED.
exit /b 1
