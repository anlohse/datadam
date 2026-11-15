#!/bin/sh
# The release packages of a datadam binary, in <output dir>:
#   datadam-<version>-linux-<arch>.tar.gz   the binary, datadam.conf, the systemd unit, LICENSE, README.md
#   datadam_<version>_<deb arch>.deb        Debian/Ubuntu: /usr/bin/datadam, /etc/datadam/datadam.conf, a
#                                           datadam user, a systemd service started on install
# The version comes from the binary (datadam --version). Run by the Dockerfile's
# package stage:  docker build --target packages --build-arg GIT_COMMIT=$(git rev-parse HEAD) -o dist .
# usage: packaging/build.sh <datadam binary> <output dir>
set -eu

bin=$(realpath "$1")
out=$2
root=$(cd "$(dirname "$0")/.." && pwd)

version=$("$bin" --version | sed -n 's/^datadam \([^ ]*\) .*/\1/p')
[ -n "$version" ] || { echo "packaging: no version from $bin --version" >&2; exit 1; }
case $(uname -m) in
  x86_64) arch=x86_64 deb_arch=amd64 ;;
  aarch64) arch=arm64 deb_arch=arm64 ;;
  *) echo "packaging: unsupported machine $(uname -m)" >&2; exit 1 ;;
esac
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$out"

# ---- tarball: run in place (data in ./data, as the repository's datadam.conf)
name=datadam-$version-linux-$arch
mkdir -p "$work/$name"
install -m 0755 "$bin" "$work/$name/datadam"
install -m 0644 "$root/datadam.conf" "$root/packaging/datadam.service" "$root/LICENSE" "$root/README.md" "$work/$name/"
tar -C "$work" --owner=0 --group=0 --numeric-owner -czf "$out/$name.tar.gz" "$name"

# ---- .deb
pkg=$work/deb
mkdir -p "$pkg/DEBIAN" "$pkg/usr/bin" "$pkg/etc/datadam" "$pkg/usr/lib/systemd/system" "$pkg/usr/share/doc/datadam"
install -m 0755 "$bin" "$pkg/usr/bin/datadam"
strip --strip-unneeded "$pkg/usr/bin/datadam" 2>/dev/null || true
# A package starts the server at once: it listens on the loopback interface
# until the administrator chooses otherwise (bind, requirepass).
sed -e 's/^bind .*/bind 127.0.0.1/' -e 's|^dir .*|dir /var/lib/datadam|' "$root/datadam.conf" > "$pkg/etc/datadam/datadam.conf"
grep -q '^bind 127.0.0.1$' "$pkg/etc/datadam/datadam.conf" && grep -q '^dir /var/lib/datadam$' "$pkg/etc/datadam/datadam.conf" ||
  { echo "packaging: datadam.conf has no bind or dir line to set" >&2; exit 1; }
install -m 0644 "$root/packaging/datadam.service" "$pkg/usr/lib/systemd/system/datadam.service"
install -m 0644 "$root/LICENSE" "$pkg/usr/share/doc/datadam/copyright"
install -m 0644 "$root/README.md" "$pkg/usr/share/doc/datadam/README.md"
for script in postinst prerm postrm; do install -m 0755 "$root/packaging/deb/$script" "$pkg/DEBIAN/$script"; done
echo /etc/datadam/datadam.conf > "$pkg/DEBIAN/conffiles"

# The shared libraries the binary needs, with the versions it was linked
# against (RocksDB, OpenSSL and LuaJIT are inside it).
mkdir -p "$work/shlibs/debian"
printf 'Source: datadam\n\nPackage: datadam\nArchitecture: any\n' > "$work/shlibs/debian/control"
depends=$(cd "$work/shlibs" && dpkg-shlibdeps -O "$pkg/usr/bin/datadam" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')
[ -n "$depends" ] || { echo "packaging: dpkg-shlibdeps found no dependencies" >&2; exit 1; }

cat > "$pkg/DEBIAN/control" <<EOF
Package: datadam
Version: $version
Architecture: $deb_arch
Maintainer: datadam <https://github.com/anlohse/datadam/issues>
Depends: $depends, adduser
Section: database
Priority: optional
Homepage: https://github.com/anlohse/datadam
Installed-Size: $(du -sk "$pkg" | cut -f1)
Description: Redis-compatible key-value store on RocksDB
 datadam speaks the Redis protocol (RESP2/RESP3) and memcache, and keeps its
 data in RocksDB with an object cache in front: data sets larger than memory.
 Secondary indexes, RediSearch-style FT.*, JSON, Lua scripting, transactions,
 Pub/Sub, a change feed, and replication with Sentinel failover.
 .
 The service starts on install, listening on 127.0.0.1:6379 (Redis) and
 127.0.0.1:11211 (memcache), with its data in /var/lib/datadam.
EOF
dpkg-deb --root-owner-group --build "$pkg" "$out/datadam_${version}_$deb_arch.deb" >/dev/null

ls -l "$out"
