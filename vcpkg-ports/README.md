# vcpkg overlay ports

Ports that take precedence over the manifest's `builtin-baseline` (`vcpkg.json`: `vcpkg-configuration`), for a
dependency that needs a newer version than the baseline has without moving every other one.

| Port | Version | Why | From |
|---|---|---|---|
| `openssl` | 3.6.5 | The baseline's 3.6.1 lacks the security fixes of 3.6.2 to 3.6.5 (among them the TLS handshake buffer allocation, "HollowByte"). | [microsoft/vcpkg@e182cb4](https://github.com/microsoft/vcpkg/tree/e182cb4dd2df2ab02f66a1aabd5f35bbdc9522c7/ports/openssl), unchanged |

Remove a port here when the baseline moves past its version.
