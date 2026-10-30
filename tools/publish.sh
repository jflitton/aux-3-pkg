#!/bin/sh
# Put a signed package list and its packages on a server whose files are in an S3 bucket.
#
#     tools/publish.sh SITE s3://BUCKET[/DIR] http://SERVER[/DIR]/
#
# SITE is a directory tools/pkg-repo.py has listed and signed (docs/publishing.md). Its files go
# into the bucket (under DIR, if given), and each is fetched back from the server's URL to check
# it: the URL pkg uses, wherever it serves the bucket from over plain HTTP.
#
# A package never changes once it's up (a new build needs a new version, so a new name; this
# refuses to put a different file where one is), so packages go up to be cached for a year.
# index and index.sig go up with no-cache, so a cache in front of the bucket asks it for them
# every time: pkg reads them as a pair, and two files cached separately could be from different
# publishes.
#
# The order makes sure a pkg never reads a list naming a package it can't fetch: the packages,
# each fetched back through the server and compared, then index.sig and index. Only in the moment
# between those two can a pkg get a list and signatures that don't match; it says the list isn't
# signed, and works when run again. Nothing is deleted: a pkg that read an older list still
# fetches what it names.
#
# Needs the AWS CLI, signed in as someone who may write the bucket.

set -e
LONG="public, max-age=31536000, immutable"
die() { echo "publish: $*" >&2; exit 1; }
[ $# -eq 3 ] || { echo "usage: publish.sh SITE s3://BUCKET[/DIR] http://SERVER[/DIR]/" >&2; exit 2; }
SITE=${1%/} DEST=${2%/} URL=${3%/}/
case $DEST in s3://?*) ;; *) die "$2: not s3://BUCKET[/DIR]" ;; esac
case $URL in http://?*) ;; *) die "$3: not an http:// URL (pkg can't use HTTPS)" ;; esac
DEST=${DEST#s3://} BUCKET=${DEST%%/*} PREFIX=
[ "$DEST" = "$BUCKET" ] || PREFIX=${DEST#*/}/
export AWS_PAGER=
[ -f "$SITE/index" ] || die "$SITE/index isn't there: list the packages with tools/pkg-repo.py index"
[ -s "$SITE/index.sig" ] || die "$SITE/index isn't signed: give tools/pkg-repo.py index the key"
[ "$(head -1 "$SITE/index")" = "aux-pkg-index 1" ] || die "$SITE/index isn't a package list"
PATHS=$(tail -n +2 "$SITE/index" | grep -v '^#' | cut -f5) || true
for p in $PATHS; do
    case $p in
        *[!A-Za-z0-9._~+/-]*) die "$p: pkg asks for a path as it is, so it can't need %-escaping" ;;
    esac
    [ -f "$SITE/$p" ] || die "$SITE/$p is in the list but isn't there"
done

NEW=
for p in $PATHS; do
    if aws s3api head-object --bucket "$BUCKET" --key "$PREFIX$p" >/dev/null 2>&1; then
        aws s3 cp "s3://$BUCKET/$PREFIX$p" - --only-show-errors | cmp -s - "$SITE/$p" ||
            die "$PREFIX$p is in the bucket already, and different: a new build needs a new version"
    else
        NEW="$NEW $p"
    fi
done
for p in $NEW; do
    aws s3 cp "$SITE/$p" "s3://$BUCKET/$PREFIX$p" --only-show-errors \
        --cache-control "$LONG" --content-type application/octet-stream
done
for p in $NEW; do   # (a new path: no cache has it, so this reads what went up)
    curl -sf "$URL$p" | cmp -s - "$SITE/$p" ||
        die "$URL$p doesn't come back as sent; the list is left as it was"
done
for f in index.sig index; do
    aws s3 cp "$SITE/$f" "s3://$BUCKET/$PREFIX$f" --only-show-errors \
        --cache-control no-cache --content-type text/plain
done
for f in index index.sig; do
    curl -sf "$URL$f" | cmp -s - "$SITE/$f" || die "$URL$f doesn't come back as sent"
done
set -- $PATHS
echo "publish: $URL lists $# package(s), $(echo $NEW | wc -w | tr -d ' ') new"
