# qnx-micro

QNX Neutrino-style microkernel in 7 C++26 modules.

## What

- Synchronous message-passing IPC (send/receive/reply rendezvous)
- Priority-inheritance scheduling
- Capability-based security
- PPS (Persistent Publish/Subscribe) namespace
- BSD-2-Clause on all code

## Build

```
cmake -S . -B build \
  -DCMAKE_CXX_COMPILER=$(brew --prefix llvm@22)/bin/clang++
cmake --build build
```

Requires Clang 22+ with C++26 module support.

## Test

```
cmake --build build --target test
```

67 test cases, 252 assertions (Catch2).

## Verification

Z3 certificates in `proof/certs/`:

- **qnx_ipc.txt** — 9 assertions UNSAT (at most one running, send-blocked implies message, no self-message, priority inheritance, etc.)
- **qnx_capability.txt** — 5 assertions UNSAT

## License

BSD-2-Clause. See [LICENSE](LICENSE).

AI use prohibited. See [LICENSE-AI](LICENSE-AI.md).
