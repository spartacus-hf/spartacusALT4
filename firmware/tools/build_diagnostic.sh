#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
DEBUG_DIR="${ROOT}/Debug"
OUT_DIR="${ROOT}/Diagnostic"
TMP_DIR="${ROOT}/.Diagnostic.tmp.$$"
MARKER=".generated_diagnostic"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

[[ -d "${DEBUG_DIR}" && ! -L "${DEBUG_DIR}" ]] || { echo "FAIL safe Debug directory missing" >&2; exit 1; }
if [[ -e "${OUT_DIR}" ]]; then
  [[ -d "${OUT_DIR}" && ! -L "${OUT_DIR}" && -f "${OUT_DIR}/${MARKER}" &&
     ! -L "${OUT_DIR}/${MARKER}" ]] || {
    echo "FAIL refusing unmanaged or symlink output: ${OUT_DIR}" >&2; exit 1;
  }
fi
[[ ! -e "${TMP_DIR}" ]] || { echo "FAIL temporary path already exists: ${TMP_DIR}" >&2; exit 1; }
cleanup() { rm -rf -- "${TMP_DIR}"; }
trap cleanup EXIT
mkdir -- "${TMP_DIR}"
touch "${TMP_DIR}/${MARKER}"

while IFS= read -r -d '' src; do
  rel="${src#${DEBUG_DIR}/}"
  [[ "${rel}" != ../* && "${rel}" != /* ]] || { echo "FAIL unsafe metadata path" >&2; exit 1; }
  mkdir -p -- "${TMP_DIR}/$(dirname "${rel}")"
  cp -- "${src}" "${TMP_DIR}/${rel}"
done < <(find "${DEBUG_DIR}" -type f \( -name makefile -o -name objects.mk -o \
  -name sources.mk -o -name objects.list -o -name subdir.mk \) -print0)

while IFS= read -r -d '' mk; do
  sed -i -e 's/-DDEBUG /-DDEBUG -DMOD_DEEP_DIAG_ENABLE=1 /g' -e 's/ -O0 / -O2 /g' "${mk}"
done < <(find "${TMP_DIR}" -name subdir.mk -type f -print0)

if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
  TOOL_DIR="$(find /opt/stm32cubeide/plugins -path '*gnu-tools-for-stm32*tools/bin' -type d 2>/dev/null | sort | tail -n 1 || true)"
  [[ -n "${TOOL_DIR}" ]] && export PATH="${TOOL_DIR}:${PATH}"
fi
command -v arm-none-eabi-gcc >/dev/null 2>&1 || { echo "FAIL STM32 compiler not found" >&2; exit 1; }
unset MAKEFLAGS MFLAGS
MAKE_JOBSERVER_ARGS=()
if make --help 2>/dev/null | grep -q -- '--jobserver-style'; then
  MAKE_JOBSERVER_ARGS=(--jobserver-style=pipe)
fi
# The copied Debug post-build recipe packages Debug/alt2.elf. Build the
# derived ELF first, then package it only after it reaches its final path.
make "${MAKE_JOBSERVER_ARGS[@]}" -C "${TMP_DIR}" -j"${JOBS}" main-build

if [[ -e "${OUT_DIR}" ]]; then rm -rf -- "${OUT_DIR}"; fi
mv -- "${TMP_DIR}" "${OUT_DIR}"
trap - EXIT
python3 "${ROOT}/tools/package_firmware.py" --build-dir Diagnostic \
  --output-dir "${ROOT}/dist/diagnostic"
