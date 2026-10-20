# Licensing

The project is published under the GNU Lesser General Public License version
2.1 or later (LGPL-2.1-or-later), like upstream libfprint; full text in
[`LICENSE`](../LICENSE). Every source file and script carries an
`SPDX-License-Identifier: LGPL-2.1-or-later` line (checked by
`tests/check-repo.sh`).

**Copyright holders.** © 2026 VisorCraft LLC (original driver) and © 2026
Radek Sipka (this fork: sensor calibration, SIFT matcher, identify, print
encryption, tools, tests and packaging). The git history records who wrote
what.

**Third-party software.** The driver contains no code from other projects. It
is compiled into libfprint (LGPL-2.1-or-later) and uses GLib
(LGPL-2.1-or-later) and OpenSSL 3 (Apache-2.0). Apache-2.0 is compatible with
version 3 of the LGPL, which the "or later" clause allows, so the combination
can be distributed; upstream libfprint links OpenSSL the same way. The SIFT
matcher is an own implementation of the published algorithm; the SIFT patent
(US 6,711,293) expired in March 2020.

**Interoperability.** The USB protocol was worked out by observing the
device's communication and by studying the Windows driver, for the sole
purpose of making the device work on Linux. No Windows driver code is
included. The repository contains protocol data observed on the USB link
(initialisation and host-part payloads, the generic capture-program
skeleton), because the device does not work without it. It holds no sensor
calibration and no data of any particular device; device-specific and
biometric files are excluded by the repository rules (`.gitignore`,
`tests/check-repo.sh`).
