# Dependency ledger (dep)

Version pairing between this app and the underlying framework
(aipassport-fw). The framework is mounted as a git submodule at
`components/framework`. After every framework commit bump that passes
full-device verification, add one row here — otherwise there is no record of
which combination was ever verified. Chinese version:
[dep.zh_CN.md](dep.zh_CN.md).

## Verified pairings

| Date | App (aipassport-radio) | Framework (aipassport-fw) | Notes |
| --- | --- | --- | --- |
| 2026-10-05 | `9fed91e` | `7c85191` | Provisioning rework / portal truncation fix / two-phase scan / bar smoothing / volume row; community rev 2053 |

- Full-device image: `build/FoloToy-AI-Passport-full.bin` (rebuilt per release,
  flashed at 0x0).
- Runtime check: on-device Settings → Device info, or MCP `get_device_info`,
  reports `framework <fw-short-hash>.<date> | app <app-short-hash>`; it must
  match a row above.

## Rules

1. After updating the framework submodule
   (`git -C components/framework fetch && pull`), rebuild and verify on the
   device; only then commit the submodule pointer and register the row here.
2. A commit that changes the submodule pointer updates this ledger in the
   same commit.
3. Rollback = point the submodule back at the framework commit from the table
   and re-flash the device.
4. Framework-side releases do not change this file; the pairing table above is
   the source of truth.
