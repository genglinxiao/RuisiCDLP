@echo off
rem Ruisi USB DLP - run the read only storage surface inventory and keep a copy.
rem Usage: list-storage-surface.cmd            (prints to screen + %TEMP% file)
rem        list-storage-surface.cmd D:\out    (also writes a csv next to the script)
setlocal EnableExtensions
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%TEMP%"
set "CSV=%OUT%\storage-surface-%COMPUTERNAME%.csv"
cscript.exe //nologo "%~dp0list-storage-surface.vbs" > "%CSV%"
if errorlevel 1 (
  echo ERROR: cscript failed. Is WMI service running?
  exit /b 1
)
type "%CSV%"
echo.
echo Saved to %CSV%
endlocal
exit /b 0
