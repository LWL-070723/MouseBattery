# Security Policy

## Supported Versions

| Version | Supported |
| --- | --- |
| `main` | ✅ |

## Reporting a Vulnerability

Please **do not open a public issue** for security problems.

Use GitHub's private vulnerability reporting:

https://github.com/LWL-070723/MouseBattery/security/advisories/new

Please include:

- affected version / commit;
- Windows version;
- steps to reproduce;
- impact and any suggested fix.

## Scope

MouseBattery is a local Windows tray application. It:

- does **not** use the network;
- does **not** install a service or kernel driver;
- does **not** request administrator privileges;
- reads a vendor HID battery/status report from the mouse receiver;
- writes only `%LOCALAPPDATA%\MouseBattery\` (log + battery cache);
- optionally writes a per-user autostart value under
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.

The HID query is read-only (`0x30`); the app does not change DPI, buttons,
lighting, or firmware.

## Prebuilt Binary

`dist/MouseBattery.exe` is **not Authenticode-signed**. Windows SmartScreen may
show "Unknown publisher". To verify the binary:

```powershell
Get-FileHash .\dist\MouseBattery.exe -Algorithm SHA256
```

Compare the result with [`SHA256SUMS.txt`](SHA256SUMS.txt), or build from source
with `build.bat`.

## Out of Scope

- attacks that require administrator/root or physical access to an already
  compromised machine;
- vulnerabilities in third-party mouse firmware or vendor software;
- the fact that a public repository can be forked or archived by third parties.

## Security Design Notes

- No network imports or telemetry.
- All HID report parsing is bounds-checked before indexing.
- Device/product strings are only displayed/logged, never used as format
  strings or executed.
- The process runs as the current user and uses a per-user mutex/single-instance
  check.
