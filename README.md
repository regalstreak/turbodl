<p align="center"><img src="assets_src/icon_1024.png" width="160" alt="TurboDL"></p>

# TurboDL

A multi-connection download manager for jailbroken PS4 (GoldHEN). It downloads files with up to 32 parallel connections, keeps going in the background while you play, and can install `.pkg` files automatically when they finish.

> **TurboDL is a generic HTTP download manager. It ships no content, links, catalogs or site-specific credentials.** Only download content you have the right to.

## Features

- **Fast**: aria2-style segmented downloads. A file is split into 16 MB segments fetched over 1–32 connections (default 12) using HTTP Range requests, with a single-connection fallback for servers without Range support.
- **Resumable**: per-segment byte progress is saved every 3 s, so downloads survive pause, closing the app, crashes and reboots.
- **Background service**: the engine runs as a daemon (`PKDL00002`) that keeps downloading while games are running.
- **Auto-install**: finished `.pkg` files are installed through the system installer (BGFT). Optionally the pkg is deleted afterwards.
- **Web panel** at `http://<ps4-ip>:8080` for your phone or PC:
  - add links (several at once)
  - pause, resume, retry and remove
  - set per-site cookies and other settings
  - browse your own catalog files
- **TV UI**: progress, speed and ETA, controlled with the gamepad.
- **Progress notifications**: PS4 popups every N % (configurable, default 10 %, 0 = off).
- **Per-site cookies** for downloads that need a login. A cookie is only sent to its own domain and subdomains, and survives redirects to CDN subdomains.
- **Catalog browser**: reads list files in FPKGi JSON format that *you* place on the console.

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
| L1 / R1 | fewer / more connections |
| ↑ / ↓ | select |

### Cookies for login-protected downloads

In the web panel under **Settings**:
1. Enter the site's domain (e.g. `example.com`).
2. Paste the `Cookie:` request header copied from your browser's dev tools.

The cookie is sent only to that domain and its subdomains. Leave the cookie empty to remove a site. Cookies are stored in `/user/data/pkgdl/cookies.txt`, one `domain<TAB>cookie` per line.

### Catalog

The catalog tab lists entries from JSON files in the FPKGi format (`{"DATA": {"<url>": {"name": ..., "title_id": ..., "size": ...}}}`). TurboDL looks in:

- `/user/data/pkgdl/catalogs/GAMES.json` (also `UPDATES`, `DLC`, `DEMOS`, `APPS`, `HOMEBREW`, `PS1`, `PS2`, `PSP`)
- `/user/data/FPKGi/ContentJSONs/<TYPE>.json`, if you already use FPKGi

No lists are included.

### Files on the console

```
/user/data/pkgdl/
  downloads/      downloaded files (+ .dlstate resume files)
  config.ini      connections, auto_install, delete_after_install, replace_installed, notify_progress_step
  cookies.txt     per-site cookies
  queue.txt       queue
  log.txt         log (also at http://<ps4-ip>:8080/log)
/system/vsh/app/PKDL00002/   background service (installed by the app)
```

## How it works

```
 PKDL00001 (app, TV UI) ──HTTP 127.0.0.1:8080──▶ PKDL00002 (daemon)
   installs/updates/starts the daemon               download engine (SceHttp + pthreads)
   mirrors its state on screen                      BGFT installer
                                                    web panel + JSON API
```

- **HTTP**: the PS4's own `libSceHttp`/`libSceSsl`, no curl port. Redirects are followed manually so that per-site cookies are re-evaluated on every hop.
- **Install**: `sceBgftServiceIntDownloadRegisterTaskByStorageEx` with the local file path, falling back to the daemon's own HTTP server over loopback.
- **Daemon**: the app remounts `/system` read-write, copies `daemon.bin` to `/system/vsh/app/PKDL00002/eboot.bin` along with a stock daemon `param.sfo`, then launches it with `sceLncUtilLaunchApp`. This is the same technique Itemzflow uses. Its process registers with `sceSystemServiceRegisterDaemon` and keeps running while games are open.
- **TV UI**: raw `sceVideoOut` framebuffers and FreeType text with a glyph cache. SDL's PS4 port fails to allocate framebuffers in this setup, so it isn't used.

### JSON API (daemon, port 8080)

| Method | Path | Params |
|---|---|---|
| GET | `/api/jobs`, `/api/jobs.tsv` | – |
| GET | `/api/info`, `/api/logtail`, `/log` | – |
| POST | `/api/add` | `urls` (newline-separated), `name`, `auto` |
| POST | `/api/job` | `id`, `op` = `pause`/`resume`/`retry`/`install`/`delete` |
| POST | `/api/cookie` | `domain`, `cookie` (empty = remove) |
| POST | `/api/settings` | `conns`, `auto`, `del`, `replace`, `notify` |
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

- GoldHEN's klog: `nc <ps4-ip> 3232`. Crash reports there include the faulting address and any missing system-module function (NID).
- Log file: `http://<ps4-ip>:8080/log` or `/user/data/pkgdl/log.txt` over FTP (GoldHEN FTP on port 2121).

## Credits

- [OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) – toolchain and samples
- [GoldHEN](https://github.com/GoldHEN) – homebrew enabler and plugin SDK (jailbreak syscall)
- flatz – Remote Package Installer (BGFT usage)
- LightningMods – PS4-Store / Itemzflow (BGFT local install flow, daemon technique)

## License

MIT. See [LICENSE](LICENSE).
