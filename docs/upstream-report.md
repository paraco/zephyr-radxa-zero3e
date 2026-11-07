# Zephyr 상류 보고 문안 — 5건

**상태: 원고 완성. 올림 여부 미확정 (2026-11-07).**

사용자가 올렸다고 했으나 **확인되지 않았다** — `gh issue list`·`gh pr list`·
`gh search issues --author paraco` 모두 결과가 비었고(계정은 `paraco` 로 확인),
받은 번호 `#12348`~`#12350` 은 **2019년에 병합된 다른 PR** 들이었다.
검색 색인이 늦었을 수도 있고, 제출 버튼 전에 닫혔을 수도 있다.
이슈 URL 을 확보하면 각 보고 머리에 적는다.

이슈 번호·링크는 아직 이 파일에 적지 않았다 — 받으면 각 보고 머리에 적는다.
상류의 답변이 오면 그 내용도 여기에 붙인다(특히 보고 3 의 DT 모순에 대한
판단은 우리 `app.overlay` 의 LEVEL 재정의를 계속 둘지에 영향을 준다).

**보낼 곳:** https://github.com/zephyrproject-rtos/zephyr/issues
**버전:** `v4.4.0-3902-gfb0fa3dd488d` (`dev` 브랜치)

| # | 제목 | 분류 | 세기 |
|---|---|---|---|
| 1 | EL2 에서 캐시를 켠 채 부팅하면 조용히 멈춘다 (`ARM64_BOOT_DISABLE_DCACHE`) | `area: ARM64` | 실기 3회, QEMU 비재현 확인 |
| 2 | `SCHED_THREAD_USAGE_AUTO_ENABLE` 을 끌 수 없다 (빌드 깨짐) | `area: Kernel` | 빌드 로그 |
| 3 | Rockchip UART 인터럽트 트리거가 트리 안에서 모순되고, ns16550 이 쓰지 않는 포트의 IRQ 를 켠다 | `area: UART`, `area: Devicetree` | 실기 실측 + 트리 내부 모순 |
| 4 | `ggml_backend_sched_new` 가 `malloc` 결과를 확인하지 않는다 | (llama.cpp) | 실기 할당 실패 |
| 5 | **ns16550 이 DesignWare busy-detect 를 지우지 않아 사이클의 77% 를 잃는다** | `area: UART` | **실기 PMU 실측 + 양성·음성 대조** |

3번이 가장 쓸모 있을 것이다 — **트리 안에서 같은 UART 를 서로 다르게
선언한 곳**을 찾았고, 그것이 실기에서 부팅 불가로 이어지는 경로를 끝까지
추적했다.

---

# 보고 1 — `ARM64_BOOT_DISABLE_DCACHE`

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

---

# 보고 2 — `SCHED_THREAD_USAGE_AUTO_ENABLE` 을 끌 수 없다

**분류:** `area: Kernel`, `bug`

## 제목 (영문)

`kernel: CONFIG_SCHED_THREAD_USAGE_AUTO_ENABLE=n does not build`

## 본문 (영문)

### Summary

`CONFIG_SCHED_THREAD_USAGE_AUTO_ENABLE` is a `bool` with `default y`
(`kernel/Kconfig`), so it looks like something a user may turn off. Setting it
to `n` does not build:

```
kernel/init.c:401:17: error: 'CONFIG_SCHED_THREAD_USAGE_AUTO_ENABLE' undeclared
  (first use in this function); did you mean 'CONFIG_SCHED_THREAD_USAGE_ALL'?
```

### Why

Two places use the symbol as a *value* rather than through `IS_ENABLED()`:

- `kernel/init.c:401` — `_kernel.cpus[id].usage->track_usage = CONFIG_SCHED_THREAD_USAGE_AUTO_ENABLE;`
- `kernel/thread.c:718` — same pattern

For a Kconfig `bool` set to `n`, `autoconf.h` does not define the macro at
all, so the expression references an undeclared identifier.

### Why it matters

We hit this while bisecting a boot failure. `THREAD_RUNTIME_STATS` brings in
three sub-options; the natural way to narrow the cause is to turn them off one
at a time. `AUTO_ENABLE` is the only one that *cannot* be turned off, and the
failure mode is a confusing compile error rather than a Kconfig message. It
cost a round trip to discover that the knob is not a knob.

