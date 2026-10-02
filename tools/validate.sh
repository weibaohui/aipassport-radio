#!/usr/bin/env bash
# 应用仓门禁封装:真正的实现在框架 submodule 里,本文件只负责定位并转发。
set -euo pipefail
app_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
framework="${app_root}/components/framework"
if [[ ! -x "${framework}/tools/validate.sh" ]]; then
    echo "ERROR: 框架未就位。请先执行:git submodule update --init --recursive" >&2
    exit 1
fi
exec "${framework}/tools/validate.sh" --project-root "${app_root}" "$@"
