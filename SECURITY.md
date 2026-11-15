# Security policy

## Supported versions

Security fixes go to the latest minor release (a new patch release) and to `main`. While datadam is at 0.x,
older minor releases are not patched: upgrade (see [docs/releases.md](docs/releases.md)).

## Reporting a vulnerability

Please do not open a public issue. Report it privately through GitHub:
[Security → Report a vulnerability](https://github.com/anlohse/datadam/security/advisories/new).

Say what an attacker needs (network access to the port, a logged-in user and its permissions, a crafted
file...), what they can do, and how to reproduce it: the commands, the configuration, the version
(`datadam --version`). You will get an answer within a week; the fix, a release and an advisory follow, and the
advisory credits you unless you prefer otherwise.

## What datadam protects

How to configure authentication, ACLs, TLS and protected mode is in [docs/security.md](docs/security.md).
In scope: anything a client can make the server do beyond what its user's permissions allow, crash it, or make
it exhaust memory or disk out of proportion to the request; reading or changing data without permission; and
the release artifacts and images. Out of scope: a server reachable from an untrusted network with protected
mode off and no password, which serves everyone by design.
