# Documentation

Technical documentation of the validity-0088 driver. For building and
installing, start with the [README](../README.md) in the repository root.

| Document | Contents |
|---|---|
| [architecture.md](architecture.md) | Source files, how a scan flows through the driver, the state machines, where data is stored |
| [protocol.md](protocol.md) | USB endpoints, TLS framing, opcodes, session preparation, capture, image record, status codes |
| [capture-program.md](capture-program.md) | Format of the capture program, all record types, sensor calibration, `calibration.bin` |
| [matcher.md](matcher.md) | SIFT matcher, print formats, verify, identify, print updates, threshold |
| [print-encryption.md](print-encryption.md) | Design of the encrypted print templates, keys, systemd credential, failure behaviour |
| [evaluation.md](evaluation.md) | Measuring match quality with scan dumps and `tools/scan-eval.c`; results |
| [packaging.md](packaging.md) | `build-libfprint.sh` and `setup-fprintd.sh` in detail; systemd notes |
| [configuration.md](configuration.md) | Environment variables |
| [licensing.md](licensing.md) | Licence, copyright holders, third-party software, interoperability |
| [testing.md](testing.md) | The test suite (`tests/run.sh`), artificial fingerprints, what the tests cover |

Conventions: hexadecimal numbers are written `0x..` or as plain wire bytes
(`6f 00 0e …`); "tap" is one finger placement on the sensor; a "print" is
one enrolled finger (several taps).
