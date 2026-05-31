# AugrixUpdater

Auto-updater for the [Augrix EA](https://augrix.io) MetaTrader 5 Expert Advisor. Downloads, verifies, and installs EA binary updates with zero user intervention.

## Features

- **Interactive TUI** — manual update, path management, API key setup
- **Watch mode** (`--watch`) — silent flag-file scanner, designed to run as a Windows Scheduled Task every 5 minutes
- **Graceful MT5 handling** — closes MetaTrader via WM_CLOSE, waits for clean shutdown, restarts after update
- **Backup & rollback** — old binary saved as `.bak`, restored on failure
- **Multi-terminal support** — auto-discovers all MT5 installations via AppData
- **Single-instance guard** — named mutex prevents concurrent watch executions
- **Secure credential storage** — API key in Windows Credential Manager, paths in Registry
- **Event reporting** — update status (started/completed/failed/rolled_back) sent to server

## How It Works

1. The EA detects a new version is available (via `/api/version`)
2. EA waits for safe conditions (no open positions, low volatility, idle trading window)
3. EA writes `augrix_update.json` flag file to its `MQL5/Files/` directory
4. Scheduled Task triggers `AugrixUpdater.exe --watch`
5. Updater finds the flag, downloads the new `.ex5` binary
6. Closes MT5 gracefully, swaps the binary (with backup), restarts MT5
7. EA reloads automatically on the new version

## Build

Requires Visual Studio 2022 Build Tools (or full VS) with C++ workload.

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

Output: `build/Release/AugrixUpdater.exe` (~400 KB, statically linked, no DLL dependencies)

### Cross-compile with MinGW (Linux)

```bash
cmake -B build -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
cmake --build build
```

> **Note:** MSVC builds are strongly recommended for distribution. MinGW builds may trigger Windows Defender false positives due to binary structure differences.

## Usage

### Interactive Mode

```
AugrixUpdater.exe
```

Presents a TUI menu:
1. Update the bots (download + install)
2. Add MT5 path
3. Remove MT5 path
4. Set API key
5. Auto-update setup (install/remove Scheduled Task)

### CLI Flags

| Flag | Description |
|------|-------------|
| `--watch` | Silent mode — scan for flag files, process updates, exit |
| `--install-task` | Register Windows Scheduled Task (5-min interval) |
| `--remove-task` | Remove the Scheduled Task |

## Architecture

```
┌─────────────┐     flag file      ┌──────────────────┐
│  Augrix EA  │ ──────────────────▶ │  AugrixUpdater   │
│  (MT5)      │                     │  (--watch mode)  │
└─────────────┘                     └────────┬─────────┘
                                             │
                                    ┌────────▼─────────┐
                                    │  augrix.io API   │
                                    │  /api/download/ea│
                                    └──────────────────┘
```

## Security

- API keys stored in Windows Credential Manager (encrypted at rest)
- Server communication over HTTPS only (WinHTTP with system cert store)
- Binary integrity verified via Ed25519 signature header
- Kill-switch: server can disable updates per-user or globally
- No admin rights required for normal operation (Scheduled Task uses current user)

## License

MIT — see [LICENSE](LICENSE)
