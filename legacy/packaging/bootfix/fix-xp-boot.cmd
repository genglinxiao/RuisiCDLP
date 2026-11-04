@echo off
rem ---------------------------------------------------------------------------
rem Ruisi USB DLP - XP boot safety fix (HF1)
rem
rem Symptom : after installing the 1.1.8 disk-class filter the machine stops with
rem           STOP 0x7B INACCESSIBLE_BOOT_DEVICE (0xC0000034) on the next boot.
rem Cause   : the driver service is created with Start=3 (DEMAND_START) and no
rem           load-order Group, while the driver is already listed in the disk
rem           class UpperFilters value. The boot volume stack therefore cannot be
rem           built.
rem Fix     : Start=0 (BOOT_START) + Group="System Bus Extender" (the group PartMgr
rem           belongs to). Setting only Start=0 fixes the cold boot but the warm
rem           reboot still falls through to PXE, so both values are mandatory.
rem
rem Run as Administrator ON THE AFFECTED MACHINE BEFORE IT IS RESTARTED, then
rem reboot and run verify-xp-boot.cmd.
rem
rem Exit codes: 0 ok / 5 not administrator / 3 service missing / 4 driver file
rem             missing / 6 configuration rejected / 7 read-back verification failed
rem ---------------------------------------------------------------------------
setlocal EnableExtensions
set "SVC=RuisiUsbDlpFilter"
set "SVCKEY=HKLM\SYSTEM\CurrentControlSet\Services\%SVC%"
set "CLASSKEY=HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e967-e325-11ce-bfc1-08002be10318}"
set "DRV=%SystemRoot%\System32\drivers\RuisiUsbDlpFilter.sys"
set "FAIL="

net session >nul 2>&1
if errorlevel 1 goto :no_admin

echo [1/4] Checking the installed filter
sc query "%SVC%" >nul 2>&1
if errorlevel 1 goto :no_service
if not exist "%DRV%" goto :no_driver
echo       service present, driver file present

echo [2/4] Forcing Start=BOOT and Group="System Bus Extender"
sc config "%SVC%" start= boot >nul 2>&1
if errorlevel 1 set "FAIL=1"
reg add "%SVCKEY%" /v Group /t REG_SZ /d "System Bus Extender" /f >nul 2>&1
if errorlevel 1 set "FAIL=1"
if defined FAIL goto :config_rejected

echo [3/4] Verifying the service configuration by read-back
set "START_VAL="
for /f "tokens=3" %%A in ('reg query "%SVCKEY%" /v Start 2^>nul ^| find /i "Start"') do set "START_VAL=%%A"
set "GROUP_VAL="
for /f "tokens=2,*" %%A in ('reg query "%SVCKEY%" /v Group 2^>nul ^| find /i "Group"') do set "GROUP_VAL=%%B"
echo       Start = %START_VAL%
echo       Group = %GROUP_VAL%
if /i not "%START_VAL%"=="0x0" goto :verify_failed
if /i not "%GROUP_VAL%"=="System Bus Extender" goto :verify_failed

echo [4/4] Checking the disk class UpperFilters value
reg query "%CLASSKEY%" /v UpperFilters 2>nul | find /i "%SVC%" >nul
if not errorlevel 1 goto :filters_ok
reg query "%CLASSKEY%" /v UpperFilters 2>nul | find /i "PartMgr" >nul
if errorlevel 1 goto :filters_unknown
echo       UpperFilters lost the filter entry; restoring PartMgr + %SVC%
reg add "%CLASSKEY%" /v UpperFilters /t REG_MULTI_SZ /d "PartMgr\0RuisiUsbDlpFilter" /f >nul 2>&1
if errorlevel 1 goto :verify_failed
reg query "%CLASSKEY%" /v UpperFilters 2>nul | find /i "%SVC%" >nul
if errorlevel 1 goto :verify_failed
echo       UpperFilters restored
goto :done

:filters_ok
echo       UpperFilters already lists the filter
goto :done

:done
echo.
echo REPAIR_OK  service=BOOT_START  group=System Bus Extender  filter=registered
echo Reboot this machine now, then run verify-xp-boot.cmd.
echo Do not insert any USB disk until verify reports SAFE.
endlocal
exit /b 0

:no_admin
echo ERROR: run this script as a local Administrator.
endlocal
exit /b 5

:no_service
echo ERROR: service %SVC% is not installed on this machine.
echo Nothing to repair. Do not create it by hand.
endlocal
exit /b 3

:no_driver
echo ERROR: %DRV% is missing while the filter service exists.
echo Run xp-filter-setup.exe uninstall before rebooting this machine.
endlocal
exit /b 4

:config_rejected
echo ERROR: Service Control Manager rejected the configuration change.
echo Do not reboot. Capture this output and escalate.
endlocal
exit /b 6

:verify_failed
echo ERROR: read-back verification failed (Start=%START_VAL% Group=%GROUP_VAL%).
echo Do not reboot this machine. Run xp-filter-setup.exe uninstall instead.
endlocal
exit /b 7

:filters_unknown
echo ERROR: UpperFilters does not contain the filter nor PartMgr.
echo Inspect the value by hand before rebooting.
endlocal
exit /b 7
