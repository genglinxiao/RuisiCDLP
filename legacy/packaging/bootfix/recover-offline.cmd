@echo off
rem ---------------------------------------------------------------------------
rem Ruisi USB DLP - offline rescue for an XP machine that already STOPs 0x7B
rem
rem Use when the affected machine can no longer boot. Detach its disk (or mount
rem the virtual disk / image), attach it to a WORKING Windows machine or WinPE,
rem and run this script against the dead system's Windows directory, e.g.
rem
rem     recover-offline.cmd E:\WINDOWS
rem
rem What it does: loads the dead system's SYSTEM hive offline, removes the disk
rem class UpperFilters override (back to PartMgr only) and disables the filter
rem service in every control set. The machine then boots without the filter;
rem uninstall the agent normally after that.
rem
rem Exit codes: 0 ok / 2 usage / 3 hive missing / 4 hive load failed
rem ---------------------------------------------------------------------------
setlocal EnableExtensions
set "WIN=%~1"
if "%WIN%"=="" goto :usage
set "HIVE=%WIN%\system32\config\SYSTEM"
if not exist "%HIVE%" goto :no_hive

reg query HKLM\RUISIOFF >nul 2>&1
if not errorlevel 1 reg unload HKLM\RUISIOFF >nul 2>&1

echo Loading %HIVE%
reg load HKLM\RUISIOFF "%HIVE%"
if errorlevel 1 goto :load_failed

for %%N in (001 002 003 004 005 006 007 008 009 010) do call :fixset %%N

reg unload HKLM\RUISIOFF >nul 2>&1
if errorlevel 1 (
  echo WARNING: the hive could not be unloaded. Close regedit/Explorer views on it
  echo and retry, or reboot the rescue machine so the hive is flushed.
)
echo.
echo RESCUE_DONE  UpperFilters reset to PartMgr, filter service disabled.
echo Move the disk back and boot the machine, then uninstall the agent.
endlocal
exit /b 0

:fixset
reg query "HKLM\RUISIOFF\ControlSet%1\Control\Class\{4d36e967-e325-11ce-bfc1-08002be10318}" >nul 2>&1
if errorlevel 1 goto :eof
reg add "HKLM\RUISIOFF\ControlSet%1\Control\Class\{4d36e967-e325-11ce-bfc1-08002be10318}" /v UpperFilters /t REG_MULTI_SZ /d "PartMgr" /f >nul
if errorlevel 1 echo   ControlSet%1: UpperFilters write FAILED
reg query "HKLM\RUISIOFF\ControlSet%1\Services\RuisiUsbDlpFilter" >nul 2>&1
if not errorlevel 1 (
  reg add "HKLM\RUISIOFF\ControlSet%1\Services\RuisiUsbDlpFilter" /v Start /t REG_DWORD /d 4 /f >nul
  echo   ControlSet%1: UpperFilters=PartMgr, service Start=4
) else (
  echo   ControlSet%1: UpperFilters=PartMgr
)
goto :eof

:usage
echo Usage: recover-offline.cmd ^<drive^>:\WINDOWS
endlocal
exit /b 2

:no_hive
echo ERROR: %HIVE% not found. Point the script at the dead system's Windows folder.
endlocal
exit /b 3

:load_failed
echo ERROR: reg load failed. The hive may be locked by another tool.
endlocal
exit /b 4
