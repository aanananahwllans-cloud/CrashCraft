@echo off
rem CrashCraft: starts Crash Bandicoot (c1, fullscreen) and your CrashCraft Minecraft instance.
rem   run_crashcraft.bat [level id]       e.g. run_crashcraft.bat 9  (N. Sanity Beach)
rem F11: fullscreen on/off. Set CRASHCRAFT_WINDOWED=1 to start in a window.
rem Your paths go in local.bat next to this file (see README), e.g.
rem   set MSYS2_ROOT=C:\msys64
rem   set PRISM=C:\Users\you\AppData\Local\Programs\PrismLauncher\prismlauncher.exe
rem   set PRISM_INSTANCE=CrashCraft
setlocal
if exist "%~dp0local.bat" call "%~dp0local.bat"
if "%MSYS2_ROOT%"=="" set MSYS2_ROOT=C:\msys64
if "%PRISM%"=="" set PRISM=%LOCALAPPDATA%\Programs\PrismLauncher\prismlauncher.exe
if "%PRISM_INSTANCE%"=="" set PRISM_INSTANCE=CrashCraft
set PATH=%MSYS2_ROOT%\mingw32\bin;%PATH%
if "%1"=="" (set ARGS=) else (set ARGS=--level %1)
start "" /D "%~dp0c1" "%~dp0c1\c1.exe" %ARGS%
if exist "%PRISM%" (
  start "" "%PRISM%" --launch "%PRISM_INSTANCE%"
) else (
  echo Start your CrashCraft Minecraft instance yourself, or set PRISM in local.bat
)
