@echo off
rem Runs a command inside the MSVC 14.51 (Visual Studio 18) x64 environment.
rem   scripts\with-msvc.cmd cmake --workflow --preset msvc-dev
rem The engine needs MSVC 14.50 or later (glaze); VS 2022 (14.44) is too old.
setlocal
set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
if defined SE_VS_ROOT set "VSROOT=%SE_VS_ROOT%"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.51 >nul
if errorlevel 1 (
  echo with-msvc: vcvars64.bat failed for "%VSROOT%" 1>&2
  exit /b 1
)
%*
exit /b %ERRORLEVEL%