### Suggestions (any one is enough)

1. Use `IS_ENABLED(CONFIG_SCHED_THREAD_USAGE_AUTO_ENABLE)` at both sites.
2. Make it an `int` (0/1) instead of a `bool`.
3. If it is genuinely not meant to be disabled, drop the symbol or mark it
   with a help text that says so.

### Environment

Zephyr `v4.4.0-3902-gfb0fa3dd488d`, `aarch64`, out-of-tree Cortex-A55 board,
`CONFIG_USE_SWITCH=y`, `CONFIG_SMP=n`.

---

# 보고 3 — Rockchip UART 인터럽트: 트리 내부 모순 + 쓰지 않는 포트의 IRQ

**분류:** `area: UART`, `area: Devicetree`, `bug`

이것은 두 부분이고, 하나로 올려도 되고 나누어도 된다. (a) 는 사실 확인만으로
고칠 수 있고, (b) 는 설계 판단이 필요하다.

## 제목 (영문)

`dts/arm64/rockchip: UART interrupt trigger type is inconsistent in-tree (EDGE vs LEVEL for the same UART), and ns16550 enables the IRQ line for instances nothing uses`

## 본문 (영문)

### (a) The in-tree Rockchip DTs contradict each other

| file | node | trigger |
|---|---|---|
| `dts/arm64/rockchip/rk3399.dtsi:95` | `uart2: serial@ff1a0000` | `IRQ_TYPE_LEVEL` |
| `dts/arm64/rockchip/rk3568.dtsi:81` | `uart2: serial@fe660000` | **`IRQ_TYPE_EDGE`** |
| `dts/arm64/rockchip/rk3588.dtsi:79` | `uart2: serial@feb50000`, `GIC_SPI 333` | **`IRQ_TYPE_EDGE`** |
| `dts/arm64/rockchip/rk3588s.dtsi:98` | `uart2: serial@feb50000`, `GIC_SPI 333` | `IRQ_TYPE_LEVEL` |

The last two describe **the same UART at the same address with the same SPI
number** and disagree. They cannot both be right.

Linux declares these level-high:
`arch/arm64/boot/dts/rockchip/rk356x-base.dtsi` and the rk3588 equivalents use
`IRQ_TYPE_LEVEL_HIGH` for the DW_apb_uart instances.

This is not cosmetic: `drivers/interrupt_controller/intc_gicv3.c` writes the
GIC `ICFGR` from this flag, so the wrong value configures the SPI as edge.

### (b) ns16550 enables the IRQ line for unused instances, and its ISR can be a no-op

With `CONFIG_UART_INTERRUPT_DRIVEN=y`, `drivers/serial/uart_ns16550.c:1947`
calls `irq_enable(DT_INST_IRQN(n))` for **every** enabled instance, whether or
not anything ever installs a callback.

If nothing installs a callback, `uart_ns16550_isr()`
(`drivers/serial/uart_ns16550.c:1405-1416`) does nothing at all — it does not
even read IIR — **unless** `CONFIG_UART_NS16550_DW8250_DW_APB=y`, in which case
it reads IIR and, for `IID == 0x7` (DesignWare busy detect), reads USR to clear
it.

On Rockchip (the UART *is* a Synopsys DW_apb_uart) that combination is a trap:

- DesignWare raises a **busy detect** interrupt when LCR is written while the
  UART is busy. It is **not maskable through IER** and is cleared only by
  reading USR (or IIR).
- So with `IER == 0x00` the line can still be asserted.
- With the (correct) level trigger from (a), the GIC re-asserts for as long as
  the condition stands, which is forever if the ISR never clears it.

### Measured on hardware (Rockchip RK3566, Cortex-A55)

A shell command reading uart2's 16550 registers, with the UART used **only**
through `uart_poll_out` (console/shell live on USB CDC-ACM, so uart2 has no
callback):

```
read 1:  IER 0x00  (no source enabled)   IIR 0xc7  -> IID 0x7 = DW busy detect
read 2:  IER 0x00                        IIR 0xc1  -> no interrupt pending
```

