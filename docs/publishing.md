# Running a pkg server

A server is a directory of files on any web server that answers plain HTTP: the
packages, `index` listing them and `index.sig` signing it ([formats.md](formats.md)).

## Once: the signing key

    tools/pkg-repo.py keygen ~/aux-pkg.key

writes the secret key to `~/aux-pkg.key` (readable only by you) and prints the public
key. Keep the secret one private and backed up: anyone who has it can sign package lists
pkg will trust, and without it you can't sign new ones. Never commit it. `pkg-repo.py`
also takes a `.env` file holding the key as `PKG_KEY=ed25519-secret ...` wherever it
takes a key file; `.env` at the top of this repository is ignored by git.

The public key is what pkg trusts. For the default server, put it in
`default_keys` in `src/defaults.h`, with the server's address in `DEFAULT_SERVER`, and
release a new pkg. For any other server, its users add it to `/etc/pkg.conf`:

    server http://example.com/aux/
    key PUBLIC-KEY

To change keys, add the new public key to `default_keys` beside the old and release pkg;
once that's in use, sign with the new key and drop the old one.

## Each time you publish

1. Build each package into the server's directory: `tools/pkg-build.py SRC site/packages`.
   Keep every package already published there too: the list names all of them.
2. List and sign them: `tools/pkg-repo.py index site KEYFILE`. This writes
   `site/index` and `site/index.sig`, listing every `*.pkg` under `site`.
3. Upload the packages first, then `index` and `index.sig`, so the list never names a
   package that isn't there yet.

For a server whose files are in an S3 bucket, `tools/publish.sh site s3://BUCKET
http://SERVER/` does step 3. It uploads the new packages, checks each comes back from
the server as sent, then uploads `index.sig` and `index`, with the caching below. It
refuses to replace a package that's up already with a different file: a new build needs
a new version. To try a list out first, publish it to a directory of its own
(`s3://BUCKET/test http://SERVER/test/`) and `pkg -s http://SERVER/test/ update`.

## What a server needs

- **Answer plain HTTP, and don't redirect it to HTTPS.** pkg can't use HTTPS, and refuses
  a redirect to it. Browsers can still be given HTTPS.
- **Don't cache `index` and `index.sig`, or only briefly.** They change with every
  publish, and pkg reads them as a pair: a cached copy of one beside a new copy of the
  other looks unsigned until both are new.
- **Packages can be cached for as long as you like.** Once published, a package never
  changes: a new version is a new file.
- **A missing file is a 404**, not a 403 (as a private S3 bucket gives when its reader
  may not list it): pkg takes a 404 for `index.sig` to mean the list isn't signed, and
  anything else as an error.
- pkg sends `Host`, asks for no compression and accepts chunked replies, so any HTTP/1.1
  server works.

## Trying it locally

    python3 -m http.server -d site 8000

and on the Mac (or `build/host/pkg -R SOMEDIR` on this one), with the key in
`/etc/pkg.conf`: `pkg -s http://YOUR-MAC:8000/ update`.
