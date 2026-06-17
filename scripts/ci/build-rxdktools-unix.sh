#!/usr/bin/env bash
# Placeholder for future Linux/macOS RXDK Tools builds.
# Enable the corresponding matrix entry in .github/workflows/build-rxdktools.yml when ready.

set -euo pipefail

platform="$(uname -s)"
echo "::warning::RXDKTools CI is not implemented on ${platform} yet."
echo "Implement this script and add the platform to the workflow matrix."
exit 1
