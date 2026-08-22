# TimeSync

TimeSync is a small Windows desktop utility for checking and synchronizing the
computer clock against verified NTP time sources.

It shows Beijing time beside the current Windows system time, reports the
difference between them, and can keep the clock synchronized on a schedule.

## Features

- Verified NTP time from configurable sources
- Beijing time and Windows system time shown together
- Manual system-time synchronization
- Optional recurring Task Scheduler synchronization
- IPv4 and IPv6 support
- Chinese and English interface
- Shared-runtime portable build and self-contained static build

## Install

1. Extract the release package to a folder that normal users cannot modify.
2. Start `TimeSync.exe`.
3. Approve the Windows administrator prompt.

TimeSync requests administrator permission when it starts because changing the
Windows clock and managing a scheduled task require elevated access. Installing
it in a protected folder also prevents a user-writable copy of the executable
from being used for privileged work.

> [!NOTE]
> Windows Defender or another security product may ask for confirmation when a
> new unsigned executable is first launched.

## Release packages

| Package | Use |
| --- | --- |
| `TimeSync-portable.zip` | Application folder with Qt runtime files |
| `TimeSync-static.exe` | Single executable with Qt and MinGW runtime linked in |

The static executable still uses normal Windows system libraries. Both packages
need network access to UDP port `123` for NTP.

## Use the application

- **Refresh** obtains a new NTP sample without changing the system clock.
- **Synchronize** obtains a fresh sample and sets the Windows system clock.
- **Edit servers** changes the NTP source order. A hostname, IPv4 address, or
  bracketed IPv6 address may be used; an optional port is supported.
- **Automatic time sync** creates or updates a recurring scheduled task.

The application keeps displaying the last verified reference time using a
monotonic timer while a refresh is not available. It does not treat the local
system clock as an authoritative time source.

## Configuration

The default configuration is `config.ini` beside `TimeSync.exe`. It contains
exactly three fields:

```ini
servers = ntp1.aliyun.com, ntp2.aliyun.com, ntp1.nim.ac.cn
scheduleIntervalMinutes = 60
language = zh-CN
```

The GUI is the recommended way to edit this file. Existing invalid files are
never silently overwritten.

## Command line

```text
TimeSync.exe
TimeSync.exe --sync-once [--dry-run] [--config <path>]
TimeSync.exe --install-task [--config <path>]
TimeSync.exe --remove-task [--config <path>]
TimeSync.exe --help
```

`--dry-run` obtains and prints an NTP sample without changing the system clock.
The command-line modes also request administrator permission through the
application manifest.

## Build from source

Requirements:

- Windows x64
- Qt 6.9.1 MinGW 64-bit
- MinGW-w64 13.1
- CMake 3.22 or newer
- Ninja

With Qt, CMake, Ninja, and MinGW available on `PATH`, configure and build the
shared-Qt version:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=<shared-Qt-prefix> \
  -DCMAKE_CXX_COMPILER=<path-to-g++.exe> \
  -DCMAKE_MAKE_PROGRAM=<path-to-ninja.exe>
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cmake --build build --target deploy-portable
```

To build the single-executable variant, configure the project with a separately
compiled static Qt 6.9.1 `qtbase` installation:

```sh
cmake -S . -B build-static -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=<static-Qt-prefix> \
  -DCMAKE_CXX_COMPILER=<path-to-g++.exe> \
  -DCMAKE_MAKE_PROGRAM=<path-to-ninja.exe>
cmake --build build-static --parallel 4
```

The static Qt build must include the Windows platform plugin. Schannel is used
for Qt's TLS support; OpenSSL is not required by TimeSync's NTP workflow.

## Troubleshooting

- If no time source responds, allow outbound UDP `123` and check the server
  list.
- If Windows rejects the clock change, confirm that the administrator prompt
  was accepted and that local policy allows changing system time.
- If a scheduled task cannot be created, run the application from its protected
  installation folder and try again.

NTP is a plain UDP protocol. Origin-timestamp and packet validation protect
against malformed or unrelated replies, but NTP itself does not provide
cryptographic authenticity.
