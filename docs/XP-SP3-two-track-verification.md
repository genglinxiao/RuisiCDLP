# XP SP3 x86 USB control tester packages

Build date: 2026-10-18. These are mutually exclusive, isolated-lab builds, **not production-qualified**.

| Track | ZIP | SHA-256 |
| --- | --- | --- |
| SetupAPI `DIF_PROPERTYCHANGE` | `RuisiUsbDlp-XP-SP3-x86-SetupAPI-1.1.6-TEST.zip` | `7b9926d904e36745ff16a4cf74968673d067445a51b2d02999384c7d930fc56c` |
| USBSTOR disk-class filter | `RuisiUsbDlp-XP-SP3-x86-Filter-1.1.7-TEST.zip` | `c2dfc275b653c0c66ad158899295e13259c34df2219f8f85cfaa95ae512d4c70` |

Both archives are in `artifacts/legacy-kits/`; each has a sibling `.zip.sha256` file and an internal `SHA256SUMS`. Each includes a separate agent MSI, the XP server MSI, real-mode installation and rollback scripts, editable configuration, English instructions and a Chinese handoff. The filter ZIP additionally includes `RuisiUsbDlpFilter.sys` and its XP-native setup helper. Testers must replace the test enrollment token and administrator password before sharing outside the isolated lab.

## Build-host verification

- `scripts/verify-xp-test-tracks.ps1` passed: every unpacked file and ZIP entry matched `SHA256SUMS` (24 SetupAPI files; 26 filter files), and both outer ZIP hashes matched their `.sha256` files.
- MSI tables identify agent versions 1.1.6 and 1.1.7, the shared XP agent upgrade code, the agent and notifier payloads, and server version 1.1.2.
- Both agent executables, the filter setup helper and the driver are x86 PE files with subsystem version 5.01 and no CLR header.
- Native policy and protected-store tests passed 2/2 for each agent build.
- The filter driver compiled and linked against extracted WDK 7.1 XP-era components without running the WDK 7.1 installer. Linker warning LNK4078 about `INIT` section attributes remains and must be reviewed before production qualification.

## Not yet verified

No accessible XP USB-pass-through VM was available on the build host, so neither archive has passed an XP boot, physical USB insertion, multi-LUN I/O, approval/revocation, rollback, or uninstaller test. The kernel driver and all binaries are unsigned. The filter can affect disk-class startup if it fails to load; use only a recoverable XP SP3 x86 NTFS snapshot. The server uses isolated-lab HTTP, not production transport. Gates F10-F12 in the test plan remain open until testers return logs and measured file-I/O results. A popup or `blocked_verified` log line alone is not evidence that all volumes were denied.

Reference implementation guidance: [Microsoft filter installation](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/installing-a-filter-driver), [AddDevice skip behavior](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/guidelines-for-writing-adddevice-routines), and [WDK 7.1 legacy download warning](https://learn.microsoft.com/en-us/windows-hardware/drivers/legacy-wdk-downloads).
