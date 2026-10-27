# syntax=docker/dockerfile:1
#
# Linux build, tests and runtime image.
#   docker build --target test -t datadam:test .   # compile + unit + integration tests
#   docker build -t datadam .                      # slim runtime image
#   docker run -p 6379:6379 -p 11211:11211 -v datadam-data:/var/lib/datadam datadam

FROM ubuntu:24.04 AS toolchain
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git curl zip unzip tar pkg-config ca-certificates python3 \
    && rm -rf /var/lib/apt/lists/*

# Optional extra root CA for networks/antivirus that intercept HTTPS:
#   docker build --secret id=extra_ca,src=my-root-ca.crt ...
# It is trusted only in the build stages, never in the runtime image.
RUN --mount=type=secret,id=extra_ca \
    if [ -s /run/secrets/extra_ca ]; then \
      cp /run/secrets/extra_ca /usr/local/share/ca-certificates/extra-ca.crt && update-ca-certificates; \
    fi

# Same vcpkg commit as the manifest's builtin-baseline.
ARG VCPKG_COMMIT=77df67cfff9c12ccfdb52284e07c87c75092f723
ENV VCPKG_ROOT=/opt/vcpkg VCPKG_DISABLE_METRICS=1
RUN git init -q "$VCPKG_ROOT" && cd "$VCPKG_ROOT" \
    && git remote add origin https://github.com/microsoft/vcpkg.git \
    && git fetch -q --depth 1 origin "$VCPKG_COMMIT" && git checkout -q FETCH_HEAD \
    && ./bootstrap-vcpkg.sh -disableMetrics

# Dependencies get their own layer: it is rebuilt only when vcpkg.json changes.
FROM toolchain AS deps
WORKDIR /src
COPY vcpkg.json .
# RocksDB translation units are memory hungry; cap parallelism for small Docker VMs.
ARG BUILD_JOBS=8
ENV VCPKG_MAX_CONCURRENCY=${BUILD_JOBS}
RUN --mount=type=cache,target=/root/.cache/vcpkg \
    "$VCPKG_ROOT/vcpkg" install --triplet x64-linux --x-install-root=/src/build/vcpkg_installed

FROM deps AS build
COPY . .
RUN cmake --preset linux && cmake --build --preset linux --parallel "$VCPKG_MAX_CONCURRENCY"

FROM build AS test
RUN ctest --preset linux && sh tests/run_integration.sh build/linux/datadam

# The unit and integration tests again, built with AddressSanitizer and
# UndefinedBehaviorSanitizer (leaks included): docker build --target asan .
FROM deps AS asan
COPY . .
ENV ASAN_OPTIONS=abort_on_error=1:detect_stack_use_after_return=1:strict_string_checks=1     UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
RUN cmake --preset linux-asan && cmake --build --preset linux-asan --parallel "$VCPKG_MAX_CONCURRENCY"     && ctest --preset linux-asan && sh tests/run_integration.sh build/linux-asan/datadam

# The unit tests built with ThreadSanitizer, the dependencies too (a triplet
# of our own, so their accesses are seen):  docker build --target tsan .
FROM toolchain AS tsan-deps
WORKDIR /src
COPY vcpkg.json .
COPY triplets/ triplets/
ARG BUILD_JOBS=8
ENV VCPKG_MAX_CONCURRENCY=${BUILD_JOBS}
RUN --mount=type=cache,target=/root/.cache/vcpkg     "$VCPKG_ROOT/vcpkg" install --triplet x64-linux-tsan --overlay-triplets=triplets       --x-install-root=/src/build/vcpkg_installed_tsan

FROM tsan-deps AS tsan
COPY . .
ENV TSAN_OPTIONS=second_deadlock_stack=1:history_size=4
RUN cmake --preset linux-tsan && cmake --build --preset linux-tsan --parallel "$VCPKG_MAX_CONCURRENCY"     && ctest --preset linux-tsan

# The libFuzzer targets (fuzz/), built with Clang, ASan and UBSan:
#   docker build --target fuzz -t datadam:fuzz .
#   docker run --rm datadam:fuzz build/linux-fuzz/fuzz_resp -max_total_time=60 fuzz/corpus/resp
FROM deps AS fuzz
RUN apt-get update && apt-get install -y --no-install-recommends clang-18 libclang-rt-18-dev llvm-18 && rm -rf /var/lib/apt/lists/*
COPY . .
RUN cmake --preset linux-fuzz && cmake --build --preset linux-fuzz --parallel "$VCPKG_MAX_CONCURRENCY"

FROM ubuntu:24.04 AS runtime
COPY --from=build /src/build/linux/datadam /usr/local/bin/datadam
COPY datadam.conf /etc/datadam/datadam.conf
# Protected mode off, as in the official Redis image: a container's ports are
# reachable only where they are published (-p). Set requirepass before
# publishing them to an untrusted network.
RUN sed -i 's/^protected-mode yes/protected-mode no/' /etc/datadam/datadam.conf
# The server may rewrite its config file (CONFIG REWRITE), so it owns /etc/datadam.
RUN useradd --system --home /var/lib/datadam datadam \
    && mkdir -p /var/lib/datadam && chown datadam /var/lib/datadam /etc/datadam /etc/datadam/datadam.conf
USER datadam
VOLUME /var/lib/datadam
EXPOSE 6379 11211
ENTRYPOINT ["datadam", "/etc/datadam/datadam.conf", "--dir", "/var/lib/datadam"]
