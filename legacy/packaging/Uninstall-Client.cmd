@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem Uninstall order matters:
rem   1. stop the service so it cannot re-disable a device while we re-enable it
rem   2. --restore-owned  re-enables every devnode this agent disabled
rem   3. --restore-backstop puts the Device Installation Restrictions values back
rem      to what they were before the agent published a baseline
rem   4. remove the MSI
rem Step 3 must run before the MSI is removed, because the before-image lives in
rem the agent data directory and the uninstaller does not touch it by default.

net session >nul 2>&1
if errorlevel 1 (
  echo This uninstaller must be run as Administrator.
  exit /b 5
)

set "KIT=%~dp0"
set "MSI="
for %%F in ("%KIT%*.msi") do if not defined MSI set "MSI=%%~fF"
if not defined MSI (
  echo Missing RuisiUsbDlpAgent profile MSI in %KIT%
  exit /b 2
)

if exist "%ProgramFiles(x86)%" (
  if exist "%ProgramFiles%\Ruisi USB DLP Legacy\UsbDlp.LegacyAgent.exe" (
    set "AGENT=%ProgramFiles%\Ruisi USB DLP Legacy\UsbDlp.LegacyAgent.exe"
  ) else (
    set "AGENT=%ProgramFiles(x86)%\Ruisi USB DLP Legacy\UsbDlp.LegacyAgent.exe"
  )
) else (
  set "AGENT=%ProgramFiles%\Ruisi USB DLP Legacy\UsbDlp.LegacyAgent.exe"
)

sc.exe stop UsbDlp.LegacyAgent >nul 2>&1
ping.exe -n 3 127.0.0.1 >nul

if not exist "%AGENT%" (
  echo Agent binary not found; skipping device and policy restoration.
  goto remove_msi
)

echo Re-enabling devices this agent disabled.
"%AGENT%" --restore-owned
if errorlevel 1 echo restore_owned reported a failure; check the devices listed above.

echo Restoring Device Installation Restrictions.
"%AGENT%" --restore-backstop
if errorlevel 1 (
  echo The agent could not restore every policy value it wrote.
  echo Values changed by someone else are deliberately left in place.
  echo Review HKLM\SOFTWARE\Policies\Microsoft\Windows\DeviceInstall\Restrictions
  echo before removing the product from this workstation.
)

:remove_msi
msiexec.exe /x "%MSI%" /qn /norestart
if errorlevel 1 (
  echo Agent MSI removal failed.
  exit /b 4
)

rem State under the agent data directory is retained unless the operator removes
rem it explicitly. Say so rather than implying a clean machine.
echo.
echo Uninstalled. The agent data directory was kept:
echo   %%ProgramData%%\UsbDlp
echo Delete it manually only after confirming no device needs re-enabling.
exit /b 0
