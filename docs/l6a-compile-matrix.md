# L6a — llama.cpp `src/` 가 Zephyr 툴체인에서 컴파일되나

2026-11-07. `aarch64-zephyr-elf-g++ 14.3.0`, `-mcpu=cortex-a55 -std=c++17`,
강제 포함 둘(`ggml_zephyr_compat.h` · `zephyr_future.h`), **상류 수정 0건**.

## 결과 — 전부 통과

```
llama-model-loader  ✓     unicode             ✓     llama-kv-cache          ✓
llama-arch          ✓     unicode-data        ✓     llama-memory-recurrent  ✓
llama-hparams       ✓     llama-mmap          ✓(주) llama-adapter           ✓
llama-impl          ✓     llama-model         ✓     llama-chat              ✓
llama-vocab         ✓     llama-graph         ✓     llama-sampler           ✓
                          llama-context       ✓     llama-grammar           ✓
                          llama-batch         ✓     llama               ✗ llama-version.h
```

`llama.cpp` 하나만 실패하고, 그것도 **cmake 가 만드는 `llama-version.h` 가
없어서**다 — 코드 문제가 아니다.

## 그걸 가능하게 한 것 — 포팅 항목 6번 `zephyr_future.h`

`llama-model-loader.cpp` 는 혼자 컴파일되지 않았다. 이 툴체인의 libstdc++ 는
`_GLIBCXX_HAS_GTHREADS` 없이 빌드돼 `<future>` 의 **정의가 전부 가드 안**이다
(`future:216`). 밖에 남는 것은 `launch`·`future_status`·`future_error`,
전방 선언들, `__async_result_of`, 그리고 **`async()` 의 선언**(`:208,:212`)이다.

로더는 그것을 쓴다:

```
:1579  std::vector<std::future<std::pair<ggml_tensor *, bool>>> validation_result;
:1730  validation_result.emplace_back(std::async(std::launch::async, …));
:1858  for (auto & future : validation_result) future.get();
```

`:1730` 은 `if (check_tensors)` 안이지만 **런타임 분기**라 `check_tensors=false`
로 둬도 늘 컴파일된다. 그래서
`error: invalid use of incomplete type 'class std::future<int>'` 가 난다.

`zephyr_future.h` 가 libstdc++ 가 비워 둔 자리를 채운다. `async` 는 **그 자리에서
실행**하고 결과(또는 예외)를 담아 돌려주며, `get()` 이 옮겨 준다. 코어 하나에
스레드가 없으므로 `launch::async` 도 `deferred` 도 즉시 실행이 맞다.

⚠ **`async` 를 새 과형으로 더하면 안 된다.** 선언이 이미 있어
`call of overloaded 'async(…)' is ambiguous` 가 난다 — 겪었다. 선언된 서명
(`future<__async_result_of<_Fn, _Args...>>`)을 **그대로 정의**해야 한다.

⚠ 의미는 아직 **컴파일만** 확인했다. 값·예외 재던지기·`void`·인자 전달의
실제 동작은 **L6a 앱의 점검 [0]** 에서 QEMU 로 가린다. 호스트 근사 시험은
하지 않는다 — 호스트 libstdc++ 에는 진짜 `future` 가 있어 같은 코드를 돌릴 수
없다(이 헤더는 그 경우 `#error` 로 막는다).

## (주) `llama-mmap.cpp` — 컴파일되지만 **쓸 수 없다**

이 툴체인은 `_POSIX_MAPPED_FILES` 를 정의하지 않는다(`-E -dM` 으로 확인).
그래서 fallback 가지로 간다:

- `llama_mmap::SUPPORTED = false` (`llama-mmap.cpp:691`)
- `llama_mmap::impl` 생성자가 `throw std::runtime_error("mmap not supported")` (`:664`)

`SUPPORTED=false` 면 로더가 `use_mmap` 을 끄고(`llama-model-loader.cpp:835-837`)
텐서마다 새 버퍼에 `fread` 한다 — **508 MiB 를 또** 쓴다. 여유 4.15 MiB 에
불가능하다.

→ **결정 3(제자리 적재)의 대체판이 그대로 필요하다.** 상류를 고치는 것이 아니라
`llama-mmap.cpp` 를 소스 목록에서 빼고 `llama_mmap_zephyr.cpp` 를 대신 링크한다.
헤더가 완전한 pimpl 이라 호출부는 하나도 안 바뀐다.

## 다음에 재야 할 것 — 이미지 크기

전부 컴파일된다는 것이 **전부 들어간다**는 뜻은 아니다. OTA 상한이 8 MiB 다.

| 항목 | 측정 |
|---|---|
| `std::regex` 한 함수의 `.o` | **259,504 bytes** (`-Os`). 쓰는 곳은 `llama-model-loader.cpp:1251`(`tensor_buft_overrides` — 우리는 null 이지만 인스턴스화는 피할 수 없다)와 `unicode.cpp`(토크나이저, L6a 범위 밖) |
| `unicode-data.cpp` | 11,019 줄, 거의 전부 표 |
| `src/models/*.cpp` | 175개 46,741 줄. `llama-model.cpp` 의 switch 가 참조한다 |

`build.sh` 가 이미 OTA 상한 대비 비율을 찍는다. **L6a 에서 재고 L6c 에서
놀라지 않는다.**
