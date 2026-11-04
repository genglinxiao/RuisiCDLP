@echo off
rem ---------------------------------------------------------------------------
rem Ruisi USB DLP - clean an XP machine back to a valid test baseline.
rem
rem Removes, in a safe order:
rem   1. the 1.1.8 kernel filter service (orphan or active)
rem   2. the driver file on disk
rem   3. the previous agent (MSI uninstall)
rem   4. the agent data directory (policy cache, owned-devices.dat)
rem   5. turns AutoRun off
rem
rem Refuses to touch anything while the disk class UpperFilters still lists the
rem filter: removing the service first in that state is what makes XP unbootable.
rem Run that case through UsbDlp.XpFilterSetup.exe uninstall instead.
rem
rem Run as Administrator. Reboot afterwards, then re-run xp-test-preflight.cmd.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions
set "SVC=RuisiUsbDlpFilter"
set "AGENT=UsbDlp.LegacyAgent"
set "SVCKEY=HKLM\SYSTEM\CurrentControlSet\Services\%SVC%"
set "CLASSKEY=HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e967-e325-11ce-bfc1-08002be10318}"
set "DRV=%SystemRoot%\System32\drivers\RuisiUsbDlpFilter.sys"
set "DATADIR=%ALLUSERSPROFILE%\Application Data\UsbDlp"
set "MSI={546D3FA6-29B8-4E82-8221-B67D6BCF1716}"
set "REBOOT_NEEDED="

net session >nul 2>&1
if errorlevel 1 (
  echo ERROR: run as Administrator.
  pause
  exit /b 5
)

echo ============================================================
echo  Cleaning this XP machine back to a test baseline
echo ============================================================

echo.
echo [0/5] safety check: disk class UpperFilters
reg query "%CLASSKEY%" /v UpperFilters 2>nul | find /i "%SVC%" >nul
if not errorlevel 1 (
  echo STOP. UpperFilters still lists %SVC%.
  echo Deleting the service in this state leaves a dangling filter entry and the
  echo machine will NOT boot. Use the proper removal path first:
  echo     UsbDlp.XpFilterSetup.exe uninstall
  echo then reboot, then run this script again.
  pause
  exit /b 6
)
echo       UpperFilters does not list the filter - safe to proceed.

echo.
echo [1/5] kernel filter service
sc query "%SVC%" >nul 2>&1
if errorlevel 1 (
  echo       not present
) else (
  sc stop "%SVC%" >nul 2>&1
  sc delete "%SVC%" >nul 2>&1
  if errorlevel 1 (
    echo       sc delete FAILED - trying the registry key directly
    set "REBOOT_NEEDED=1"
  ) else (
    echo       deleted (takes effect after reboot)
    set "REBOOT_NEEDED=1"
  )
  rem A driver service that was marked for deletion can survive a reboot when its
  rem binary is already gone. UpperFilters is clean at this point, so removing the
  rem key directly is safe.
  sc query "%SVC%" >nul 2>&1
  if not errorlevel 1 (
    reg delete "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%" /f >nul 2>&1
    if errorlevel 1 (
      echo       registry key still there - delete it by hand:
      echo       reg delete "HKLM\SYSTEM\CurrentControlSet\Services\%SVC%" /f
    ) else (
      echo       registry key removed
    )
  )
)

echo.
echo [2/5] driver file
if exist "%DRV%" (
  del /f /q "%DRV%" 2>nul
  if exist "%DRV%" (
    echo       still in use - it will disappear after reboot
  ) else (
    echo       deleted
  )
) else (
  echo       not present
)

echo.
echo [3/5] previous agent (MSI %MSI%)
sc query "%AGENT%" >nul 2>&1
if errorlevel 1 (
  echo       agent service not present, skipping MSI uninstall
) else (
  sc stop "%AGENT%" >nul 2>&1
  ping -n 3 127.0.0.1 >nul
  msiexec.exe /x "%MSI%" /qn /norestart
  if errorlevel 1 (
    echo       MSI uninstall FAILED ^(1619/1605 = package not cached^).
    echo       Use Control Panel -^> Add or Remove Programs for this one,
    echo       then re-run this script.
    sc delete "%AGENT%" >nul 2>&1
    set "REBOOT_NEEDED=1"
  ) else (
    sc query "%AGENT%" >nul 2>&1
    if errorlevel 1 (
      echo       agent uninstalled
    ) else (
      sc delete "%AGENT%" >nul 2>&1
      echo       service key lingers - deleted, takes effect after reboot
      set "REBOOT_NEEDED=1"
    )
  )
)

echo.
echo [4/5] agent data directory
if exist "%DATADIR%" (
  rd /s /q "%DATADIR%" 2>nul
  if exist "%DATADIR%" (
    echo       could not remove - delete it by hand
  ) else (
    echo       removed
  )
) else (
  echo       not present
)

echo.
echo [5/5] AutoRun
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\Explorer" /v NoDriveTypeAutoRun /t REG_DWORD /d 255 /f >nul 2>&1
if errorlevel 1 (
  echo       FAILED to set NoDriveTypeAutoRun
) else (
  echo       NoDriveTypeAutoRun=0xff
)

echo.
echo ============================================================
echo  Done. REBOOT now, then run xp-test-preflight.cmd again.
echo  It must print VERDICT: GO before any enforcement test.
echo  Keep list-storage-surface.vbs next to the preflight script,
echo  otherwise the "devices left disabled" check is skipped.
echo ============================================================
pause
exit /b 0
