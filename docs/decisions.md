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
- `pkg` trusts the keys built into it plus any added in `/etc/pkg.conf`. More than one key
  can be trusted at once, so a key can be replaced by shipping a `pkg` that knows the new
  one.
- A custom server either has its key added in `/etc/pkg.conf` or is used with an
  explicit flag that accepts it unsigned.
- A package file installed from disk is checked against its own checksums only: the same
  trust as running any program you downloaded.

### Servers

pkg has a default server built in, which answers plain HTTP (and HTTPS for browsers).
The user can add other servers in `/etc/pkg.conf` or name one on the command line. During
development, a local server.

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

`-s` goes with `pkg update` only. Installs and upgrades fetch from the server the current
list came from, so a list and its packages can't come from different places.

### Mac file names in Mac Roman

`pkg-build.py` writes names in Mac Roman, which A/UX's Finder shows; macOS names are UTF-8.

### Tested in QEMU, against a server inside A/UX

QEMU's Quadra 800 has no network card A/UX 3.1 can drive, so the A/UX tests run a small
HTTP and DNS server on A/UX's loopback interface. pkg also builds for the host, where the
same code is tested against a local server in seconds. Real hardware (DaynaPORT or an
Ethernet card) is still to be tried.

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
