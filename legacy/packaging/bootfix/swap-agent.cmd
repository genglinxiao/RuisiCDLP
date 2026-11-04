@echo off
rem ---------------------------------------------------------------------------
rem Swap the installed XP agent for the freshly built cm-backend binary.
rem
rem The previous log kept showing "dry_run=true" and had no debug_config_enable
rem line, which means the service was still running the OLD exe: the file swap
rem never took effect (copy skipped, wrong path, or file locked).
rem
rem Usage:  swap-agent.cmd <path to new UsbDlp.LegacyAgent.exe>
rem Run as Administrator.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions
set "SRC=%~1"
set "AGENT=UsbDlp.LegacyAgent"
set "DESTDIR=C:\Program Files\Ruisi USB DLP Legacy"
set "DEST=%DESTDIR%\UsbDlp.LegacyAgent.exe"
set "EXPECTED=372224"

net session >nul 2>&1
if errorlevel 1 (
  echo ERROR: run as Administrator.
  pause
  exit /b 5
)
if "%SRC%"=="" (
  echo Usage: swap-agent.cmd ^<path to new UsbDlp.LegacyAgent.exe^>
  pause
  exit /b 2
)
if not exist "%SRC%" (
  echo ERROR: source not found: %SRC%
  pause
  exit /b 2
)

echo Source : %SRC%
for %%F in ("%SRC%") do echo Size   : %%~zF bytes  ^(new build should be %EXPECTED%^)
for %%F in ("%DEST%") do echo Current: %%~zF bytes

echo.
echo [1/4] stopping the service
sc stop "%AGENT%" >nul 2>&1
ping -n 4 127.0.0.1 >nul
sc query "%AGENT%" | find /i "STOPPED" >nul
if errorlevel 1 (
  echo       service did not stop. Reboot, then run this script again.
  pause
  exit /b 6
)
echo       stopped

echo.
echo [2/4] backing up the current binary
if exist "%DEST%" (
  copy /y "%DEST%" "%DEST%.bak" >nul
  echo       saved as UsbDlp.LegacyAgent.exe.bak
) else (
  echo       no existing binary at %DEST%
)

echo.
echo [3/4] copying the new binary
attrib -r "%DEST%" >nul 2>&1
copy /y "%SRC%" "%DEST%" >nul
if errorlevel 1 (
  echo       COPY FAILED - file may still be locked. Reboot and retry.
  pause
  exit /b 7
)
for %%F in ("%DEST%") do set "NOW=%%~zF"
echo       copied, destination is %NOW% bytes

echo.
echo [4/4] starting the service
sc start "%AGENT%" >nul 2>&1
ping -n 4 127.0.0.1 >nul
sc query "%AGENT%" | find /i "RUNNING" >nul
if errorlevel 1 (
  echo       service did not report RUNNING - check Event Viewer
) else (
  echo       running
)

echo.
echo ============================================================
if not "%NOW%"=="%EXPECTED%" (
  echo  WARNING: destination size %NOW% does not match the expected
  echo  %EXPECTED% bytes. You may have copied the OLD binary.
  echo  Re-download the kit and use artifacts\xp-cm-1.1.9\UsbDlp.LegacyAgent.exe
) else (
  echo  Done. Insert a USB disk and check the log:
  echo  the first line must read  agent_start dry_run=false
  echo  and the next line must contain debug_config_enable
)
echo ============================================================
pause
exit /b 0
