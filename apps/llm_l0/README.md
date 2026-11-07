# L0 — C++17 런타임 관문

[docs/llm-hub-test-plan.md](../../docs/llm-hub-test-plan.md) 의 사다리 첫 칸.
**"ggml + llama.cpp 그래프" 안(B안)이 성립하는가** 를 가린다. 한 파일에 세
기능을 넣어 한 번에 본다 — 하나라도 막히면 포팅 비용이 두 배가 된다.

## 결과 (2026-11-07, QEMU `-cpu cortex-a55`)

```
=== L0: C++17 런타임 관문 ===
  [1] std::vector<float>(1024)  size=1024  ... OK
  [2] throw/catch std::runtime_error ... OK
  [3] pthread_create + join (ggml 이 쓰는 것) ... OK
  [4] std::thread (참고) ... 쓸 수 없다 — libstdc++ 가 gthreads 없이 빌드됐다
=== L0 PASS (실패 0) ===
```

## 배운 것 셋 — 두 개는 내 가정이 틀렸다

**① picolibc 로 된다. newlib 로 바꿀 필요가 없다.**
계획에 "libc 를 newlib 로 바꿔야 할 가능성이 높다" 고 적었는데 **틀렸다.**
`GLIBCXX_LIBCPP` 의 의존성이 `NEWLIB_LIBC || PICOLIBC`(`lib/cpp/Kconfig:119-125`)
이고, picolibc 로 STL·예외·RTTI 가 모두 켜졌다.

**② `std::thread` 는 쓸 수 없다. 그러나 ggml 에는 필요 없다.**
툴체인의 libstdc++ 가 gthreads 없이 빌드돼 `std::thread` 의 생성자 템플릿이
막혀 있다(`no matching function for call to std::thread::thread(...)`).
**ggml 은 pthread 를 쓰므로 영향이 없다.**
⚠ 다만 **llama.cpp 의 C++ 부분이 `std::thread` 를 쓰는 곳이 있으면 걸린다** —
3단계에서 확인할 잔여 위험으로 남긴다.

**③ `CPP_EXCEPTIONS` 는 `MINIMAL_LIBCPP` 에서 열리지 않는다.**
`lib/cpp/Kconfig:159-171` 이 `if !MINIMAL_LIBCPP` 로 감싸고 있다. 기본값이
`MINIMAL_LIBCPP` 이므로 **`CONFIG_GLIBCXX_LIBCPP=y` 를 명시**해야 예외가 켜진다.
그러지 않으면 "y 를 줬는데 n 이 됐다" 는 경고만 나오고 조용히 꺼진다.

## 그리고 함정 하나 (실제로 걸렸다)

`pthread_create(NULL attr)` 가 **EINVAL(22)** 로 실패했다. 스택을 받을 데가
없어서다 — `CONFIG_DYNAMIC_THREAD` 이 필요하고, 그것은
**`CONFIG_THREAD_STACK_INFO` 에 의존**한다(`kernel/Kconfig:192-195`).
그 한 줄을 빼면 빌드는 통과하고 **런타임에만 실패한다.**

## 돌리는 법

QEMU — **`-cpu cortex-a55` 를 쓴다.** 우리 보드와 MIDR 이 같고(`0x412fd050`)
dotprod 가 있으며 i8mm 이 없다. `-cpu max` 는 i8mm 이 있어 **실기에 없는 커널
경로**를 고르게 하므로 쓰지 않는다.

```bash
west build -p always -b qemu_cortex_a53 -d /tmp/q-l0 apps/llm_l0
qemu-system-aarch64 -cpu cortex-a55 -machine virt,secure=on,gic-version=3 \
  -m 2G -nographic -no-reboot -kernel /tmp/q-l0/zephyr/zephyr.elf
```

실기 — `moru ota apply try` 슬롯으로 올린다(깨져도 전원 재투입으로 복구).

```bash
./build.sh "$PWD/apps/llm_l0"
```

## build.sh 를 바꿨는데 기존 빌드가 안 바뀌었다는 근거

`build.sh` 가 호출자의 `ZEPHYR_TOOLCHAIN_VARIANT` 를 존중하게 고쳤다. 기본값은
`cross-compile` 그대로다. **기존 moru 빌드가 영향받지 않았음을 이렇게 확인했다:**

```
text 섹션  143,360 B  —  바이트 단위로 같다 ✅
전체 바이너리  27 바이트 다름  (157135~157146 · 157218~157224 · 170815~170826)
```

세 군데 모두 **문자열 영역**이고, 눈으로 확인한 내용은 박혀 있는 버전
해시다(`4e39f765…` → `f37b3ff5…`). 나머지는 `MORU_BUILD_TIME` 이다.
**즉 코드는 그대로이고 차이는 매 빌드마다 바뀌는 문자열뿐이다.**

⚠ 처음 커밋 메시지(`f37b3ff`)에 "moru 바이너리가 바이트 단위로 같은 것을
cmp 로 확인했다" 고 적었는데 **부정확했다** — 전체 바이너리는 27바이트
다르다. 같은 것은 **`.text`** 다. 비교 대상(`sdcard/zephyr.bin`)이 이전
커밋에서 만들어진 것이라 버전 문자열이 다를 수밖에 없었는데 그것을 빼고
비교하지 않았다.

## ✅ 실기 통과 — 2026-11-07, Radxa ZERO 3E (RK3566)

```
=== L0: C++17 런타임 관문 ===
  [1] std::vector<float>(1024)  size=1024  ... OK
  [2] throw/catch std::runtime_error ... OK
  [3] pthread_create + join (ggml 이 쓰는 것) ... OK
  [4] std::thread (참고) ... 쓸 수 없다 — libstdc++ 가 gthreads 없이 빌드됐다
=== L0 PASS (실패 0) ===
```

RAM 812 KB / 128 MB (0.62%) · 이미지 136,208 B.

**QEMU 와 네 항목이 전부 같다.** 특히 `[2]` 가 값지다 — arm64 의 테이블 기반
예외 전개가 `.eh_frame` 을 읽고 스택을 거슬러 올라가는데, 메모리 배치가
1 MiB → 128 MiB 로 바뀐 실기 EL1 에서도 동작한다.

### ⚠ 세 번 만에 받았다 — 앞선 두 번은 **채널이 죽어 있었다**

1차·2차 투입에서 아무 출력이 없었고 나는 "L0 이 멈췄다" 로 읽었다. **틀렸다.**
CP2102 어댑터가 USB 에서 빠져 `tio` 가 **죽은 핸들**을 쥐고 있었다.
`ps` 로 보면 `tio` 는 살아 있고 `/dev/cu.usbserial-0001` 도 (다시 꽂힌 뒤엔)
존재해서 정적 확인으로는 정상처럼 보였다.

**L0 은 한 번도 멈춘 적이 없다.** 세 번 모두 통과했을 가능성이 높고, 내가
결과를 받지 못했을 뿐이다.

그래서 `tools/hw_channel_check.sh` 를 만들고 두 번 고쳤다 —
자세한 것은 [docs/bringup-rules.md](../../docs/bringup-rules.md) 의
"규칙 6 을 하루에 세 번 밟았다".
