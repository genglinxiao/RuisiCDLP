@echo off
rem ---------------------------------------------------------------------------
rem Ruisi USB DLP - XP physical test pre-flight check (READ ONLY, changes nothing)
rem
rem Answers one question: is this machine a valid baseline for a B-route test,
rem or does it still carry residue from a previous install that will make the
rem test result meaningless?
rem
rem Run as Administrator. Exit 0 = GO, exit 1 = NO-GO.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions
set "SVC=RuisiUsbDlpFilter"
set "AGENT=UsbDlp.LegacyAgent"
set "SVCKEY=HKLM\SYSTEM\CurrentControlSet\Services\%SVC%"
set "CLASSKEY=HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e967-e325-11ce-bfc1-08002be10318}"
set "DRV=%SystemRoot%\System32\drivers\RuisiUsbDlpFilter.sys"
set "DATADIR=%ALLUSERSPROFILE%\Application Data\UsbDlp"
set "NOGO="
set "REASON="

echo ============================================================
echo  Ruisi USB DLP - XP test baseline check
echo ============================================================
echo.

net session >nul 2>&1
if errorlevel 1 (
  echo [FAIL] not running as Administrator
  set "NOGO=1"
  set "REASON=%REASON% admin;"
) else (
  echo [ OK ] running as Administrator
)

ver | find /i "5.1." >nul
if errorlevel 1 (
  echo [FAIL] this is not Windows XP ^(kernel 5.1^)
  set "NOGO=1"
  set "REASON=%REASON% not_xp;"
) else (
  for /f "tokens=*" %%V in ('ver') do echo [ OK ] %%V
)

echo.
echo --- leftover kernel filter (the 1.1.8 blue-screen track) ---
sc query "%SVC%" >nul 2>&1
if errorlevel 1 (
  echo [ OK ] filter service not installed
) else (
  if exist "%DRV%" (
    echo [FAIL] filter service %SVC% present WITH driver file  ^<-- must be removed
    set "NOGO=1"
    set "REASON=%REASON% filter_service;"
  ) else (
    echo [WARN] orphan service key %SVC% ^(driver file already gone^)
    echo        harmless: nothing to load and UpperFilters is clean,
    echo        but delete it cleanly with:
    echo        reg delete "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%" /f
  )
)
reg query "%CLASSKEY%" /v UpperFilters 2>nul | find /i "RuisiUsbDlpFilter" >nul
if errorlevel 1 (
  echo [ OK ] disk class UpperFilters is clean
) else (
  echo [FAIL] %CLASSKEY%
  echo        still lists RuisiUsbDlpFilter  ^<-- boot risk, must be removed
  set "NOGO=1"
  set "REASON=%REASON% upperfilters;"
)
if exist "%DRV%" (
  echo [WARN] driver file still on disk: %DRV%
  echo        a stale file alone is harmless if no service and no UpperFilters,
  echo        but delete it so the next install cannot pick it up
) else (
  echo [ OK ] no driver file on disk
)

echo.
echo --- previous agent ---
sc query "%AGENT%" >nul 2>&1
if errorlevel 1 (
  echo [ OK ] agent service not installed
) else (
  echo [WARN] agent service %AGENT% present
  echo        for a clean baseline run: msiexec /x {product} /qn  then reboot
  set "REASON=%REASON% agent_present;"
)
if exist "%DATADIR%" (
  echo [WARN] leftover data directory: %DATADIR%
  echo        holds legacy-agent.ini, the encrypted policy cache and owned-devices.dat
) else (
  echo [ OK ] no agent data directory
)

echo.
echo --- devices left disabled by a previous run ---
if exist "%~dp0list-storage-surface.vbs" (
  for /f "tokens=2 delims=|" %%S in ('cscript.exe //nologo "%~dp0list-storage-surface.vbs" ^| find /i "disabled_total"') do set "DISABLED_COUNT=%%S"
) else (
  echo [SKIP] list-storage-surface.vbs not next to this script
  echo        check Device Manager manually for devices with a red X / yellow mark
)

if defined DISABLED_COUNT (
  if "%DISABLED_COUNT%"=="0" (
    echo [ OK ] no disabled device nodes
  ) else (
    echo [FAIL] %DISABLED_COUNT% device^(s^) are disabled right now  ^<-- must be enabled first
    echo        run: UsbDlp.LegacyAgent.exe --restore-owned  ^(if the agent is still there^)
    echo        or enable them by hand in Device Manager
    set "NOGO=1"
    set "REASON=%REASON% disabled_devices;"
  )
)

echo.
echo --- AutoRun (infection vector, must be off before the test) ---
set "AUTORUN="
for /f "tokens=3" %%A in ('reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\Explorer" /v NoDriveTypeAutoRun 2^>nul ^| find /i "NoDriveTypeAutoRun"') do set "AUTORUN=%%A"
if /i "%AUTORUN%"=="0xff" (
  echo [ OK ] NoDriveTypeAutoRun=0xff ^(AutoRun disabled^)
) else (
  echo [WARN] NoDriveTypeAutoRun=%AUTORUN%  ^(not 0xff^)
  echo        test results on the 2 second enforcement window will be polluted
  echo        set: reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\Explorer" /v NoDriveTypeAutoRun /t REG_DWORD /d 255 /f
)

echo.
echo ============================================================
if defined NOGO (
  echo  VERDICT: NO-GO  ^(reasons:%REASON%^)
  echo  Clean the items marked FAIL first, then run this check again.
  echo ============================================================
  endlocal
  exit /b 1
)
echo  VERDICT: GO  - this machine is a valid B-route test baseline
echo ============================================================
endlocal
exit /b 0
