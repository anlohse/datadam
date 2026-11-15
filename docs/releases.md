# Releases

What a release is, what its version promises, where to get it, and how one is made.

- [Versions](#versions)
- [Compatibility of the data on disk](#compatibility-of-the-data-on-disk)
- [What a release publishes](#what-a-release-publishes)
- [Verifying a download](#verifying-a-download)
- [Upgrading](#upgrading)
- [Making a release](#making-a-release)

## Versions

Versions follow [Semantic Versioning](https://semver.org): `MAJOR.MINOR.PATCH`. While the major version is 0,
datadam is not yet stable:

- a **minor** release (0.2 → 0.3) may change commands, defaults and configuration, and may change the storage
  format (below); its [CHANGELOG](../CHANGELOG.md) entry says what to do;
- a **patch** release (0.2.0 → 0.2.1) only fixes bugs and never changes the storage format: it replaces the
  previous patch of the same minor in place, and back.

From 1.0 on, a breaking change waits for the next major version.

`datadam --version` prints the version and the commit the binary was built from; `INFO server` has them as
`datadam_version` and `datadam_git_sha1`. The Redis version datadam reports to clients (`redis_version`, 7.2.0)
is the protocol level it implements, not its own version.

## Compatibility of the data on disk

A data directory records the layout of its data, the **storage format** (`INFO persistence`:
`storage_format`). Format 1 is the layout of 0.1 and 0.2; a directory without the record (from 0.1) is format 1.

- A datadam opens a directory of its own format or of an older one it can read (upgrading it in place, if a
  release ever needs to, with a line in the log and in the CHANGELOG).
- A datadam refuses a directory of a newer format and does not touch it:
  `/var/lib/datadam holds data in storage format 2, written by a newer datadam; datadam 0.2.0 reads formats up to 1`.
  Downgrading across a format change means restoring the backup taken before the upgrade.
- A primary and its replicas must run the same minor version: the replication stream carries the primary's
  records as they are. Upgrade the replicas first, then fail over (see [Upgrading](#upgrading)).

## What a release publishes

A release `vX.Y.Z` publishes, from one commit of `main` that passed CI:

| Where | What |
|---|---|
| `ghcr.io/anlohse/datadam:X.Y.Z`, `:X.Y`, `:latest` | The container image (linux/amd64), with an SBOM and build provenance attached. `:X.Y` follows the latest patch of that minor. |
| [GitHub release](https://github.com/anlohse/datadam/releases) | `datadam-X.Y.Z-linux-x86_64.tar.gz`: the binary, `datadam.conf`, a systemd unit, LICENSE and README. |
| | `datadam_X.Y.Z_amd64.deb`: Debian 13 / Ubuntu 24.04 and later. `/usr/bin/datadam`, `/etc/datadam/datadam.conf` (listening on 127.0.0.1), a `datadam` user, data in `/var/lib/datadam`, and a systemd service started on install. |
| | `datadam-X.Y.Z-dependencies-spdx.tar.gz`: the SPDX document of every library linked into the binary (RocksDB, OpenSSL, LuaJIT...). |
| | `SHA256SUMS`, and the release notes from the CHANGELOG. |

Between releases, every commit of `main` that passes CI is published as `ghcr.io/anlohse/datadam:sha-<commit>`
and `:edge` (the latest of them). They are for trying a fix before it is released; pin `sha-<commit>`, by
digest, if something depends on one.

The binary needs glibc 2.39 or later (it is built on Ubuntu 24.04); the libraries it uses besides glibc and
libstdc++ are linked into it.

## Verifying a download

Every file of a release, and the image, has a build provenance attestation: it was built by this repository's
release workflow, from the tagged commit.

```bash
sha256sum -c SHA256SUMS --ignore-missing
gh attestation verify datadam_0.2.0_amd64.deb --repo anlohse/datadam
gh attestation verify oci://ghcr.io/anlohse/datadam:0.2.0 --repo anlohse/datadam
```

## Upgrading

1. Read the CHANGELOG entries between the two versions.
2. Take a backup (`BACKUP`, see [Operations](operations.md)).
3. A single server: stop it (it syncs the WAL and closes RocksDB), replace the binary or the image, start it.
   With the .deb, `sudo apt install ./datadam_X.Y.Z_amd64.deb` restarts the service.
4. Replication: upgrade and restart each replica, wait for it to catch up (`INFO replication`), then fail over
   to one of them (`FAILOVER`, or `SENTINEL FAILOVER` with `promotion-handoff yes`) and upgrade the old primary.

## Making a release

1. On a branch: set the version in `CMakeLists.txt` (`project(datadam VERSION X.Y.Z ...)`), move the
   `Unreleased` entries of `CHANGELOG.md` to a `## [X.Y.Z] - YYYY-MM-DD` section, and merge it to `main`.
2. When CI has passed on that commit of `main`, tag it and push the tag:

   ```bash
   git tag -a vX.Y.Z -m "datadam X.Y.Z" <commit>
   git push origin vX.Y.Z
   ```

3. The [release workflow](../.github/workflows/release.yml) checks that the tag matches `CMakeLists.txt` and
   that the CHANGELOG has the section, builds and tests the commit again, builds the packages and installs the
   .deb on a clean Ubuntu, scans the image (Trivy), runs the redis-cli suite against it, and only then
   publishes the image and the GitHub release. If a step fails, nothing is published: fix it on `main`, delete
   the tag (`git push --delete origin vX.Y.Z`), and tag the new commit.
