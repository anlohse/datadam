# Security

datadam protects access with Redis-style passwords and ACL users, and can encrypt Redis-protocol connections
with TLS, optionally logging clients in by their certificate. This page covers each mechanism, how it applies
to the memcache port and to replication, and a hardening checklist.

## Contents

- [Defaults](#defaults)
- [Protected mode](#protected-mode)
- [Passwords: requirepass and AUTH](#passwords-requirepass-and-auth)
- [ACL users](#acl-users)
- [Rules](#rules)
- [How permissions are checked](#how-permissions-are-checked)
- [Categories](#categories)
- [Commands that return keys](#commands-that-return-keys)
- [Changing users at run time](#changing-users-at-run-time)
- [ACL LOG](#acl-log)
- [Keeping users in the config file](#keeping-users-in-the-config-file)
- [memcache connections](#memcache-connections)
- [TLS](#tls)
- [Replication credentials](#replication-credentials)
- [Logs that show commands](#logs-that-show-commands)
- [What is not supported](#what-is-not-supported)
- [Hardening checklist](#hardening-checklist)

## Defaults

Out of the box datadam listens on `bind 0.0.0.0`, port 6379 (Redis) and port 11211 (memcache), with no
password and no TLS. Every connection is logged in as the `default` user, which may run every command on every
key, so [protected mode](#protected-mode) accepts connections from the loopback interface only, until you set
a password. See the [hardening checklist](#hardening-checklist).

## Protected mode

As in Redis, `protected-mode yes` (the default) refuses clients from other machines while the `default` user
needs no password: a server started with the sample configuration on a machine with a public address is not
open to the internet by mistake.

- **Who is refused.** A connection to the Redis port, the TLS port or the memcache port whose peer is not a
  loopback address (`127.0.0.0/8`, `::1`, or an IPv4-mapped loopback). The Redis ports reply
  `-DENIED datadam is running in protected mode ...` with the ways out, and memcache replies
  `SERVER_ERROR datadam is running in protected mode ...`; then the connection closes.
- **Who passes.** Loopback clients always. On the TLS port, a client that presented a certificate the server
  verified (`tls-auth-clients yes` or `optional`). Everyone, as soon as the `default` user has a password
  (`requirepass`, or `ACL SETUSER default >...`) or is turned off (`ACL SETUSER default off`): every client must
  then log in anyway.
- **When it is decided.** When the connection is accepted. Setting a password or `CONFIG SET protected-mode no`
  applies to the next connections.
- **Docker.** The image sets `protected-mode no`, as the official Redis image does: a container's ports are
  reachable only where `docker run -p` publishes them, and the containers of a Docker network would otherwise
  be refused. Set `requirepass` before publishing a port on an untrusted network.
- **Replicas and Sentinel.** A replica, or a Sentinel, on another machine is an ordinary remote client: give
  the primary a password (and the replica `masterauth`), or turn protected mode off on a private network.

## Passwords: requirepass and AUTH

`requirepass` is the password of the `default` user, as in Redis. Set it in the config file or at run time:

```text
127.0.0.1:6379> CONFIG SET requirepass s3cret
OK
```

The value cannot contain spaces. Setting it replaces every password of the `default` user with this one
(`resetpass >s3cret`); setting it to the empty string makes `default` passwordless again (`nopass`).

Once the `default` user needs a password (or is `off`), every new connection must log in:

- Before logging in, only `AUTH`, `HELLO` and `QUIT` are accepted. Any other command replies
  `NOAUTH Authentication required.`
- Before logging in, a request larger than 16 KB is refused with
  `ERR Protocol error: unauthenticated request too big` and the connection is closed, so an anonymous client
  cannot make the server buffer large requests.
- `AUTH password` logs in as `default`; `AUTH username password` logs in as any user;
  `HELLO 3 AUTH username password` logs in and switches to RESP3 in one step. `HELLO` without `AUTH` on a
  connection that has not logged in replies `NOAUTH HELLO must be called with the client already
  authenticated...`.
- A wrong password, an unknown user or a user that is `off` replies
  `WRONGPASS invalid username-password pair or user is disabled.` The failure is counted in `auth_failures`
  (`INFO stats`) and recorded in the [ACL LOG](#acl-log). There is no delay or lockout after failures.
- `AUTH password` while the `default` user needs no password replies
  `ERR AUTH <password> called without any password configured for the default user. Are you sure your
  configuration is correct?`.
- Connections that are already logged in stay logged in when the password changes.

With `redis-cli`, pass the password in the environment rather than on the command line:

```bash
REDISCLI_AUTH=s3cret redis-cli -h db.internal PING
REDISCLI_AUTH=app-secret redis-cli -h db.internal --user app GET app:1
```

## ACL users

Users work as in Redis 7. `default` always exists; other users are created with `ACL SETUSER` or with `user`
lines in the config file:

```text
ACL SETUSER app on >app-secret ~app:* %R~shared:* +@all -@dangerous -@admin
ACL SETUSER reports on >reports-secret %R~* +@read +@connection -@dangerous
ACL SETUSER ops on >ops-secret allkeys +@all -config +config|get
```

- A new user starts with nothing: `off`, no password, no key patterns, no commands. It cannot log in until it
  is `on` and has a password (or `nopass`).
- `ACL SETUSER` on an existing user applies the new rules on top of its current ones. The rules are applied
  in order, all of them or none: one invalid rule replies
  `ERR Error in ACL SETUSER modifier '<rule>': <reason>` and leaves the user unchanged.
- User names cannot be empty or contain spaces.
- `ACL GETUSER`, `ACL LIST` and `ACL USERS` show users; `ACL DELUSER` removes them (not `default`:
  `ERR The 'default' user cannot be removed`). `ACL WHOAMI` names the logged-in user.

The `default` user can be restricted or disabled like any other. With `ACL SETUSER default off`, every
connection must log in as a named user.

## Rules

| Rule | Effect |
|---|---|
| `on`, `off` | Allow or forbid logging in as the user. `off` does not close connections already logged in. |
| `>password` | Add a password. Stored as a SHA-256 hash. Also clears `nopass`. |
| `<password` | Remove a password (`The password you are trying to remove from the user does not exist` if it is not there). |
| `#hash` | Add a password by its SHA-256 hash: exactly 64 lower-case hexadecimal characters. Also clears `nopass`. |
| `!hash` | Remove a password by its hash. |
| `nopass` | Any password is accepted; removes all passwords. |
| `resetpass` | Remove all passwords and `nopass`: nobody can log in as the user with a password. |
| `~pattern` | Keys matching the glob pattern may be read and written. |
| `%R~pattern` | Keys matching the pattern may only be read. |
| `%W~pattern` | Keys matching the pattern may only be written. `%RW~pattern` equals `~pattern`. |
| `allkeys` | Same as `~*`; replaces the other key patterns. |
| `resetkeys` | Remove all key patterns. |
| `+command`, `-command` | Allow or deny a command. |
| `+command\|subcommand`, `-command\|subcommand` | Allow or deny one subcommand. Only for the commands with subcommands: `CONFIG`, `CLIENT`, `ACL`, `CACHE` and `COMMAND`. |
| `+@category`, `-@category` | Allow or deny every command of a [category](#categories). |
| `allcommands`, `nocommands` | Same as `+@all` and `-@all`. |
| `&pattern`, `allchannels`, `resetchannels` | Pub/Sub channels the user may publish and subscribe to (glob); `allchannels` is `&*`. New users have none; the `default` user has all. See [Pub/Sub](pubsub.md#permissions). |
| `reset` | Back to a new user: `off`, no passwords, no keys, no channels, no commands. |

Keywords and command names are case-insensitive; passwords and key patterns are not. Command rules apply in
the order given, as in Redis: a later `+@category`, `-@category`, `+command` or `-command` replaces any
earlier subcommand rule of the commands it covers, and `+@all`/`-@all` start the command rules over. So
`-@dangerous +config|get` works, but `+config|get -@dangerous` loses the `config|get` exception again.

`ACL LIST` and `ACL GETUSER` show a user as the rules that rebuild it from scratch:

```text
127.0.0.1:6379> ACL LIST
1) "user app on #8b5d...e1f2 ~app:* %R~shared:* resetchannels +@all -@dangerous -@admin"
2) "user default on nopass ~* &* +@all"
```

## How permissions are checked

For a user that may run every command on every key (such as the initial `default` user), nothing is checked
beyond one atomic read per command. For any other user, every command is checked before it runs:

1. **The command.** If the user may not run it, the reply is
   `NOPERM User <name> has no permissions to run the '<command>' command`, or `'<command>|<subcommand>'`
   for a denied subcommand.
2. **Every key it names.** Each command knows which of its arguments are keys and whether it reads or writes
   them. `SINTERSTORE` needs write access to its destination and read access to its sources; `RENAME`,
   `SMOVE`, `LMOVE` and `RPOPLPUSH` need write access to both keys; `MGET` needs read access to every key. One
   key without the needed access refuses the whole command with `NOPERM No permissions to access a key`.

Refusals are counted in `acl_access_denied` (`INFO stats`) and recorded in the [ACL LOG](#acl-log).

Some consequences worth knowing:

- `PING`, `ECHO`, `CLIENT`, `HELLO` and `AUTH` are in `@connection`. A user without `+@connection` (or the
  individual commands) cannot `PING`, and cannot run `AUTH` or `HELLO` again on its connection to switch to
  another user: open a new connection instead. `AUTH`, `HELLO` and `QUIT` are always accepted on a connection
  that has not logged in yet.
- `ACL WHOAMI` and `ACL CAT` are allowed to every logged-in user.
- A command that is not in any category a user was granted is denied, as in Redis. `TIME` is only in
  `@fast`.

## Categories

`ACL CAT` lists the categories and `ACL CAT <category>` the commands in one. The categories and their
membership follow Redis, so rule sets written for Redis mean the same here, with datadam's additions:

| Category | Commands |
|---|---|
| `@read` | Commands that read keys: `GET`, `MGET`, `EXISTS`, `TTL`, `HGETALL`, `ZRANGE`, `LRANGE`, `SCAN`, `KEYS`, `DBSIZE`, the reading `JSON.*` commands, `IDX.QUERY`/`COUNT`/`GET`/`INFO`/`LIST`, `FT.SEARCH`/`INFO`/`_LIST`/`TAGVALS`... |
| `@write` | Commands that write keys: `SET`, `DEL`, `EXPIRE`, `HSET`, `ZADD`, `LPUSH`, `FLUSHALL`, the writing `JSON.*` commands, `IDX.CREATE`/`DROP`, `FT.CREATE`/`DROPINDEX`... |
| `@keyspace` | `DEL`, `UNLINK`, `EXISTS`, `TYPE`, the `EXPIRE` and `TTL` families, `PERSIST`, `RENAME`, `RENAMENX`, `KEYS`, `SCAN`, `DBSIZE`, `FLUSHDB`, `FLUSHALL`. |
| `@string`, `@hash`, `@set`, `@sortedset`, `@list` | The commands of each type, including `HSCAN`, `SSCAN` and `ZSCAN`. |
| `@json` | Every `JSON.*` command (datadam's category). |
| `@index` | Every `IDX.*` and `FT.*` command (datadam's category). |
| `@search` | An alias of `@index`. |
| `@pubsub` | `SUBSCRIBE`, `PSUBSCRIBE`, `SSUBSCRIBE`, the unsubscribe commands, `PUBLISH`, `SPUBLISH`, `PUBSUB`. |
| `@transaction` | `MULTI`, `EXEC`, `DISCARD`, `WATCH`, `UNWATCH`. |
| `@blocking` | `BLPOP`, `BRPOP`, `BLMOVE`, `BRPOPLPUSH`, `BLMPOP`, `BZPOPMIN`, `BZPOPMAX`, `BZMPOP`. |
| `@admin` | `CONFIG`, `ACL`, `SAVE`, `BGSAVE`, `CACHE`, `IDX.CREATE`, `IDX.DROP`, `FT.CREATE`, `FT.DROPINDEX`, `CDC.INFO`, `CDC.READ`, `REPLICAOF`, `SLAVEOF`, `ROLE`, `REPLCONF`, `REPLSYNC`. |
| `@dangerous` | `CONFIG`, `ACL`, `INFO`, `KEYS`, `FLUSHDB`, `FLUSHALL`, `SAVE`, `BGSAVE`, `IDX.CREATE`, `IDX.DROP`, `FT.CREATE`, `FT.DROPINDEX`, `CDC.READ`, `REPLICAOF`, `SLAVEOF`, `ROLE`, `REPLCONF`, `REPLSYNC`. |
| `@connection` | `PING`, `ECHO`, `QUIT`, `SELECT`, `AUTH`, `HELLO`, `CLIENT`, `COMMAND`. |
| `@fast`, `@slow` | As in Redis. |
| `@all` | Every command. |

Index and search definitions (`IDX.CREATE`, `FT.CREATE` and their drops) are `@admin` and `@dangerous`; their
queries are `@read`. `CDC.READ` is `@admin` and `@dangerous` because it shows the values written to any key.

`@scripting` holds `EVAL`, `EVALSHA`, `FCALL`, `SCRIPT`, `FUNCTION` and their `_RO` variants. Each command a
script runs is checked again, as the same user, with its own keys, so a script cannot do what its user may not;
see [Lua scripting](scripting.md#permissions).

Redis categories for features datadam does not have (`@stream`, `@bitmap`, `@hyperloglog`, `@geo`) are
accepted and contain no commands, so Redis rule sets such as `+@all -@stream` load unchanged.

`WATCH` needs read permission on its keys, and the commands queued after `MULTI` are checked (command and
keys) when they are queued: a refusal there makes `EXEC` reply `EXECABORT`. Channel refusals reply
`NOPERM No permissions to access a channel` and appear in `ACL LOG` with reason `channel`.

## Commands that return keys

Redis checks only the keys a command names, so a user limited to `~app:*` can still discover every key name
with `KEYS` or `SCAN`. datadam filters the keys that such commands find on their own: `KEYS`, `SCAN`,
`IDX.QUERY`, `IDX.COUNT`, `IDX.GET`, `FT.SEARCH` and `CDC.READ` return and count only the keys the user may
read. A user limited to `~app:*` sees only `app:` keys, and `IDX.COUNT` counts only those.

Not filtered: `DBSIZE` and `INFO keyspace` (a total), `IDX.INFO` and `FT.INFO` (index statistics),
`FT.TAGVALS` (the distinct values of a field, across all documents) and `FT.DROPINDEX ... DD`, which deletes
every document of the index whatever the user's key patterns. `FT.DROPINDEX` is `@admin` and `@dangerous`
and `INFO` is `@dangerous`, but `DBSIZE`, `IDX.INFO`, `FT.INFO` and `FT.TAGVALS` are plain `@read` commands:
grant `@index` with that in mind.

## Changing users at run time

- A change made with `ACL SETUSER` applies to connections already logged in as that user from their next
  command.
- A user turned `off` cannot log in again, but its open connections stay.
- Deleting a user (`ACL DELUSER`) closes its connections: their next command replies
  `ERR the user of this connection was deleted` and the connection is closed.
- Changing a user's passwords does not log out its open connections.

Users live in memory. Run `CONFIG REWRITE` to keep them across restarts (see
[below](#keeping-users-in-the-config-file)).

## ACL LOG

`ACL LOG` keeps the latest refusals and failed logins, newest first:

```text
ACL LOG            # the 10 newest entries
ACL LOG 50         # the 50 newest
ACL LOG RESET      # empty it
```

Each entry is a map with `count`, `reason` (`command`, `key` or `auth`), `context` (`toplevel`), `object`
(the command, `command|subcommand`, key, or `AUTH`), `username`, `age-seconds`, `client-info` (as in
`CLIENT INFO`, including `tls=yes|no`; `protocol=memcache` for the memcache port), `entry-id`,
`timestamp-created` and `timestamp-last-updated`. The same refusal (same reason, object and user) repeated
within a minute increments `count` on one entry instead of adding entries.

`acllog-max-len` (default 128, up to 100,000, changeable with `CONFIG SET`) caps the number of entries; 0
turns the log off. `INFO stats` shows the totals `auth_failures` and `acl_access_denied`.

## Keeping users in the config file

A `user` line in the config file defines a user from scratch (starting from `reset`), with the same rules as
`ACL SETUSER`:

```text
user default off
user app on >app-password ~app:* +@read +@write -@dangerous
user reports on #9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08 %R~* +@read -@dangerous
```

`#` gives the SHA-256 of the password instead of the password itself (here, of `test`), so the file does not
hold it in clear text. `requirepass` is applied first and `user` lines after it, so a `user default ...` line
wins over `requirepass`. An invalid rule stops the server at startup with `config: user <name>: <error>`.

`CONFIG REWRITE` writes the current users back into the file the server was started with: the existing `user`
lines are replaced by one line per user (passwords as `#hash`), and if the file had none they are added at the
end under `# Generated by CONFIG REWRITE`. The `default` user is written only when it differs from
`on nopass ~* &* +@all`. Without a config file, `CONFIG REWRITE` replies
`ERR The server is running without a config file`.

`CONFIG REWRITE` also writes `requirepass` and `masterauth` in clear text, and `CONFIG GET requirepass`
returns the password. Keep the config file readable only by the datadam process and its administrators, and
deny `CONFIG` to application users (it is `@admin` and `@dangerous`).

There is no separate ACL file: `aclfile`, `ACL SAVE` and `ACL LOAD` do not exist.

## memcache connections

The memcache text protocol has no authentication of its own. When the `default` user needs a password (or is
off), a memcache connection must first send `auth [user] <password>`:

```text
auth s3cret
OK
auth app app-secret
OK
```

A failed `auth` replies `CLIENT_ERROR authentication failed`, counts in `auth_failures` and is recorded in
the ACL LOG. Before a successful `auth`, only `version` and `quit` are accepted; any other command replies
`CLIENT_ERROR authentication required (send: auth <password>)` and closes the connection. The password crosses
the network in clear text: the memcache port has no TLS.

Once logged in, a memcache connection has the permissions of its user, checked as for the equivalent Redis
command and its keys:

| memcache command | Checked as | Key access |
|---|---|---|
| `get`, `gets` | `MGET` | read, every key |
| `gat`, `gats` | `GETEX` | write, every key |
| `set`, `replace`, `cas` | `SET` | write |
| `add` | `SETNX` | write |
| `append`, `prepend` | `APPEND` | write |
| `delete` | `DEL` | write |
| `incr`, `decr` | `INCRBY` | write |
| `touch` | `EXPIRE` | write |
| `flush_all` | `FLUSHALL` | none |
| `stats` | `INFO` | none |
| `version`, `verbosity`, `quit` | not checked | none |

Refusals reply `CLIENT_ERROR NOPERM user <name> may not run '<command>'` or
`CLIENT_ERROR NOPERM no permissions to access a key`. See [memcache](memcache.md).

## TLS

datadam can serve the Redis protocol over TLS (OpenSSL) on a separate port, next to the plain port or
instead of it. The memcache port and the replica's connection to its primary are always plain TCP.

### Options

| Option | Default | `CONFIG SET` | Meaning |
|---|---|---|---|
| `tls-port` | 0 (off) | no | Port for TLS connections. With `port 0`, only the TLS port listens. |
| `tls-cert-file` | (none) | yes | Server certificate chain, PEM. Required with `tls-port`. |
| `tls-key-file` | (none) | yes | Private key of the certificate, PEM. Required with `tls-port`. |
| `tls-ca-cert-file` | (none) | yes | CA certificates that sign client certificates, PEM. Required unless `tls-auth-clients no`. |
| `tls-auth-clients` | `yes` | yes | `yes`: clients must present a certificate signed by one of those CAs. `optional`: a certificate is verified if the client sends one. `no`: none is requested. |
| `tls-auth-clients-user` | `off` | yes | `cn`: a verified client certificate logs in as the ACL user named by its Common Name. |
| `tls-protocols` | `TLSv1.2,TLSv1.3` | yes | `TLSv1.2` and/or `TLSv1.3`, separated by commas or spaces. The lowest and highest given set the allowed range. |
| `tls-ciphers` | OpenSSL default | yes | TLS 1.2 cipher list, OpenSSL syntax. |
| `tls-ciphersuites` | OpenSSL default | yes | TLS 1.3 cipher suites, OpenSSL syntax. |

A configuration that cannot work is refused at startup and by `CONFIG SET`:

- `port 0 disables the plain port; set tls-port, or nothing listens`
- `tls-port needs tls-cert-file and tls-key-file`
- `tls-auth-clients yes/optional needs tls-ca-cert-file (or set tls-auth-clients no)`
- `tls-auth-clients-user cn needs client certificates (tls-auth-clients yes or optional)`

At startup, a certificate or key that cannot be loaded, a key that does not match the certificate, or an
invalid cipher list stops the server with a `TLS: ...` message. Because `tls-port` cannot change at run time,
TLS can only be turned on by restarting.

A TLS-only server with client certificates:

```text
port 0
tls-port 6380
tls-cert-file /etc/datadam/server.crt
tls-key-file /etc/datadam/server.key
tls-ca-cert-file /etc/datadam/ca.crt
tls-auth-clients yes
tls-protocols TLSv1.3
```

```bash
redis-cli -p 6380 --tls --cacert ca.crt --cert client.crt --key client.key PING
```

For a test setup, a private CA and certificates can be made with OpenSSL:

```bash
openssl req -x509 -new -nodes -newkey rsa:2048 -days 365 -subj "/CN=datadam-test-ca" \
  -keyout ca.key -out ca.crt
openssl req -new -nodes -newkey rsa:2048 -subj "/CN=localhost" -keyout server.key -out server.csr
printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\n' > server.ext
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 365 \
  -extfile server.ext -out server.crt
openssl req -new -nodes -newkey rsa:2048 -subj "/CN=app" -keyout client.key -out client.csr
openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 365 -out client.crt
```

### Logging in with a client certificate

With `tls-auth-clients-user cn` (which needs `tls-auth-clients yes` or `optional`), a client whose
certificate was verified and whose Common Name is the name of an existing ACL user that is `on` is logged in
as that user as soon as the handshake ends, without `AUTH` and whatever the user's passwords. A user can
therefore be certificate-only:

```text
ACL SETUSER app on resetpass ~app:* +@read +@write +@connection
```

If no such user exists, or it is `off`, the connection follows the normal rules: it is logged in as `default`
if `default` needs no password, and must `AUTH` otherwise.

### Replacing certificates without a restart

`CONFIG SET` of any `tls-*` option except `tls-port` builds a new TLS context, which new connections use;
connections already open keep the one they started with. The files are read and checked first: if one cannot
be loaded or the key does not match, `CONFIG SET` fails (`ERR CONFIG SET failed - TLS: ...`) and nothing
changes.

The files are read again whenever a `CONFIG SET` names a `tls-*` option, even with the value it already has.
There are two ways to rotate. Overwrite the files in place and name one of the options:

```text
CONFIG SET tls-cert-file /etc/datadam/server.crt
```

Or write the new files under new names and switch to them in one command, then `CONFIG REWRITE` to keep the
new paths for the next start:

```text
CONFIG SET tls-cert-file /etc/datadam/server-2026.crt tls-key-file /etc/datadam/server-2026.key
```

When certificate and key are overwritten in place, replace both before the `CONFIG SET`: the server checks that
the key matches the certificate, and refuses the change (keeping the old context) if it does not.

### Handshake timeout and statistics

- A client that has not completed the TLS handshake within 10 seconds is disconnected. This is fixed, not
  configurable.
- `INFO stats` shows `tls_connections` (completed handshakes) and `tls_handshake_failures` (failed or timed
  out handshakes).
- `CLIENT INFO` and the `client-info` of ACL LOG entries show `tls=yes` for TLS connections.

## Replication credentials

A replica logs in to its primary with `masteruser` and `masterauth` (both changeable with `CONFIG SET`; they
are used on the next connection to the primary). When `masterauth` is set, the replica sends
`AUTH <masteruser> <masterauth>`, or `AUTH <masterauth>` without `masteruser`; then `PING`,
`REPLCONF listening-port <port>` and `REPLSYNC <id> <offset>`. `masterauth` cannot contain spaces.

The primary's user for replicas needs only these commands; `REPLCONF` and `REPLSYNC` are `@admin` and
`@dangerous`, and no key patterns are required:

```text
ACL SETUSER replicator on >repl-secret +ping +replconf +replsync
```

The replication stream carries every key and every change, whatever the user's key patterns, so treat the
replication user as a full-access credential. By default the replica connects over plain TCP, so
credentials and data cross the network unencrypted. With `tls-replication yes` it connects to the primary's
TLS port instead, verifies the primary's certificate (CA and host name) and presents its own, which the
primary can require (`tls-auth-clients yes`) and use as its login (`tls-auth-clients-user cn`). See
[Replication](replication.md#replication-over-tls).

## Logs that show commands

`SLOWLOG` and `MONITOR` show the arguments of commands, so both are in `@admin` and `@dangerous`, and both hide
passwords: `AUTH` and `HELLO` appear with their name only, `ACL SETUSER` with the user name and `CONFIG SET`
with nothing past `SET`, each followed by `(redacted)`. Values written by other commands do appear: grant
them only to operators who may read the data.

`CLIENT` (including `CLIENT LIST` and `CLIENT KILL`) is in `@connection`, as one command for every
subcommand. To let a user see its own connection but not close others, deny the subcommand:
`ACL SETUSER app -client|kill -client|list`.

## What is not supported

- ACL selectors (`(...)` rules): `ACL GETUSER` always shows an empty `selectors` list.
- `ACL SAVE`, `ACL LOAD`, `ACL GENPASS`, `ACL DRYRUN`, and the `aclfile` option. Users are kept with
  `CONFIG REWRITE`.
- Commands for the `@stream`, `@bitmap`, `@hyperloglog` and `@geo` categories (the categories exist,
  empty).
- TLS on the memcache port.
- Redis's other TLS options (`tls-ca-cert-dir`, `tls-dh-params-file`, `tls-prefer-server-ciphers`,
  `tls-session-caching` and the like) and certificate revocation lists.
- `protected-mode`, `rename-command`, and limits on failed login attempts.

## Hardening checklist

1. **Bind to the right interface.** Set `bind` to a private address (or `127.0.0.1`), and firewall the ports.
   Keep `protected-mode yes` unless the ports are reachable from trusted networks only.
2. **Require a login.** Set `requirepass`, or better, create named users and run
   `ACL SETUSER default off` so that every client must log in as a user with only the permissions it needs.
3. **Give applications narrow users.** Key patterns for their prefixes, `-@dangerous -@admin`, and
   `%R~` patterns for read-only consumers. Keep `CONFIG`, `ACL`, `KEYS`, `FLUSHALL`, `CDC.READ`, index
   definitions and `REPLICAOF` for administrators.
4. **Turn off what you do not use.** `memcache-port 0` if no client speaks memcache: its traffic, password
   included, is plain text.
5. **Encrypt.** Use `tls-port` (with `port 0` for TLS only) and `tls-auth-clients yes` where clients can
   present certificates; consider `tls-protocols TLSv1.3`.
6. **Protect the config file and data directory.** The config file can hold `requirepass` and `masterauth`
   in clear text; the data directory holds every value. Make both readable only by the datadam user.
7. **Isolate replication.** Give replicas their own user (`+ping +replconf +replsync`) and keep the link on a
   private network or tunnel.
8. **Watch the counters.** Alert on `auth_failures`, `acl_access_denied` and `tls_handshake_failures`
   (`INFO stats`), and review `ACL LOG`.
9. **Persist your users.** Run `CONFIG REWRITE` after changing users, or they are lost on restart.