`IER == 0` while an interrupt is pending is the whole point: it cannot be
masked. The second read shows it was latched and that one read clears it.

Logging the GIC IAR for the first 24 interrupts gave `0x0096` = 150 every time
(= `GIC_SPI 118` + 32 = uart2).

### How it turns into a boot failure

With the defaults this is survivable — the board boots and runs. Turn on
`CONFIG_THREAD_RUNTIME_STATS=y` and it is not: `arch/arm64/core/isr_wrapper.S:53`
calls `z_sched_usage_stop` at interrupt entry under
`CONFIG_SCHED_THREAD_USAGE`, and the result is an endless
switch-then-interrupt loop that never reaches the first `POST_KERNEL` init.

Measured with temporary markers in `kernel/usage.c` / `kernel/include/usage.h`
(887 KB of UART output):

```
context switches (z_sched_usage_switch) : 48,936 enter / 48,936 exit
z_sched_usage_stop                      : 97,871 enter / 97,871 exit
POST_KERNEL priority-0 marker           : 0
```

`stop` is entered exactly twice per switch — once from the switch path, once
from the ISR entry. Entries and exits are balanced, so this is neither
recursion nor a deadlock: it returns every time and never makes progress.
Without the markers the loop runs at full speed and the board is simply
silent, which is why it first looked like a dead hang.

Setting `CONFIG_UART_NS16550_DW8250_DW_APB=y` fixes it completely: the board
boots, and `IIR` reads `0xc1` on the first read.

### Suggestions

1. **(a)** Make the Rockchip UART trigger types consistent and level, matching
   Linux. The rk3588 / rk3588s disagreement is a concrete, checkable bug.
2. **(b1)** Have `rockchip,rk3568-uart` (and the rk3588 equivalents) `select`
   `UART_NS16550_DW8250_DW_APB`, since those parts *are* DW_apb_uart. A user
   should not have to know the IP vendor to get a working ISR.
3. **(b2)** Either do not `irq_enable()` an instance that has no interrupt
   user, or make `uart_ns16550_isr()` always read IIR so an unexpected,
   un-maskable source cannot latch forever. Today a correct devicetree
   (level-triggered) is what makes the latch fatal, which is an unpleasant
   trap for anyone fixing their DT.

### Environment

Zephyr `v4.4.0-3902-gfb0fa3dd488d`, out-of-tree `radxa_zero_3e/rk3568` board
(Rockchip RK3566, Cortex-A55), `CONFIG_GIC_V3=y`, `CONFIG_SMP=n`,
`CONFIG_USE_SWITCH=y`, shell and console on USB CDC-ACM, uart2 used only for
`uart_poll_out` (markers plus `chosen zephyr,log-uart`).

---

## 올리기 전 확인할 것 (보고 2·3)

- 보고 2: 지금 `dev` 브랜치에서 `CONFIG_SCHED_THREAD_USAGE_AUTO_ENABLE=n` 로
  빌드해 같은 오류가 나는지 한 번 더 확인한다(우리는 확인했다).
- 보고 3(a): rk3588 / rk3588s 의 해당 줄이 지금도 그대로인지 확인한다 —
  고쳐졌다면 rk3568 만 남은 것이고 보고가 더 짧아진다.
- 보고 3(b): 비슷한 이슈가 이미 있는지 찾아본다. 검색에서 #117634
  (`TRACING_ASYNC` 가 `USE_SWITCH` 에서 전환 중 커널 타이머를 건다)를 찾았는데
  **다른 버그**다(우리 구성에는 `CONFIG_TRACING` 이 없다). 다만 같은 영역에
  아키텍처별 함정이 있다는 맥락으로 링크할 만하다.

---

## (4) `ggml_backend_sched_new` 가 `malloc` 결과를 확인하지 않는다

**파일:** `ggml/src/ggml-backend.cpp:2251-2252`

```c
sched->context_buffer_size = ggml_sched_max_splits*GGML_SCHED_MAX_SPLIT_INPUTS*2*sizeof(struct ggml_tensor)
                           + ggml_graph_overhead_custom(graph_size, false);
sched->context_buffer = (char *) malloc(sched->context_buffer_size);   // ← 결과 미확인
```

