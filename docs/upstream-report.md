# Zephyr 상류 보고 문안 — `ARM64_BOOT_DISABLE_DCACHE` 기본값

**상태:** 초안. 사용자가 Zephyr GitHub 에 올리면 된다(제가 계정으로 올릴 수
없다). 아래 본문을 그대로 붙이거나 줄여 쓸 수 있다.

**보낼 곳:** https://github.com/zephyrproject-rtos/zephyr/issues
**분류:** `area: ARM64`, `bug` 또는 `enhancement`

---

## 제목 (영문)

`arm64: booting from EL2 with caches on silently hangs unless ARM64_BOOT_DISABLE_DCACHE is manually enabled`

## 본문 (영문)

### Summary

On an out-of-tree Cortex-A55 board (Rockchip RK3566) loaded by U-Boot's `go`
command, enabling a perfectly ordinary option — `CONFIG_INIT_STACKS=y` —
made the board hang with **zero console output**. Not a fault message, not a
single character. It took three hardware round trips and most of a day to
find that the real cause was unrelated to `INIT_STACKS`:
`CONFIG_ARM64_BOOT_DISABLE_DCACHE` was off.

### Why it is easy to hit and hard to diagnose

1. **U-Boot's `go` does not clean or disable caches.** Unlike `booti`/`bootm`,
   it jumps straight to the image. So Zephyr is entered at EL2 with MMU and
   D-cache still enabled. Loading a bare image with `go` is a common
   bring-up path.

2. **`arch/arm64/core/reset.S` paints the ISR stack with 0xAA before
   `switch_el`** (when `CONFIG_INIT_STACKS=y`). Those 4 KiB become dirty
   cache lines at EL2. After `eret` to EL1, `SCTLR_EL1` is at its reset
   value, so MMU/C are off and subsequent stack accesses bypass the cache
   and go to DRAM. When a stale dirty line is later evicted it overwrites
   what EL1 wrote — the return address among it.

3. **The guard exists but is off by default.**
   `ARM64_BOOT_DISABLE_DCACHE` (`arch/arm64/core/Kconfig:446`) describes this
   exact hazard:

   > To make it safe, if data cache is enabled in case of Zephyr is booting
   > from EL2, enable this option, it will clean and invalidate all data
   > cache and then disable data cache...

   But it has no `default`, and it `depends on ARM64_DCACHE_ALL_OPS`, which
   is also off by default. So **the option does not even appear in
   menuconfig** until you first enable its dependency. A user hitting the
   hang has no way to stumble onto it.

4. **The symptom gives nothing to go on.** The hang is before any console
   init, so there is no output, no fault dump, no `k_panic`. We only located
   it by writing synchronous `uart_poll_out` markers into `SYS_INIT` hooks
   and seeing that not even the `PRE_KERNEL_2` marker printed.

### Evidence

Same board, same image, only these two lines toggled:

| build | `INIT_STACKS` | `ARM64_DCACHE_ALL_OPS` + `ARM64_BOOT_DISABLE_DCACHE` | result |
|---|---|---|---|
| A | y | (off) | **silent hang** |
| B | y | y | boots, shell works |

`INIT_STACKS` itself is innocent — it is only the trigger that dirties
enough cache lines for the problem to show. With caches disabled at EL2 the
same option works fine.

The same root cause also explained a second symptom we had written off as
"unusable on this board": `CONFIG_FRAME_POINTER=y` (which pulls in
`ARCH_HAS_STACKWALK` → `ARCH_STACKWALK` → `EXCEPTION_STACK_TRACE`) hung
identically. It works once the caches are handled.

### Suggestions (any one would have saved us the day)

1. **Make it the default where it is safe**, e.g.
   `default y if ARM64_DCACHE_ALL_OPS` — or have `ARM64_BOOT_DISABLE_DCACHE`
   `select ARM64_DCACHE_ALL_OPS` so the option is visible on its own.
2. **Put the symptom in the help text**, not just the remedy — something
   like "If the bootloader leaves caches enabled (e.g. U-Boot's `go`), not
   enabling this can hang the kernel before any console output."
3. Optionally, **detect it at runtime**: at EL2, if `SCTLR_EL2.C` is set and
   this option is off, that is a configuration that cannot work. A compile-
   time warning is impossible (it depends on the loader), but
   `z_arm64_el2_init` could clean/invalidate unconditionally — the cost is
   one cache maintenance pass at boot.

### Environment

- Zephyr 4.4.99 (`v4.4.0-3902-gfb0fa3dd488d`)
- Out-of-tree board: Radxa ZERO 3E, Rockchip RK3566, Cortex-A55 r2p0
  (`MIDR 0x412fd050`), booted by vendor U-Boot via `go`
- `CONFIG_ARM64_VA_BITS=32`, `PA_BITS=32`, `MMU=y`, `MAX_XLAT_TABLES=16`

### Not reproducible under QEMU (verified)

`qemu_cortex_a53`, same Zephyr tree, `samples/hello_world` built with
`-DCONFIG_INIT_STACKS=y`:

```
$ grep -E '^CONFIG_INIT_STACKS|ARM64_BOOT_DISABLE_DCACHE' build/zephyr/.config
CONFIG_INIT_STACKS=y
# (ARM64_BOOT_DISABLE_DCACHE not present — ARM64_DCACHE_ALL_OPS is off,
#  so the symbol is invisible, exactly as on our board)

$ west build -t run
*** Booting Zephyr OS build v4.4.0-3902-gfb0fa3dd488d ***
Hello World! qemu_cortex_a53/qemu_cortex_a53
```

It boots fine. QEMU starts from a clean reset state with caches off, so the
precondition — entered at EL2 **with caches already on** — never occurs.
This is consistent with the analysis: the bug only bites when a bootloader
hands over with caches enabled, which is why it is invisible in CI.

---

## 올리기 전 확인할 것

- [x] QEMU 에서 `INIT_STACKS=y` 가 정상인지 확인했다 (2026-11-06) —
      `qemu_cortex_a53` + `hello_world` 가 같은 설정으로 정상 부팅한다
- [ ] 이미 같은 이슈가 있는지 검색 — `ARM64_BOOT_DISABLE_DCACHE`,
      `EL2 cache hang`, `INIT_STACKS arm64 hang`
- [ ] 보드 포팅이 out-of-tree 임을 분명히 적는다 (인트리 보드가 아니므로
      재현 코드를 제공할 수 없다)

## 우리 쪽 근거 문서

- [hardware-verified.md](hardware-verified.md) — "INIT_STACKS 무음 해결",
  "역추적 복구" 절. 실기 3회 + 재현 표
- [trm-grounding.md](trm-grounding.md) §7 — 기전 분석과 기각된 가설들
- [bringup-rules.md](bringup-rules.md) 규칙 4 — "유죄 입증" 과 "기전 설명" 을
  구분해 적어 둔 것이 이 수정을 가능하게 했다
