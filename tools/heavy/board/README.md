# heavyboard

heavyboard shows PRACOVNA's heavy slot ([../README.md](../README.md)) on kovarex.com: the job
running now, the queue, and the finished jobs. It is a CGI program, and the page is public.
PRACOVNA posts every change to it, with a password (kovarex, 2026-11-23).

- `GET index.cgi`: the page. Its script reads `?json` every 5 seconds and counts running times in
  the browser, in the viewer's own time zone.
- `GET index.cgi?json`: the status as PRACOVNA last posted it.
- `POST index.cgi?update`: a new status, sent as a JSON object with the header `X-Heavy-Password`.
  - Wrong or missing password: 403.
  - No password file on the server: 503.
  - Not a JSON object: 400.
  - Larger than 1 MB: 413.
  - Accepted: 204.

## Data

The data directory is `/home/protected/heavyboard`, or `HEAVYBOARD_DATA` when set. On
NearlyFreeSpeech, the web cannot read /home/protected. The directory holds two files:
- `password`: only its first line is used.
- `status.json`: the last posted status. Each update replaces it whole, by writing a new file and
  renaming it over the old one.

## Building and testing

- `kpp --test` here runs the handler's tests.
- `kpp` builds a local version that serves at http://127.0.0.1:8765/.
- `kpp --release --target freebsd heavyboard` builds the site's program. Add
  `--sysroot "C:\Program Files\kpp\sysroots\freebsd-x86_64"` when the kpp you use has no FreeBSD
  sysroot of its own.

## On PRACOVNA

heavy.ps1 writes C:\kpp-sandbox\heavy\status.json whenever a job queues, starts or finishes.
publish.ps1 then posts it to the address and password in C:\kpp-sandbox\heavy\publish.txt (two
lines). The last post's result is in publish.log.

## Install

The upload is a folder with four files: index.cgi (the FreeBSD build), .htaccess
(`DirectoryIndex index.cgi`), password and install.sh. Run `sh install.sh` in that folder on the
site. It does the following:
- Puts index.cgi and .htaccess in /home/public/heavy.
- Puts the password in /home/protected/heavyboard.
- Gives the web group write access to /home/protected/heavyboard, because NearlyFreeSpeech runs CGI
  programs in that group.
