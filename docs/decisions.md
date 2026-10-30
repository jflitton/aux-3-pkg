# Decisions

What we've settled about `pkg`, and why. Newest last. When a decision changes, edit it here
and say what replaced it, rather than leaving two answers.

## 2026-10-29: the first round

### Package manager first, toolchain and runtime later

`pkg` comes before a toolchain or a shared runtime library. glibc can't run on A/UX: it
needs Linux's syscalls and ELF's dynamic loader, and A/UX has COFF and System V Release 3
static shared libraries (`/shlib/libc_s` and friends, built with `/bin/mkshlib`). The
work that exists elsewhere is a compiler, not a libc: gcc 2.5.7 to 2.7.2 binaries from the
1990s jagubox archive (mirrored on discmaster from ftp.mayn.de), and uyjulian's unfinished
modern cross-toolchain (github.com/uyjulian/aux_crosscompile_toolchain). Those, and a
shared runtime library, will be delivered as packages, so the delivery mechanism comes
first.

### Its own repository, copied into aux-3-patches as a binary

`aux-pkg` is a separate repository, MIT licensed. aux-3-patches will get a `--pkg` option
that installs a built `pkg` from `patches/pkg/bin/`, copied from a tagged `aux-pkg`
release by a script that records the version. Not a git submodule: aux-3-patches tells
users to use GitHub's "Download ZIP", which leaves submodules out. `pkg` is built only
from our own source, so it meets aux-3-patches' rule for `bin/`. The patch also registers
`pkg` as an installed package, so it can upgrade itself.

### Built freestanding on macOS

`pkg` is C, cross-compiled with m68k-elf-gcc, with our own syscall stubs and the small
part of libc it needs, and converted from ELF to an A/UX COFF executable by our own tool.
No Apple code goes into the binary, the build is reproducible, and building needs no
emulator. The alternative, compiling on A/UX with its own `cc`, was rejected: that
compiler is pre-ANSI (no prototypes, `const` or `volatile`), every build would need an
emulator, and the binary would carry Apple's libc.

### Plain HTTP, a signed index

A/UX can't realistically do TLS, so `pkg` speaks plain HTTP. Integrity comes from the
index instead:

- The server holds an `index` listing every package with its SHA-256, and a signature
  of the index (Ed25519).
- The maintainer keeps the signing key and signs once per publish, when the index is
  rebuilt. Packages themselves aren't signed; the index's hashes cover them. Authors who
  aren't the maintainer send packages in, or run their own server with their own key.
- `pkg` trusts the keys built into it plus any added in `/etc/pkg.conf`, each for one
  server only (2026-10-30, "Keys belong to a server"). More than one key can be trusted
  for a server at once, so a key can be replaced by shipping a `pkg` that knows the new
  one.
- A custom server either has its key in `/etc/pkg.conf` or is accepted unsigned there
  (or, for the default server, with `pkg update --unsigned`).
- A package file installed from disk is checked against its own checksums only: the same
  trust as running any program you downloaded.

### Servers

pkg has a default server built in, which answers plain HTTP (and HTTPS for browsers).
The user can add other servers in `/etc/pkg.conf` or name one on the command line. During
development, a local server. How several work together: 2026-10-30, "More than one
server".

### A/UX 3.1 required, 3.0.1 wanted

3.1 must work; 3.0.1 should, if its syscalls match.

### Packages never overwrite files they don't own

If any file in a package already exists and doesn't belong to that same package (as in an
upgrade), `pkg` refuses the whole package before writing anything. There's no `--force`.
Directories that already exist, such as `/usr/local/bin`, are shared. Replacing A/UX's own
files isn't needed: fixes to A/UX itself go through aux-3-patches, and new software goes
in `/usr/local`.

### No install or remove scripts

Packages are files only. A script could change any file, which would break the rule
above. If a real package turns out to need setup, we revisit this.

### Command line only

A Mac front end can come later, as a package of its own.

### No dependencies yet

Every A/UX program links against Apple's libc, which is always present, so packages can
be self-contained. Dependencies become worth having with a shared runtime library, and
we add them then. Until then every package records its format version, and `pkg` refuses
a format newer than it understands ("upgrade pkg first"), so packages that need
dependencies can't be installed by a `pkg` that would ignore them.

## 2026-10-29: while building it

### Formats

Settled in [formats.md](formats.md). Packages are gzip-compressed ustar archives, which
any host can make and which pkg unpacks itself (A/UX has no gzip). Text files are lines of
tab-separated fields, so Mac file names with spaces need no quoting. `index.sig` signs
the whole index, and each line carries its public key in full, so a list can carry
signatures by more than one key during a change of keys. Versions compare as Debian's do.

### The package list remembers its server

`-s` goes with `pkg update` only. Installs and upgrades fetch a package from the server
whose list named it, so a list and its packages can't come from different places. (With
more than one server, each list remembers its own.)

### Mac file names in Mac Roman

`pkg-build.py` writes names in Mac Roman, which A/UX's Finder shows; macOS names are UTF-8.

### Tested in QEMU, against a server inside A/UX

QEMU's Quadra 800 has no network card A/UX 3.1 can drive, so the A/UX tests run a small
HTTP and DNS server on A/UX's loopback interface. pkg also builds for the host, where the
same code is tested against a local server in seconds.

### pkg doesn't know how servers are hosted

pkg's messages say what it needs from a server (plain HTTP), not how to set up any
particular host. A redirect to HTTPS is refused with "redirects to HTTPS, which pkg can't
use", and no mention of whatever serves the files. What a server needs is in
[publishing.md](publishing.md).

### Packages are trusted