실패하면 `context_buffer` 가 `NULL` 이 된 채로 진행하고, 한참 뒤
`ggml_backend_sched_split_graph`(`:1480`)가 그것을 `mem_buffer` 로 넘긴다.
`ggml_init` 은 `mem_buffer == NULL` 이면 **스스로 할당**하므로 거기서
`ggml_aligned_malloc` 이 실패하고, 증상은 이렇게 나타난다:

```
ggml_aligned_malloc: insufficient memory (attempted to allocate 197.41 MB)
ggml/src/ggml.c:1662: GGML_ASSERT(ctx->mem_buffer != NULL) failed
```

**왜 문제인가:** 197 MB 를 **누가** 요구했는지 알 수 없다. 메시지는
`ggml_init` 을 가리키지만 실제 요구자는 스케줄러 생성이다. 우리는 이 때문에
"포팅 결함" 으로 오진하고 호스트와 버퍼 크기를 하나하나 대조하는 데 시간을
썼다 (호스트의 `compute buffer size = 2.22 MiB` 는 **다른 버퍼**라 더
헷갈렸다).

**제안:** `malloc` 뒤에 확인을 넣는다.

```c
sched->context_buffer = (char *) malloc(sched->context_buffer_size);
if (sched->context_buffer == NULL) {
    GGML_LOG_ERROR("%s: failed to allocate context buffer of %zu bytes "
                   "(graph_size=%zu, max_split_inputs=%d)\n",
                   __func__, sched->context_buffer_size,
                   graph_size, GGML_SCHED_MAX_SPLIT_INPUTS);
    ggml_backend_sched_free(sched);
    return NULL;
}
```

**곁들여:** 그 크기 추정이 단일 백엔드에서 매우 보수적이다.
`ggml_sched_max_splits = graph_size` 는 "노드마다 split 하나" 가정인데
(`:2241` 주석), 백엔드가 하나면 split 은 하나다. qwen35 + 단일 CPU 백엔드에서
`graph_size = 10240` · `GGML_SCHED_MAX_SPLIT_INPUTS = 30` 이면 197 MB 이고,
실제로 쓰이는 양은 그 수백 분의 일이다. `sched->n_backends == 1` 일 때
`ggml_sched_max_splits` 를 1 로 두면 임베디드에서 쓸 수 있게 된다.

(우리는 `GGML_SCHED_MAX_SPLIT_INPUTS` 를 강제 포함으로 낮춰 우회했다 —
`#ifndef` 가드가 있어 상류 수정 없이 됐다. 그 가드는 고맙다.)

---

## (5) ns16550 이 DesignWare 의 busy-detect 인터럽트를 처리하지 않아, Rockchip UART 를 인터럽트 구동으로 쓰면 코어가 **사이클의 77% 를 잃는다**

**상태: 원고 완성, 올리지 않았다 (2026-11-07). 실기 실측 완료.**

보고 3 과 같은 영역이지만 **다른 결함**이다. 보고 3 은 DT 의 트리거 선언
모순과 쓰지 않는 포트의 IRQ 를 켜는 문제이고, 이것은 **드라이버가 이 하드웨어의
인터럽트 하나를 지울 줄 모른다**는 문제다.

### 제목 (영문)

> `drivers/serial/uart_ns16550`: DesignWare busy-detect interrupt is never
> cleared on Rockchip RK356x — interrupt storm costs 77% of CPU cycles

### 본문 (영문)

**Summary.** On Rockchip RK3566 (Radxa ZERO 3E, `rockchip,rk3568-uart`
which binds `uart_ns16550`), enabling `CONFIG_UART_INTERRUPT_DRIVEN=y`
produces a permanent interrupt storm on the console UART. Measured with the
PMU: the CPU loses **77% of its cycles** to the ISR. An ALU-only loop that
touches no memory runs at IPC 0.45; masking interrupts with
`msr daifset, #0xf` raises it to IPC 1.99.

