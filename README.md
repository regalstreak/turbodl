<p align="center"><img src="assets_src/icon_1024.png" width="150" alt="TurboDL"></p>

<h1 align="center">TurboDL</h1>

<p align="center">A multi-connection, resumable download manager and installer for jailbroken PS4 (GoldHEN).<br>
It downloads with up to 32 connections per file, keeps going in the background while you play, and installs <code>.pkg</code> files automatically when they finish.</p>

<p align="center">
  <a href="https://github.com/regalstreak/turbodl/releases/latest"><img alt="Release" src="https://img.shields.io/github/v/release/regalstreak/turbodl?label=Download&style=for-the-badge"></a>
  <img alt="License" src="https://img.shields.io/badge/license-MIT-blue?style=for-the-badge">
</p>

## Download

**➡ [Latest release](https://github.com/regalstreak/turbodl/releases/latest)** — download `TurboDL-<version>.pkg` and install it with GoldHEN's package installer (from USB, `/data/pkg`, or any remote pkg installer).

Direct link to the newest build:
`https://github.com/regalstreak/turbodl/releases/latest/download/TurboDL-1.0.0.pkg`

Requires **GoldHEN** (tested on firmware 12.02).

## Screenshots

| TV — downloads | TV — catalog (search on the console) |
|---|---|
| ![TV downloads](docs/screenshots/tv-downloads.png) | ![TV catalog](docs/screenshots/tv-catalog.png) |

| Web panel — desktop | Web panel — mobile |
|---|---|
| ![Web panel](docs/screenshots/web-desktop.png) | <img src="docs/screenshots/web-mobile.png" width="290" alt="Web panel on mobile"> |

![Web panel catalog](docs/screenshots/web-catalog.png)

*(Rendered from the real UI code with `make shots`.)*

> **TurboDL is a generic HTTP download manager. It ships no content, links, catalogs or site-specific credentials.** Only download content you have the right to.

## Features

- **Fast**: aria2-style segmented downloads. A file is split into 16 MB segments fetched over 1–32 connections (default 12) using HTTP Range requests, with a single-connection fallback for servers without Range support.
- **Parallel**: several downloads at once (default 4, up to 8). Each one uses its own set of connections, and the rest wait in the queue.
- **Resumable**: per-segment byte progress is saved every 3 s, so downloads survive pause, closing the app, crashes and reboots.
- **Background service**: the engine runs as a daemon (`TRBD00002`) that keeps downloading while games are running.
- **Auto-install**: finished `.pkg` files are installed through the system installer (BGFT). Optionally the pkg is deleted afterwards.
- **Web panel** at `http://<ps4-ip>:8080` for your phone or PC:
  - add links (several at once)
  - pause, resume, retry and remove
  - set per-site cookies and other settings
  - browse your own catalog files (whole list, loads as you scroll, searchable)
- **TV UI**: progress, speed and ETA, controlled with the gamepad.
- **Progress notifications**: PS4 popups every N % (default 50 %, so at 50 % and when it finishes; 0 turns them off).
- **Per-site cookies** for downloads that need a login. A cookie is only sent to its own domain and subdomains, and survives redirects to CDN subdomains.
- **Catalog browser** on the TV and in the web panel: reads list files in FPKGi JSON format that *you* place on the console, with search on both.

## Requirements

- PS4 with **GoldHEN** (tested on firmware 12.02). TurboDL asks GoldHEN for a jailbreak at startup.
- The console on your LAN, if you want to use the web panel.

## Install & use

1. Install `IV0000-PKDL00001_00-PKGDOWNLOADER000.pkg`. Use GoldHEN's package installer from `/data/pkg` or USB, or any remote pkg installer.
2. Open **TurboDL**. On first launch it installs and starts the background service. The top line of the TV UI says **BACKGROUND MODE** when that worked. If the service can't start, TurboDL runs the engine inside the app instead ("in-app mode"), and you need to keep the app open while it downloads.
3. Open `http://<ps4-ip>:8080` on your phone or PC and paste links.

After a reboot, open TurboDL once to start the background service again.

### Controller (TV UI)

| Button | Action |
|---|---|
| ✕ | pause / resume / retry |
| △ | install the selected (finished) pkg |
| □ ×2 | remove item and delete its file |
| L1 / R1 | fewer / more connections per download |
| L2 / R2 | fewer / more parallel downloads |
| OPTIONS | switch between the download list and the catalog |
| SQUARE (catalog) | search the catalog with the on-screen keyboard |
| TRIANGLE (catalog) | clear the search filter |
| L3 (catalog) | reload the list from disk |
| ↑ / ↓ | select |

### Cookies for login-protected downloads

In the web panel under **Settings**:
1. Enter the site's domain (e.g. `example.com`).
2. Paste the `Cookie:` request header copied from your browser's dev tools.

The cookie is sent only to that domain and its subdomains. Leave the cookie empty to remove a site. Cookies are stored in `/user/data/turbodl/cookies.txt`, one `domain<TAB>cookie` per line.

### Catalog

The catalog tab lists entries from JSON files in the FPKGi format (`{"DATA": {"<url>": {"name": ..., "title_id": ..., "size": ...}}}`). TurboDL looks in:

- `/user/data/turbodl/catalogs/GAMES.json` (also `UPDATES`, `DLC`, `DEMOS`, `APPS`, `HOMEBREW`, `PS1`, `PS2`, `PSP`)
- `/user/data/FPKGi/ContentJSONs/<TYPE>.json`, if you already use FPKGi

No lists are included.

### Upgrading from an older build

The data directory used to be `/user/data/pkgdl`; it is now `/user/data/turbodl`. Move it once by hand if you are upgrading (the app does not touch it):

```sh
# over the GoldHEN FTP server (port 2121)
curl -Q "RNFR /user/data/pkgdl/config.ini" -Q "RNTO /user/data/turbodl/config.ini" ftp://<ps4-ip>:2121/
curl -Q "RNFR /user/data/pkgdl/downloads/<file>" -Q "RNTO /user/data/turbodl/downloads/<file>" ftp://<ps4-ip>:2121/
```

The app keeps its internal id `PKDL00001` so upgrades install over the previous version instead of creating a second app. The background service was renamed `PKDL00002` -> `TRBD00002`; the old `/system/vsh/app/PKDL00002` folder is left in place (unused) and can be deleted over FTP after a reboot.

### Files on the console

```
/user/data/turbodl/
  downloads/      downloaded files (+ .dlstate resume files)
  config.ini      connections, max_parallel_downloads, auto_install, delete_after_install,
                  replace_installed, notify_progress_step
  cookies.txt     per-site cookies
  queue.txt       queue
  log.txt         log (also at http://<ps4-ip>:8080/log)
/system/vsh/app/TRBD00002/   background service (installed by the app)
```

## How it works

```
 PKDL00001 (app, TV UI) ──HTTP 127.0.0.1:8080──▶ TRBD00002 (daemon)
   installs/updates/starts the daemon               download engine (SceHttp + pthreads)
   mirrors its state on screen                      BGFT installer
                                                    web panel + JSON API
```

- **HTTP**: the PS4's own `libSceHttp`/`libSceSsl`, no curl port. Redirects are followed manually so that per-site cookies are re-evaluated on every hop.
- **Install**: `sceBgftServiceIntDownloadRegisterTaskByStorageEx` with the local file path, falling back to the daemon's own HTTP server over loopback.
- **Daemon**: the app remounts `/system` read-write, copies `daemon.bin` to `/system/vsh/app/TRBD00002/eboot.bin` along with a stock daemon `param.sfo`, then launches it with `sceLncUtilLaunchApp`. This is the same technique Itemzflow uses. Its process registers with `sceSystemServiceRegisterDaemon` and keeps running while games are open.
- **TV UI**: raw `sceVideoOut` framebuffers and FreeType text with a glyph cache. SDL's PS4 port fails to allocate framebuffers in this setup, so it isn't used.

### JSON API (daemon, port 8080)

| Method | Path | Params |
|---|---|---|
| GET | `/api/jobs`, `/api/jobs.tsv` | – |
| GET | `/api/info`, `/api/logtail`, `/log` | – |
| POST | `/api/add` | `urls` (newline-separated), `name`, `auto` |
| POST | `/api/job` | `id`, `op` = `pause`/`resume`/`retry`/`install`/`delete` |
| POST | `/api/cookie` | `domain`, `cookie` (empty = remove) |
| POST | `/api/settings` | `conns`, `parallel`, `auto`, `del`, `replace`, `notify` |
| POST | `/api/install_path` | `path` of a pkg already on the console |
| POST | `/api/launch`, `/api/kill` | `tid` (title id) |
| POST | `/api/quit` | stop the daemon (queue is resumed on next start) |
| GET | `/files/<id>` | serve a downloaded file (Range supported) |

## Building

Requirements:

- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases) v0.5.4+, the release tarball (not the git repo)
- LLVM `clang` + `ld.lld` (macOS: `brew install llvm lld`)
- `xxd`
- On Apple Silicon, Rosetta (the SDK host tools are x86_64)

```sh
export OO_PS4_TOOLCHAIN=/path/to/PS4Toolchain
make            # -> IV0000-PKDL00001_00-PKGDOWNLOADER000.pkg
./test/run.sh   # host regression tests (no PS4 needed)
```

## Debugging

- The service writes boot breadcrumbs (`[turbodl] boot: ...`) to both the log file and the kernel log, so `nc <ps4-ip> 3232` shows how far it got, alongside crash reports with the faulting address and any missing system-module function (NID).
- Log file: `http://<ps4-ip>:8080/log` or `/user/data/turbodl/log.txt` over FTP (GoldHEN FTP on port 2121).

## Credits

- [OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) – toolchain and samples
- [GoldHEN](https://github.com/GoldHEN) – homebrew enabler and plugin SDK (jailbreak syscall)
- flatz – Remote Package Installer (BGFT usage)
- LightningMods – PS4-Store / Itemzflow (BGFT local install flow, daemon technique)

## License

MIT. See [LICENSE](LICENSE).
