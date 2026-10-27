#!/usr/bin/env python3
"""Fail closed on the unqualified generic firmware route; no input/device I/O."""
if __name__ == '__main__':
    raise SystemExit(
        'j716f preflight: generic board.toml firmware/bootimg route is not '
        'qualified for the selected native logger. Use the dedicated '
        'prepare-native-firmware.py -> package-native-image.py path documented '
        'in docs/NATIVE_MAINLINE.md. Do not bypass this gate with skip-preflight. '
        'The historical TzDxe preflight is archived, not a current prerequisite.'
    )
