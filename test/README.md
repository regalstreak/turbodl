# Host tests

`test/run.sh` builds the download engine and web panel for the host (macOS/Linux) with a mock
HTTP layer (`mock_http.c`, plain sockets instead of the PS4's SceHttp) and runs it against
`server.py`, a fake file host that redirects, requires a login cookie, supports Range and
randomly drops connections mid-transfer.

It checks: helpful 401 error, per-site cookie, pause/resume, crash/restart resume,
byte-identical output and no sanitizer errors.

```
./test/run.sh
```