**Root cause.** The RK356x UART is a Synopsys DesignWare DW_APB_UART, not a
plain 16550. It has an extra interrupt that standard 16550 does not:
`IIR[3:0] == 0x7`, "busy detect". It is raised when `LCR` is written while
the transmitter is busy, and it is **cleared only by reading the UART Status
Register (USR) at offset 0x7C**. Reading `LSR` or `IIR` does not clear it,
and — crucially — **it cannot be masked via `IER`**.

Zephyr's generic `uart_ns16550.c` has no knowledge of `USR`. Linux handles
this in a dedicated driver: `drivers/tty/serial/8250/8250_dw.c`,
`dw8250_handle_irq()` reads `UART_USR` when `(iir & 0x3f) == UART_IIR_BUSY`.

The trigger in practice: the bootloader (U-Boot) leaves the UART running at
1.5 Mbps; Zephyr's init writes `LCR` to reconfigure to 115200 8N1 while the
transmitter is still busy.

**Measurements** (RK3566 Cortex-A55 r2p0, MIDR `0x412fd050`, 815 MHz;
Zephyr `v4.4.0-3902-gfb0fa3dd488d`; UART at `0xfe660000`, Zephyr IRQ 150):

```
                        cycles/iter   IPC    EXC_TAKEN   IIR
baseline (untouched)       21.96      0.45     21,971    0xc7  (busy detect)
after reading IIR          21.96      0.45     21,970    —     (not cleared)
after reading USR (0x7C)    5.00      1.99          0    0xc1  (none pending)
after irq_disable()         5.00      1.99          0          (positive control)
after irq_enable()          5.00      1.99          0          (stays fixed)
```

`IER` was `0x00` throughout — **every maskable UART interrupt was already
disabled** and the IRQ still fired. That is what identifies busy-detect
specifically; nothing maskable can explain it. `USR` read `0x03` with
`BUSY=1`.

Cost per exception: `(10,981,957 − 2,500,719) / 21,972 ≈ 386 cycles`.

**End-to-end impact.** A llama.cpp inference app on this board went from
**23.6 s/token to 1.423 s/token (16.6×)** purely by setting
`CONFIG_UART_INTERRUPT_DRIVEN=n`, with bit-identical output (12/12 token ids
and logits unchanged).

**Suggested fix.** Handle the DesignWare case in the ISR, gated on a new
Kconfig or on a `snps,dw-apb-uart`-style compatible:

```c
/* DW_APB_UART: busy detect (IIR[3:0] == 0x7) is cleared only by reading
 * USR at 0x7C. It is not maskable via IER. */
if ((iir & 0xf) == 0x7) {
        (void)ns16550_inbyte(dev, 0x7C / sizeof(...));
}
```

A second, complementary fix is to avoid writing `LCR` while `USR.BUSY` is
set (Linux also does this, in `dw8250_set_termios`).

**Why this matters beyond one board.** `dts/arm64/rockchip/rk3568.dtsi` is
in-tree and declares `compatible = "rockchip,rk3568-uart", "ns16550"`, so
any in-tree RK356x board (`roc_rk3568_pc`, `rock_3b`, …) that turns on
interrupt-driven UART hits this. The symptom is not a failure — output is
correct — it is a silent 4–17× slowdown, which is the hardest kind to find.

### 올리기 전 확인할 것

- [ ] `uart_ns16550.c` 의 현재 `dev`(메인라인) 상태를 다시 읽어 그 사이에
      DW 처리가 들어왔는지 확인한다
- [ ] 다른 DW_APB_UART SoC(예: Intel, Altera, 일부 Allwinner)에서 같은
      드라이버를 쓰는 보드가 영향을 받는지 한 줄 조사한다
- [ ] 우리 쪽 고침(`soc_init/`)이 돌아간 뒤의 moru 측정값을 본문에 넣는다

### 우리 쪽 근거 문서

- `docs/l8-why-slow.md` — L8i(지목)·L8j(확정) 전체 출력
- `apps/llm_l8i/src/main.c` — PMU 이벤트 계수판
- `apps/llm_l8j/src/main.c` — 양성·음성 대조를 포함한 확정판
- `apps/llm_common/ladder.conf` — 사다리 쪽 우회(=n)와 그 근거
