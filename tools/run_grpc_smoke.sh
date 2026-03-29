#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV_DIR="${HOME}/.clawspan-grpc-smoke-venv"

if [[ ! -d "${VENV_DIR}" ]]; then
  python3 -m venv "${VENV_DIR}"
fi

"${VENV_DIR}/bin/python" -m pip install -U pip >/dev/null
"${VENV_DIR}/bin/python" -m pip install -U grpcio protobuf >/dev/null

"${VENV_DIR}/bin/python" "${REPO_ROOT}/tools/grpc_smoke.py" "$@"
