@echo off
rem ---------------------------------------------------------------------------
rem Ruisi USB DLP - XP boot safety check (read only, no changes)
rem Exit 0 = SAFE to reboot / exit 1 = NOT SAFE / exit 2 = filter not installed
rem ---------------------------------------------------------------------------
setlocal EnableExtensions
set "SVC=RuisiUsbDlpFilter"
set "SVCKEY=HKLM\SYSTEM\CurrentControlSet\Services\%SVC%"
set "CLASSKEY=HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e967-e325-11ce-bfc1-08002be10318}"
set "DRV=%SystemRoot%\System32\drivers\RuisiUsbDlpFilter.sys"
set "BAD="

sc query "%SVC%" >nul 2>&1
if errorlevel 1 goto :not_installed

if exist "%DRV%" (echo driver_file=present) else (echo driver_file=missing & set "BAD=1")

set "START_VAL="
for /f "tokens=3" %%A in ('reg query "%SVCKEY%" /v Start 2^>nul ^| find /i "Start"') do set "START_VAL=%%A"
set "GROUP_VAL="
for /f "tokens=2,*" %%A in ('reg query "%SVCKEY%" /v Group 2^>nul ^| find /i "Group"') do set "GROUP_VAL=%%B"
echo service_start=%START_VAL%
echo service_group=%GROUP_VAL%
if /i not "%START_VAL%"=="0x0" set "BAD=1"
if /i not "%GROUP_VAL%"=="System Bus Extender" set "BAD=1"

reg query "%CLASSKEY%" /v UpperFilters 2>nul | find /i "%SVC%" >nul
if errorlevel 1 (echo disk_upper_filter=missing & set "BAD=1") else (echo disk_upper_filter=present)

echo.
if defined BAD (
  echo BOOT_SAFETY=NOT_SAFE  ^<-- this machine will STOP 0x7B on the next boot
  echo Run fix-xp-boot.cmd as Administrator before restarting.
  endlocal
  exit /b 1
)
echo BOOT_SAFETY=SAFE  service=BOOT_START  group=System Bus Extender
endlocal
exit /b 0

:not_installed
echo service=%SVC% not installed - nothing to check
endlocal
exit /b 2
