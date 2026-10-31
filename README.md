# pkg for A/UX 3

The missing package manager for A/UX 3.1, Apple's Unix for 68k Macs. It can install software from
package files or via http. A/UX can't use HTTPS, so pkg checks every download against 
a signed package list instead. Installed packages can be upgraded, checked and removed cleanly.

**Status:** early. The default server has pkg itself and, so far, two other packages,
gzip and mksh, from [aux-3-ports](https://github.com/jflitton/aux-3-ports). A/UX 3.0.1 should work
too (its system calls and structures are the same as far as pkg uses them) but hasn't
been tried.

## Getting pkg

The easiest way is [aux-3-patches](https://github.com/jflitton/aux-3-patches)' `--pkg`
option, which puts pkg on an A/UX disk image as it makes it, recorded as an installed
package so that `pkg upgrade` upgrades pkg too. Or build it (see
[docs/development.md](docs/development.md)) and copy `build/aux/pkg` to the Mac as
`/usr/local/bin/pkg`, mode 755, with `ftp` in binary mode.

## Using it

In CommandShell, as root:

    pkg update                    get the lists of packages from the servers
    pkg list -a                   see what's there
    pkg install hello             install a package from a server
    pkg install hello-1.0.pkg     ...or from a package file
    pkg upgrade                   install newer versions of what you have
    pkg remove hello              remove it again

and `pkg list` (what's installed), `pkg search WORD`, `pkg info PACKAGE`, `pkg files
PACKAGE`, `pkg verify` (have installed files changed?) and `pkg owner /PATH` (which package
installed it). `pkg help` lists them all. Options can go before or after the command.

`pkg upgrade` upgrades pkg before anything else and has the new pkg do the rest, since a
new package can need a newer pkg. (That's from pkg 0.4; with an older one, `pkg upgrade
pkg` first.)

A package file is told from a package's name by how it's written, never by what's on
disk: it ends in `.pkg`, or it's a path with a `/` in it (`./hello` for one in the current
directory).

pkg exits 0 if it did what was asked, 1 if it couldn't, found nothing (`search`, `info`,
`owner`) or found changes (`verify`), and 2 if the command line is wrong, having done
nothing.

## Other servers

pkg's own server is called `main`. To add another beside it, give it a name in
`/etc/pkg.conf`, with the key its lists are signed with (the server's owner will tell you
it):

    repo jeff http://example.com/aux/ PUBLIC-KEY

`pkg update` then gets every server's list, and `pkg list -a` shows that server's
packages as `jeff/NAME`. A name is looked for on main first, then on the other servers in
the order of their lines, and the first that has it is the one, whatever version the
others have: `pkg install jeff/hello` gets jeff's anyway. An installed package is only
upgraded from the server it came from; installing it from another moves it there.
`pkg list` shows where each came from the same way, and every command takes a package
by the name it shows.

To use a different server instead of main, such as a mirror of it, put `server
http://example.com/aux/` in `/etc/pkg.conf`, with a line `key PUBLIC-KEY` unless it's a
mirror; `pkg update -s http://example.com/aux/` does that until the next update.

A key is trusted only for the server it's given for. `unsigned` on a repo line (or a line
`unsigned`, or `pkg update --unsigned`, for main) accepts that server's list if it isn't
signed by a key pkg trusts for it; then nothing guards against a download being changed
on the way.

## What pkg won't do

- **Overwrite a file a package didn't install**, or another package's file. If anything
  in a package is in the way, pkg installs none of it and says what's in the way.
- **Run scripts** from packages: a package is files, plus the few changes pkg itself
  makes and undoes: adding a shell to `/etc/shells`, and a command to run at boot to
  `/etc/inittab`.
- **Dependencies**: each package has what it needs.

[docs/decisions.md](docs/decisions.md) says why.

## Making packages

Lay the files out under a directory as they'll be installed, add a `+PKG` there:

    format 1
    name hello
    version 1.0
    summary Prints a greeting

and run `tools/pkg-build.py DIR`. A package whose program is a login shell says so with
a line `shell /usr/local/bin/NAME` (and `format 2`), and pkg adds it to `/etc/shells`.
One with a command to run at every boot says `boot /usr/local/bin/NAME ARGS` (and
`format 3`), and pkg adds a line for it to `/etc/inittab`.
To run a server of your own, see
[docs/publishing.md](docs/publishing.md); the formats are in
[docs/formats.md](docs/formats.md).

## Limitations

- Packages are trusted: one can put files anywhere but pkg's own directory, including
  through a symbolic link it installed. The signature says who made the list; pkg doesn't
  judge what's in a package.
- The package list doesn't expire, so someone in the middle could keep showing you an old
  signed list, hiding newer versions.
- HTTP only.

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
