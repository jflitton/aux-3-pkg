# Formats

The files `pkg` reads and writes. Each has a format number; `pkg` refuses a number higher
than it knows (see [decisions.md](decisions.md), "No dependencies yet"). All text is
ASCII lines ending in a newline; fields are separated by one tab, so names and paths may
contain spaces (Mac files often do) but not tabs or newlines.

## Names, versions and paths

- A package **name** is 1 to 64 of `a-z 0-9 + - . _`, starting with a letter or digit
  and not ending in `.pkg` (which marks a package file on pkg's command line).
- A **version** is 1 to 64 of `A-Z a-z 0-9 + - . _ ~`. Versions compare as Debian's do:
  runs of digits numerically, everything else character by character, and `~` sorts
  before anything, even the end (`1.0~beta1` < `1.0` < `1.0-1` < `1.0.1` < `1.10`).
- A **path** in a package is relative to `/`, without a leading `/` or `./`: for example
  `usr/local/bin/hello`. No empty, `.` or `..` components, no control characters, at most
  255 bytes. Nothing may go under `usr/local/lib/pkg`, which is `pkg`'s own.

## Package file (`NAME-VERSION.pkg`)

A gzip-compressed ustar archive. The first member is `+PKG`, a regular file of `key
value` lines (the key, one space, the rest of the line):

    format 1
    name hello
    version 1.0-1
    summary Prints a greeting

`format`, `name`, `version` and `summary` are required; other keys are kept and shown by
`pkg info` (for example `url`, `license`, `maintainer`), except `installed`, `from` and
`server`, which are the record's own (below) and make the package invalid.

`shell PATH`, which may be given more than once, names a login shell in the package, as a
path from `/` (`shell /usr/local/bin/mksh`). Installing the package adds it to
`/etc/shells`; removing the package takes it out, and is refused while it's someone's
login shell in `/etc/passwd`. A package with a `shell` line is format 2, which needs pkg
0.3 or later.

`boot COMMAND`, which may be given more than once, is run at every boot: a program in the
package as a path from `/`, then any arguments, one space apart, in letters, digits and
`/ . _ , = + -` (`boot /usr/local/bin/netsetup boot`). Installing the package adds a line
for it to `/etc/inittab`, after the one that runs `/etc/rc`:

    pk00:2:wait:/usr/local/bin/netsetup boot </dev/syscon >/dev/syscon 2>&1	# pkg netsetup

with the first `pkNN` id that's free, and the tab and comment naming the package.
Removing the package takes the line out, found by what follows its third colon, so the
id, run levels and action can be changed (`off` stops it running). A package with a
`boot` line is format 3, which needs pkg 0.5 or later; packages with neither line are
format 1.

Every other member is the
package's content: regular files, directories and symbolic links, with their modes,
owners and modification times. Other member types, and other names starting with `+`,
make the package invalid. A Mac file's resource fork and Finder information are its
AppleDouble `%NAME` file beside it, an ordinary file to `pkg`.

`tools/pkg-build.py` makes these.

## Server

A server is a URL ending in `/`, under which are:

| Path | What |
|---|---|
| `index` | the packages on offer |
| `index.sig` | signatures of `index` |
| the packages | wherever `index` says, relative to the server's URL |

`index`:

    aux-pkg-index 1
    hello	1.0-1	12345	SHA256	hello-1.0-1.pkg	Prints a greeting

The first line names the format. Then one line per package: name, version, size in bytes,
the SHA-256 of the package file (64 lowercase hex digits), its path relative to the
server, and its summary. Lines starting with `#` are ignored. A name may appear more than
once, at different versions; `pkg` takes the highest.

`index.sig`: one or more lines of

    ed25519	PUBLIC-KEY	SIGNATURE

the key (64 hex digits) and an Ed25519 signature of the whole of `index` (128 hex
digits). `pkg` accepts `index` if a line's key is one it trusts and its signature is good.

`tools/pkg-repo.py` makes both from a directory of packages.

## On the Mac

| Path | What |
|---|---|
| `/etc/pkg.conf` | settings (below) |
| `/usr/local/lib/pkg/installed/NAME` | what's installed (below) |
| `/usr/local/lib/pkg/servers` | the servers pkg has package lists from (below) |
| `/usr/local/lib/pkg/lists/NAME` | the last package list from server NAME, as fetched |
| `/usr/local/lib/pkg/download/` | downloads in progress |
| `/usr/local/lib/pkg/lock` | the process ID of the `pkg` changing things, while it runs |

`/etc/pkg.conf`, optional, `#` starting a comment:

    server http://example.com/aux/     # instead of the built-in server, main
    key 0123...cdef                    # also trust this signing key for main
    unsigned                           # accept main's package list unsigned
    repo jeff http://example.org/aux/ 4567...89ab   # another server, and its key

A `repo` line is a server's name (as a package's name, but not `main`), its URL, and then
one or more keys trusted to sign its list, or `unsigned`, or both. Keys are trusted only
for the server they're given for. Servers are searched in this order: main, then the
repo lines as written.

`servers`: a line per server, in that order: its name, URL, and `signed` or `unsigned`,
separated by tabs. `pkg update` writes it after the lists.

The record of an installed package: `key value` lines as in `+PKG`, with its `format`
(plus `installed`, the
time as seconds since 1970, `from`, the URL or file it came from, and `server`, the name
of the server it came from, if it came from one), then a line per thing it installed:

    f	PATH	MODE	UID	GID	SIZE	SHA256	a regular file (MODE in octal)
    l	PATH	TARGET					a symbolic link
    d	PATH						a directory it created

Only directories the package created are listed; removing the package removes them if
they are empty.
