# TimeSync

<img src="resources/branding/lec-logo.png" alt="LEC Software Studio logo" width="128">

English | [简体中文](README.zh-CN.md)

TimeSync is a Windows desktop app that shows Beijing Standard Time (UTC+08:00) beside the Windows clock and sets the system time immediately or on a schedule.

![Standard Time Sync English interface showing Beijing Standard Time, Windows system time, their difference, schedule controls, and time servers](docs/screenshot.jpg)

Version **1.0.0** · Published by **LEC Software Studio / 乐程软件工作室**

## Run on Windows

Choose the package you received:

| Package | How to launch |
| --- | --- |
| `TimeSync-portable.zip` | Extract the entire application folder, keeping its Qt runtime files together, then open `TimeSync.exe`. |
| `TimeSync-static.exe` | Place the single executable in your application folder and open it. |

Use a folder protected from modification by unprivileged users: the executable runs with elevated permissions, including during scheduled synchronization.

Accept the Windows User Account Control (UAC) prompt at startup. Administrator access is required to adjust the system clock and manage the scheduled task.

Allow outbound **UDP port 123** for NTP queries. TimeSync supports both IPv4 and IPv6 time servers.

The initial interface language is Simplified Chinese. Use the **界面语言 / Language** selector to choose **English** or **简体中文**. The window is titled **Standard Time Sync** in English and **标准时间同步** in Chinese.

## Check and synchronize the clock

1. Read the Beijing Standard Time and Windows system time panels, with the clock difference displayed below them.
2. Select **Refresh** to request a new NTP sample while leaving the Windows clock unchanged.
3. Select **Sync system time now** to set the Windows clock using the verified standard time. TimeSync uses the current sample while it remains valid; otherwise, it queries NTP before setting the clock.

Open **Edit servers** to change the time sources and save the list. The defaults contain **25 servers** from Aliyun, NIM, and CERNET.

NTP over UDP provides no cryptographic assurance of a server's authenticity. Sample verification includes origin-timestamp checks that associate replies with requests.

## Set up automatic synchronization

1. In **Automatic time sync**, select **Enable automatic time sync**.
2. Enter a **Sync interval** between **1 and 10080 minutes**; the default is **60 minutes**.
3. Select **Apply** to create or update the Windows Task Scheduler task, then check **Schedule status**.

To delete the task, select **Remove schedule** and confirm removal.

The task launches the same TimeSync executable used to configure it, under the current administrative user with highest privileges. Keep that executable in its installed folder so the task can continue to launch it.

## Settings file

Use the GUI to change servers, scheduling, and language. Settings are stored in `config.ini` alongside the executable.

The file format is UTF-8 with exactly three fields, one per line, without comments or section headers. This example uses three sample servers:

```ini
servers = ntp1.aliyun.com, ntp2.aliyun.com, ntp1.nim.ac.cn
scheduleIntervalMinutes = 60
language = zh-CN
```

| Field | Accepted value |
| --- | --- |
| `servers` | Comma-separated NTP server list, managed through **Edit servers**. |
| `scheduleIntervalMinutes` | Integer from `1` to `10080`, in minutes; defaults to `60`. |
| `language` | `zh-CN` for Simplified Chinese (default) or `en-US` for English. |

If `config.ini` is invalid, TimeSync keeps the file and reports an error. Edit it to match the format above, then retry.

## Command-line use

Run these commands from the application folder; brackets indicate optional arguments:

```text
TimeSync.exe
TimeSync.exe --sync-once [--dry-run] [--config <path>]
TimeSync.exe --install-task [--config <path>]
TimeSync.exe --remove-task [--config <path>]
TimeSync.exe --help
```

- Launching without arguments opens the GUI.
- `--sync-once` performs one synchronization; adding `--dry-run` fetches and prints an NTP sample while leaving the system clock unchanged.
- `--install-task` creates or updates the scheduled task using the configured interval.
- `--remove-task` deletes the scheduled task.
- `--config <path>` selects a configuration file for the operation.
- `--help` displays usage information.

## Compile and package

Build requirements: **Windows x64**, **Qt 6.9.1 MinGW 64-bit** with Core, Network, and Widgets, **MinGW-w64** with **C++20** support, **CMake 3.22+**, and **Ninja**.

Run the commands from the source directory, replacing the angle-bracket placeholders with your Qt prefix and tool paths. The examples use shell-style `\` line continuation; in PowerShell or Command Prompt, join each continued command onto one line and remove the trailing `\` characters. Quote paths containing spaces.

### Shared Qt application folder

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=<shared-Qt-prefix> \
  -DCMAKE_CXX_COMPILER=<path-to-g++.exe> \
  -DCMAKE_MAKE_PROGRAM=<path-to-ninja.exe>
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --build build --target deploy-portable
```

When TimeSync is built against shared Qt, `deploy-portable` copies `TimeSync.exe` and the Qt runtime into `build/deploy`.

### Static single executable

Use a separately built static Qt 6.9.1 `qtbase` installation containing the Windows platform plugin (`QWindowsIntegrationPlugin`) and Schannel TLS plugin (`QSchannelBackendPlugin`).

```sh
cmake -S . -B build-static -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=<static-Qt-prefix> \
  -DCMAKE_CXX_COMPILER=<path-to-g++.exe> \
  -DCMAKE_MAKE_PROGRAM=<path-to-ninja.exe>
cmake --build build-static --parallel
```

## Resolve common problems

| Problem | Action |
| --- | --- |
| All time sources fail to respond | Check outbound UDP port `123` access and review the configured servers in **Edit servers**. |
| Windows refuses a clock adjustment | Reopen the application and accept UAC. Ask an administrator to check the **Change the system time** user right in Local Security Policy. |
| Scheduled synchronization fails | Launch the application from its installed folder with administrator privileges and apply the schedule again. |

## Publisher and license

The **About** dialog identifies LEC Software Studio (乐程软件工作室) and links to the [studio website](https://lec-page-2026.ziroo.cn/) and [GitHub organization](https://github.com/lec-org).

TimeSync is licensed under the [GNU Affero General Public License v3.0](LICENSE).
