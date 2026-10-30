# pkg for A/UX 3

The missing package manager for A/UX 3.1, Apple's Unix for 68k Macs. It can install software from
package files or via http. A/UX can't use HTTPS, so pkg checks every download against 
a signed package list instead. Installed packages can be upgraded, checked and removed cleanly.

**Status:** early, and the default server has no packages yet. A/UX 3.0.1 should work
too (its system calls and structures are the same as far as pkg uses them) but hasn't
been tried.

## Getting pkg

Build it (see [docs/development.md](docs/development.md)) and copy `build/aux/pkg` to the
Mac as `/usr/local/bin/pkg`, mode 755: with `ftp` in binary mode, or onto a disk image
with aux-3-patches' `tools/ufsput.py`. aux-3-patches will get a `--pkg` option that does
this when it makes a disk.

## Using it

In CommandShell, as root:

    pkg update                    get the list of packages from the server
    pkg list -a                   see what's there
    pkg install hello             install a package from the server
    pkg install hello-1.0.pkg     ...or from a package file
    pkg upgrade                   install newer versions of what you have
    pkg remove hello              remove it again

and `pkg list` (what's installed), `pkg search WORD`, `pkg info PACKAGE`, `pkg files
PACKAGE`, `pkg verify` (have installed files changed?) and `pkg owner PATH` (which package
installed it). `pkg` alone lists them all.

To use another server, `pkg -s http://example.com/aux/ update` once: installs and upgrades
then come from the server the list came from. To use it every time, put it in
`/etc/pkg.conf`, with the key its lists are signed with (the server's owner will tell you
it):

    server http://example.com/aux/
    key PUBLIC-KEY

`-u` (or a line `unsigned` in `/etc/pkg.conf`) accepts a list that isn't signed by a key
pkg trusts; then nothing guards against a download being changed on the way.

## What pkg won't do

- **Overwrite a file a package didn't install**, or another package's file. If anything
  in a package is in the way, pkg installs none of it and says what's in the way.
- **Run scripts** from packages: a package is only files.
- **Dependencies**: each package has what it needs.

[docs/decisions.md](docs/decisions.md) says why.

## Making packages

Lay the files out under a directory as they'll be installed, add a `+PKG` there:

    format 1
    name hello
    version 1.0
    summary Prints a greeting

and run `tools/pkg-build.py DIR`. To run a server of your own, see
[docs/publishing.md](docs/publishing.md); the formats are in
[docs/formats.md](docs/formats.md).

## Limitations

- Packages are trusted: one can put files anywhere but pkg's own directory, including
  through a symbolic link it installed. The signature says who made the list; pkg doesn't
  judge what's in a package.
- The package list doesn't expire, so someone in the middle could keep showing you an old
  signed list, hiding newer versions.
- HTTP only, one server at a time.

## License

MIT; see [LICENSE](LICENSE). pkg's Ed25519 follows TweetNaCl (public domain) and its
inflate Mark Adler's puff.c (zlib license).

## Disclaimer

This project is provided "as is", without warranty of any kind, express or implied,
including the warranties of merchantability and fitness for a particular purpose. pkg runs
as root and installs software from wherever you point it; software can lose data or leave
a Mac unable to start, so you use it at your own risk, and keep backups. Packages are the
work of their own authors, who answer for them. The authors of pkg are not liable for any
damage or loss arising from its use, or from the use of anything it installs.

This project isn't affiliated with or endorsed by Apple. A/UX, Macintosh and Quadra are
trademarks of Apple Inc.; other names are trademarks of their owners.
