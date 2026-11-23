# Zephyr 4.4(개발판) → 4.5.0 전환 (2026-11-23)

기본 작업 공간을 `~/zephyrproject`(v4.4.0-3902, 4.4.99) 에서 `~/zephyrproject-4.5`(v4.5.0) 로 옮겼다.
`build.sh` · `tools/qemu_run.sh` · `tools/qemu_edge_run.sh` 의 기본값이 바뀌었다(옛 판은 `PIZZA_ZEPHYR_WS=~/zephyrproject PIZZA_ZEPHYR_VENV=~/.zephyr-venv`).

## 보드 포팅 — 고칠 것 없음
보드(`boards/radxa/radxa_zero_3e`) · DT(`dts/arm64/rockchip/rk3566-usb.dtsi`) · 드라이버(dwc3 UDC · DW-MSHC · hwinfo OTP)는
소스 변경 없이 4.5 로 빌드된다: ledblink · moru · hubllm (hubllm RAM 98.92 %, 256 MB 창).
업스트림 4.5 에는 `radxa_zero_3e` 가 없다(Radxa 는 rock_3b · rock_5b_plus 뿐, SoC 는 rk3568.dtsi 재사용).

## QEMU 에서 바뀐 것 — RAM 디스크 매핑
4.5 의 `drivers/disk/ramdisk.c` 는 MMU 가 있으면 버퍼를 `k_mem_map_phys_bare` 로 **커널 가상 공간에 다시 매핑**한다.
qemu_cortex_a53 기본 `KERNEL_VM_SIZE` 8 MiB 로는 32 MiB RAM 디스크가 들어가지 않아 부팅 직후
`insufficient virtual address space (requested 33558528)` → 커널 패닉. RAM 디스크를 쓰는 시험 앱 9개
(hubml_q · q2 · q4 · q5 · q6 · q7 · q9 · q10 · q11)에 `CONFIG_KERNEL_VM_SIZE=0x8000000` · `CONFIG_MAX_XLAT_TABLES=32`.
실기 앱은 RAM 디스크를 쓰지 않는다(SD).

## QEMU 사다리 결과 (4.5, 40개)
| 판정 | 앱 |
|---|---|
| PASS 20 | Lm1·Lm2·Lm4~Lm11 (hubml_q*) · Lm3 (hubml_edge_q, M33) · L0 · L2 · L3 · L4 · L5a · L5b · L6a · L6b · L7a |
| 측정 끝까지 7 | L8a · L8b · L8c · L8g · L8h · L8i · L8n (판정 줄 없는 측정 앱 — 죽지 않고 "측정 끝") |
| QEMU 빌드 불가(원래) 9 | L6d · L8j · L9b (`#error` 실기 전용) · L8d · L8p · L8q (보드 전용 심볼) · L9c · L9d (`moru_led.h`) · **L1 (결함: 모델 창 576 MiB 인데 BUILD_ASSERT 가 512 MiB — 2026-11-08 창 확대 때 안 고침)** |
| 4.4 에서도 같음 4 | L6c · L8e (`-smp 4` QEMU 에서 출력 0 B) · L8f (출력 0 B) · L8k (`ESR 0x02000000` CPU 예외, 실기 PMU 측정) |
| 시간 초과 1 | L7b — QEMU 가 느려 600 초 안에 못 끝냄(prefill 18 토큰 68 초). 그때까지 토큰·1등 호스트와 같음 |
| 실기 전용 1 | L9a — 실 SoC 의 CRU·I2C 를 읽어 QEMU 에서 예외 |

**4.5 때문에 새로 생긴 실패는 RAM 디스크 매핑 하나뿐**이고 위 설정으로 고쳤다.
4코어(SMP) 동작은 QEMU 가 4.4 에서도 못 보여 주므로 **실기(hubllm, 4코어)로 확인한다.**

## 실기 (2026-11-23, apply try)
- **1차 fd55929: USB 전에 무음 멈춤.** 4.5 에서 `CONFIG_UART_NS16550_DW8250_DW_APB` 가 DT 호환 `snps,dw-apb-uart` 에
  걸리게 바뀌어 꺼졌다(빌드 경고 한 줄). 보드 DTS uart2 에 호환을 더함(7437239) → 경고 0 · 설정 y.
- **2차 7437239 (build 74372394feb0): 통과.** 부팅 슬롯 try · Zephyr 4.5.0 · 4코어 기동 · OTP 칩 ID · SD(FatFs, ml 상태 e1~e4 복원,
  e4 schema 4d463334) · LLM 적재 8809 ms(4.4 8791 ms) · `hub_chat` "대한민국의 수도는?" → 서울, ttft 1400 ms · tg 4.27 tok/s(1104 MHz)
  · `ml status` · `kernel heap` 정상.
- **영구 적용 (19:31, 사용자 지시):** 같은 판 CRC 0x319d7cfb · `moru ota apply` → 부팅 슬롯 main · build 74372394feb0 · LLM 8817 ms.
  이전 영구판 6dccf09(4.4)는 카드의 `zephyr.bin.old`.
- 같은 날 4.4 영구판에서 재부팅·USB 끊김이 여러 번 있었다(18:08~18:23). 보드가 USB-C 7-in-1 어댑터 아래였고, 맥 본체에 직접
  꽂은 뒤로는 재현되지 않았다 — 전원 공급으로 본다(확정 아님).

## 남은 일
- L1 BUILD_ASSERT 를 576 MiB 로 고치고 `tools/l1_golden.py` 로 골든 CRC 재생성(별도).
- `tools/qemu_run.sh` 의 제한시간이 걸리지 않는 경우가 있었다(L8k · L9a 가 600 초 넘게 남음) — 별도 확인.
