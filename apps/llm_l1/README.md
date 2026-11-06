# L1 — 대용량 메모리 관문

[docs/llm-hub-test-plan.md](../../docs/llm-hub-test-plan.md) 의 사다리 둘째 칸.
**"Zephyr 창(128 MB) 밖의 DRAM 을 읽고 쓸 수 있는가"** 를 가린다.
Qwen3.5-0.8B Q4 가 약 0.55 GB 라 창에 안 들어가므로, 이게 안 되면 L2 이후가
전부 성립하지 않는다.

## 결과 (2026-11-07, QEMU `-cpu cortex-a55`)

```
=== L1: 대용량 메모리 관문 ===
  창: 0x60000000 + 640 MiB  (DT model_blob, zephyr,memory-region)
  [1] 별칭 점검 (2 MiB 보폭 320 지점) ... OK
  [2] 전 영역 쓰기 640 MiB ... 360 ms  (1777 MiB/s)
  [3] 되읽기 값 비교 ... OK
  [4] CRC32 (64 KiB 청크) ... 0xa8b4f938  (3800 ms)
      OK — 호스트 골든값 0xa8b4f938 와 일치
=== L1 PASS (실패 0) ===
```

## 이 한 번이 증명한 것 넷

**① 창 밖 DRAM 을 매핑하는 데 C 코드가 0줄이다.**
arm64 가 DT 의 `zephyr,memory-region` 노드를 부팅 때 평면 매핑한다
(`arch/arm64/core/mmu.c:907` 의 `mmu_dt_regions`). `device_map()` 호출도,
SoC 의 `mmu_regions[]` 배열 수정도 필요 없었다 — **`app.overlay` 한 장**이다.

⚠ 단 **`zephyr,memory-attr` 속성이 있어야 항목이 생긴다.**
`MMU_REGION_DT_FLAT_ENTRY_FROM_DT` 가 `IF_ENABLED(DT_NODE_HAS_PROP(...))` 로
감싸고 있다(`include/zephyr/arch/arm64/arm_mmu.h:236-242`). 빼면 **빌드는 통과하고
조용히 안 매핑돼** 런타임에 폴트가 난다. `DT_MEM_ARM64_MMU_NORMAL` 을 쓴다.

**② 2 MB 정렬이 필수다.**
`set_mapping`(`mmu.c:372`)은 `size < level_size || virt & (level_size-1)` 일 때만
서브테이블로 내려간다. 즉 **정렬이 맞으면 가장 큰 블록을 쓴다.**
`0x60000000` + 640 MiB 는 둘 다 2 MB 정렬이라 L2 블록 320개 = 추가 테이블 2개다.
정렬을 깨면 4 KB 페이지로 깔려 `MAX_XLAT_TABLES` 를 넘기고 **콘솔이 올라오기
전에 죽는다** — 과거의 "조용한 죽음" 과 증상이 똑같아 원인을 가릴 수 없다.

**③ 640 MiB 가 진짜 RAM 이다.** 2 MiB 보폭 320 지점에 그 주소에서만 나오는
값을 넣고 전부 되읽었다. 창이 실제 RAM 보다 크면 뒤쪽 쓰기가 앞쪽을 덮어
여기서 걸린다 — 640 MB 를 다 쓰기 전에, 싸게.

**④ 골든값 장치가 성립한다 — L2 이후가 이것에 얹힌다.**
Zephyr `crc32_ieee` 가 다항식 `0xedb88320` · 초기 `~0` · 최종 반전이라
`zlib.crc32` 와 같고(`subsys/crc/crc32_sw.c`), 청크로 이어 계산하는 것도
의미가 같다. 640 MiB 를 64 KiB 씩 1만 240번 이어 계산한 값이 호스트와
**비트 단위로 같았다.**

## 패턴을 주소의 함수로 잡은 이유

```c
word[i] = (uint32)(i * 2654435761) ^ 0x6D6F7275   /* 'moru' */
```

- 값이 **첨자의 함수**이므로 호스트에서 numpy 로 벡터화된다(640 MiB 가 0.17 초).
  xorshift 같은 순차 PRNG 는 numpy 로 못 펼쳐 호스트 생성이 느리다.
- 주소가 섞이는 오류(별칭·바이트순서·워드 교환)를 **값으로** 잡는다.
  0 으로 채우면 창이 실제 RAM 보다 커도 통과해 버린다.

## 숫자를 어떻게 읽나

쓰기 **1777 MiB/s** 는 **QEMU 수치다 — 하드웨어 대역폭이 아니다.**
L8 에서 실기로 다시 재야 한다. CRC32 3800 ms(≈ 168 MiB/s)는 소프트웨어
CRC 가 바이트당 테이블 조회 2회를 하기 때문이고, TCG 라 더 느리다.

## 남은 위험 — L1 이 가릴 수 없는 것

모델 블롭은 **Zephyr 의 MMU·캐시가 켜지기 전에** U-Boot(`fatload`) 또는
QEMU(`-device loader`)가 써 넣는다. 그 뒤 캐시 가능 매핑으로 읽으면
**낡은 캐시 줄을 볼 수 있다.** L1 은 CPU 가 직접 쓴 것을 CPU 가 읽으므로
이 경로를 건드리지 못한다. **L2 의 첫 항목이 이것이다** — 실제 블롭을
`-device loader` 로 넣고 헤더 매직(`GGUF`)이 보이는지부터 확인한다.

## 돌리는 법

```bash
./tools/qemu_run.sh apps/llm_l1 600
```

골든값을 다시 만들 때 (영역 크기나 패턴을 바꿨다면):

```bash
python3 tools/l1_golden.py --size 640M
```

`main.c` 에 `BUILD_ASSERT` 가 있어 크기를 바꾸면 **빌드가 먼저 막는다.**