The no-overwrite rule guards against accidents, not malicious packages: a package can put
files anywhere but pkg's own directory, and pkg doesn't police paths through symbolic
links a package installed. The signature is what says a package can be trusted.

## 2026-10-30: publishing

### The repository is public

It holds nothing about the machines it's developed on or how the default server is
hosted. Paths to the A/UX disk and ROM the tests boot go in `local.mk`, which git
ignores; the signing key stays out of git; and `tools/publish.sh` takes the bucket and
the server's URL as arguments. The default server's address is in `src/defaults.h`, as
pkg needs it.

### Packages are cached for a year, the list not at all

A package never changes once it's up: a new build needs a new version, and
`tools/publish.sh` refuses to replace one with a different file. So packages can be
cached for a year. `index` and `index.sig` shouldn't be cached: pkg reads them as a pair,
and two files cached separately could come from different publishes.

### Tests never reach the default server

The build for this machine, which the tests use, leaves the default server out. Its
built-in keys are the real ones.

## 2026-10-30: more than one server

### Replacing the default server and adding servers are different things

pkg's own server is called `main`. A `server` line in `/etc/pkg.conf` replaces it: with
a mirror, which the built-in keys still verify because a list names its packages
relative to the server, or with a different set of packages altogether, given its key by
`key` lines. `repo NAME URL KEY` lines add servers beside it. Cargo draws the same line
(source replacement is for mirrors; other registries are named), as Debian does between
a mirror and another source.

### Keys belong to a server

The built-in keys and `key` lines are trusted for main only, and the keys on a repo line
for that server only. Before, any key pkg trusted could sign any server's list, so over
plain HTTP whoever held a key added for one server could pass a list off as another's;
that's why Debian replaced `apt-key`'s shared keyring with a key per source. `unsigned`
is per server too, and `--unsigned` is for main only: accepting one hobbyist's server
unsigned mustn't leave main unchecked.

### The first server that has a package has it

Servers are searched in order: main, then the repo lines as written. The first that has a
package by a name is where it comes from, whatever version the others have, and
`SERVER/NAME` picks one. apt, dnf and pip's `--extra-index-url` take the highest version
on any server, which lets any server replace any package by numbering it higher
("dependency confusion"); pacman's ordered repositories don't. Only one package of a name
can be installed, so names aren't namespaced, as npm's scopes are: `SERVER/NAME` only
chooses.

### Packages stay with the server they came from

An installed package's record names its server, and it's upgraded from there only, as
FreeBSD's pkg does by default. A repo adding a package main has, or main adding one a
repo has, doesn't move anyone's installed copy; `pkg install SERVER/NAME` does. If pkg
has no list from a package's server any more, it says so and doesn't upgrade it from
elsewhere. A package installed from a file, or before records named servers, follows the
first server that has it.

A record's `installed`, `from` and `server` lines are pkg's own, so a package's `+PKG`
may no longer have them: one could otherwise name its server for itself.

### A server that can't be had keeps its last list

`pkg update` gets every server's list. From one it can't reach, or whose list it refuses,
it keeps the list it had, so a server being down neither hides its packages nor changes
which server a name resolves to; `pkg update` still fails. A server no longer in
`/etc/pkg.conf` is dropped. `-s` still names main's server for one update.

### Dependencies between servers, once there are dependencies

Not built yet ("No dependencies yet"), but decided now, while it's cheap: a package may
depend on packages from its own server or from main, and nothing else, as Ubuntu's PPAs
build on its main archive. A repo can rely on main; main never relies on a repo, and no
repo on another.

## 2026-10-30: the command line

What matters most is that pkg does exactly what was meant or nothing; then that it's
what someone would guess; then that it looks like package managers they know.

### A mistake on the command line does nothing

Every option and argument is checked before pkg changes anything, and a mistake gets a
line saying what's wrong (exit 2), not the whole usage. Before, `-u` was ignored by
every command but `update`, and `pkg -s NAME update`, given a server's name for its
address, updated the other servers before failing.

### Options go anywhere

Before or after the command, with `--` ending them, as apt and Homebrew allow. Before,
options after the command were package names: `pkg remove games -R /mnt` removed games
from `/`, then said `-R` and `/mnt` weren't installed. No package's name starts with
`-`, so this is never ambiguous.

### Files and packages are told apart by how they're written

A package file ends in `.pkg` or is a path with a `/` in it; `SERVER/NAME` is a package,
even if a file has that name (`./SERVER/NAME` is the file). Before, it depended on what
was in the current directory. Package names may no longer end in `.pkg`, so that a
mistyped file name is reported as a missing file, not a missing package.

### pkg takes back every name it shows

`pkg list` shows a package from a server other than main as `SERVER/NAME`, as `pkg list
-a` does, and every command that takes an installed package takes that form, checking
it came from that server.

### `owner` takes whole paths

From `/`, without `.` or `..`: a relative path was taken from `/`, and `..` can't be
resolved without knowing which directories are symbolic links, so pkg gave wrong
answers. A directory a package made is that package's.

### Renamed options

`-R` is `-r`, as for FreeBSD's pkg and pacman; `-R` reads as "recursive", which
FreeBSD's `pkg delete -R` is. `-u` is `--unsigned`, the word `/etc/pkg.conf` uses: the one
option that turns checking off is spelled out (as apt's `--allow-unauthenticated` and
dnf's `--nogpgcheck` are), and `-u` read like `update` or `upgrade`. `-v` did nothing and
is gone. `-s` and `--unsigned` go with `update` and are shown there.

### Exit status

0 done; 1 couldn't, or found nothing (`search` now too, as `owner` and `info` already
did), or `verify` found changes; 2 a mistake on the command line.
