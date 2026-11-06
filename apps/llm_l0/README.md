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
