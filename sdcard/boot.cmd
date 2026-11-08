# PiZZa — Radxa Zero 3E 에서 Zephyr 를 올린다
#
# U-Boot 가 부팅 대상 파티션마다 boot.scr 를 extlinux.conf 보다 먼저 찾으므로,
# 이 스크립트가 있으면 콘솔 입력 없이 Zephyr 로 간다.
#
# ⚠ 적재에 실패하면 go 를 부르지 않고 그냥 끝낸다. 그러면 U-Boot 가 다음
#   부팅 항목(파티션 3 의 extlinux.conf)으로 넘어가 Linux 가 뜬다.
#   즉 이 파일이 있어도 Zephyr 를 못 읽으면 보드는 평소대로 부팅한다.
#
# 되돌리려면 이 파일(boot.scr)만 지우면 된다.

# ⚠ 'usb stop' 을 넣지 않는다.
#
#   U-Boot 의 preboot 가 'usb start' 로 USB 를 올려 두는데, 'usb stop' 은
#   dwc3 장치를 제거하면서 클럭 묶음을 해제한다. Zephyr 에는 CRU 드라이버가
#   없어 그 클럭을 되살릴 수 없고, 그러면 컨트롤러 레지스터가 전부 0 으로
#   읽혀 GSNPSID 검사에서 init 이 중단된다 (실기에서 확인).
#
#   즉 U-Boot 가 켜 둔 클럭을 그대로 물려받는 것이 지금은 필수다.

setenv zaddr 0x40000000

# ── 진입 주소 = **적재 주소** (2026-11-07 정정) ────────────────────────────
#
# 전에는 `setenv zentry 0x40001084` 로 **고정**돼 있었다. 그 값은 moru 의
# ELF 진입점(`__start`)이고 **앱마다 다르다**:
#     moru     __start = 0x40001084
#     llm_l0   __start = 0x4001040c
#     llm_l5b  __start = 0x40070f5c
# 그대로 두면 사다리 앱을 올릴 때 **이미지 중간으로 뛰어** 멈춘다.
#
# 고칠 필요가 없었다 — Zephyr arm64 는 **Linux arm64 이미지 헤더**를 붙인다.
# 오프셋 0 이 `__start` 로 가는 분기다(objdump 로 확인):
#     moru     0: 14000421  b 0x1084
#     llm_l5b  0: 1401c3d7  b 0x70f5c
# 오프셋 0x10 의 image_size 도 각 이미지의 RAM 사용량과 맞는다
# (moru 0xa3000 · l5b 0x32c6000 ≈ 50.8 MiB = 링커 보고 51,992 KB).
#
# 그래서 **적재 주소로 뛰면 어떤 이미지든 동작한다.** 앱마다 값을 바꿀
# 필요가 없어졌다.
setenv zentry ${zaddr}

# ── OTA (2026-11-06) ──────────────────────────────────────────────────
#
# Zephyr 가 USB 로 받은 새 이미지를 0x50000000 에 쌓고 매직 "MORU" 를 쓴 뒤
# 재부팅한다 (moru ota begin -> verify -> apply). 여기서 그것을 SD 에 쓴다.
#
#   헤더  [0] 매직 0x4d4f5255 "MORU" · [4] 크기 · [8] CRC32 · [12] 0
#   본체  [16] 부터 — Zephyr 가 256 KiB 까지 0 으로 채워 둔다
#
# ⚠ 고정 256 KiB 를 쓴다. U-Boot 에 setexpr 가 없어 헤더의 크기를 변수로
#   읽을 수 없기 때문이다(실측 확인). 그래서 Zephyr 쪽 apply 가 이미지
#   뒤를 0 으로 채운다 — 쓰레기가 아니라 0 이 들어가고, 그 영역은 Zephyr
#   의 .bss 라 arch_bss_zero() 가 어차피 지운다.
#
# ⚠ CRC 검증은 Zephyr 의 moru ota verify 가 한다. U-Boot 의 crc32 는 결과를
#   변수로 받을 수 없어(setexpr 없음) 여기서 비교할 수 없다. apply 는
#   verify 를 통과한 뒤에만 매직을 쓰므로 그것이 보호다.
#
# ⚠ 폴백: 이 블록이 무엇을 실패해도 아래 평소 적재 경로가 그대로 돈다.
#   fatwrite 가 실패하면 기존 zephyr.bin 이 손상되지 않고 남는다.
# ── OTA 슬롯 크기 = 8 MiB (2026-11-07, 256 KiB 에서 키웠다) ─────────────
#
# 왜 고정 크기인가: 이 U-Boot 에는 **setexpr 가 없다.** 헤더에 적힌 실제
# 크기(ota_addr+4)를 env 변수로 읽어 올 수단이 없으므로 fatwrite 에 상수를
# 줘야 한다. itest.l 은 비교만 하고 대입을 못 한다.
#
# 왜 8 MiB 인가:
#   · 사다리 L4~L5b 이미지가 585~604 KiB 다. 256 KiB 로는 **안 들어갔다** —
#     그래서 그 칸들을 실기에서 돌릴 수 없었다.
#   · 슬롯을 바꿀 때마다 **카드 리더 작업이 필요하다**(OTA 는 zephyr.bin 만
#     쓰고 boot.scr 은 못 쓴다). 그래서 한 번에 넉넉히 잡는다.
#     L6 은 ggml + llama.cpp 모델 그래프라 몇 MB 가 될 수 있다.
#   · 영역은 0x50000000 + 8 MiB → 0x50800000 에서 끝난다. 모델 창
#     (0x58000000)까지 112 MiB 여유다.
#
# 대가: 이미지가 작아도 매번 8 MiB 를 쓴다(SD 에 몇 초). zephyr.bin 파일도
# 8 MiB 가 되지만 fatload 가 sram 창(128 MiB)에 올리고 go 가 진입점으로
# 뛰므로 뒤쪽 쓰레기는 무해하다.
#
# ⚠ **moru 의 MORU_OTA_MAX_SIZE 와 반드시 같아야 한다**
#   (apps/moru/src/moru_shell.c). moru 쪽이 더 크면 이미지가 **조용히 잘려**
#   부팅이 멈춘다. 그래서 moru ota begin 이 256 KiB 를 넘을 때 이 boot.scr
#   버전을 요구한다고 경고한다.

setenv ota_addr 0x50000000
setenv ota_body 0x50000010
setenv ota_magic 0x4d4f5255
setenv ota_magic_try 0x4d4f5452

echo ""
echo "=== PiZZa Zephyr loader ==="

# ── LED: U-Boot 단계에서 가장 먼저 **계속 켜짐** (soc_init/moru_led.c 의 표 참고) ──
# GPIO0_A0 = 출력(DDR_L 0xfdd60008) · 1(DR_L 0xfdd60000). 위 16 비트는 쓰기 허가.
# U-Boot 의 fatload 는 동기라 블롭을 읽는 30~50 초 동안 깜박일 수 없다 —
# 그래서 이 단계는 '켜짐 고정' 이다. Zephyr 가 이어 받아 READY(2 초 점멸)로 바꾼다.
mw.l 0xfdd60008 0x00010001 1
mw.l 0xfdd60000 0x00010001 1

# ── 부팅 슬롯 표식 (Studio F6, soc_init/boot_slot.c 가 읽는다) ──────────────
# 0x51000018 = 'SLOT'(0x544f4c53) · 0x5100001c = 1 try · 2 main · 3 예비(mmc 1:2).
# 먼저 지워 둔다 — 표식 없이 뛰면 Zephyr 는 boot_slot=- 로 낸다.
mw.l 0x51000018 0 1
mw.l 0x5100001c 0 1

if itest.l *${ota_addr} -eq ${ota_magic}; then
	echo "=== OTA image in RAM at ${ota_addr} ==="

	# 지금 것을 .old 로 백업한다 (롤백용)
	if fatload ${devtype} ${devnum}:${distro_bootpart} ${zaddr} zephyr.bin; then
		if fatwrite ${devtype} ${devnum}:${distro_bootpart} ${zaddr} zephyr.bin.old ${filesize}; then
			echo "  backup  zephyr.bin -> zephyr.bin.old (${filesize})"
		else
			echo "  ⚠ backup 실패 — 그래도 적용은 계속한다"
		fi
	fi

	if fatwrite ${devtype} ${devnum}:${distro_bootpart} ${ota_body} zephyr.bin 0x800000; then
		echo "  applied zephyr.bin (8 MiB fixed)"
		# 매직을 지워 다음 부팅에 다시 적용되지 않게 한다
		mw.l ${ota_addr} 0 1
		echo "  magic cleared"
	else
		echo "  ⚠ fatwrite 실패 — 기존 zephyr.bin 을 그대로 쓴다"
	fi
fi

# ── 한 번만 시도하는 실험 슬롯 (2026-11-06 저녁) ─────────────────────────
#
# 왜: 부팅을 막는 설정을 시험할 때마다 카드를 빼서 zephyr.bin.old 를
# 되돌려야 했다. 하루에 네 번 했다. 이 슬롯은 그 비용을 없앤다 —
# **멈추면 전원만 다시 주면 복구된다.**
#
# 쓰기: 매직이 'MOTR' 이면 zephyr.bin **대신 zephyr.bin.try** 에 쓴다.
#       정상 zephyr.bin 은 손대지 않으므로 백업도 필요 없다.
# 읽기: 아래 적재 경로보다 **먼저** .try 를 찾고, 찾으면 **뛰기 전에
#       지운다.** 그러면 그 이미지가 멈춰도 다음 전원 투입에는 파일이
#       없어 평소 zephyr.bin 으로 돌아온다.
if itest.l *${ota_addr} -eq ${ota_magic_try}; then
	echo "=== OTA(try) image in RAM at ${ota_addr} ==="
	if fatwrite ${devtype} ${devnum}:${distro_bootpart} ${ota_body} zephyr.bin.try 0x800000; then
		echo "  staged zephyr.bin.try (one-shot, 8 MiB fixed)"
		mw.l ${ota_addr} 0 1
		echo "  magic cleared"
	else
		echo "  ⚠ fatwrite 실패 — 실험 슬롯을 쓰지 못했다"
	fi
fi

# ── 모델 블롭 적재 (2026-11-07) ───────────────────────────────────────────
#
# QEMU 의 `-device loader,file=…,addr=0x58000000` 에 해당하는 실기 경로다.
# 사다리 L2(GGUF 파서)·L3(역양자화)가 이 블롭을 읽는다.
#
# ⚠ 주소 **0x58000000** 은 apps/llm_common/model_blob.overlay 의 reg 와
#   **반드시 같아야 한다.** 어긋나면 앱이 전부 0 을 읽고, 역양자화 결과가
#   ±0.0 이라 그럴듯해 보여 "포팅이 틀렸다" 로 오진한다(실제로 겪었다).
#   build.sh 가 빌드할 때마다 두 값을 대조한다.
#
# ⚠ **있으면 올리고 없으면 넘어간다.** 블롭을 안 쓰는 앱(L0·L1·L4·L5)과
#   moru 가 이 줄 때문에 부팅을 못 하면 안 된다.
#
# fatload 는 크기 인자를 안 주면 **파일 전체**를 읽는다 — fatwrite 와 달리
# 고정 크기가 필요 없다(헤더에서 크기를 읽을 필요도 없다).
#
# ⚠ **파티션을 두 군데 본다.** boot 파티션(314.6 MB)에는 실제 모델
#   507.85 MiB 가 안 들어간다. 그래서 큰 쪽(파티션 3, 15.6 GB)을 **먼저**
#   보고, 없으면 boot 파티션의 작은 시험 GGUF 로 떨어진다.
#
#   파티션 3 은 2026-11-07 에 Radxa 의 ext4 루트를 지우고 FAT32("BLOB")로
#   다시 만든 것이다(사용자 결정). U-Boot 에 `ext4load` 가 있는지 확인할
#   필요도, macOS 에서 ext4 에 쓸 방법을 찾을 필요도 없어진다 — 지금 쓰는
#   `fatload` 그대로다.
#
#   ⚠ 큰 쪽을 먼저 보는 순서가 중요하다. 작은 시험 GGUF 가 boot 파티션에
#     남아 있으면, 순서가 반대면 **그것이 실제 모델을 가린다** — L2 가
#     통과하는데 L6 은 "모델이 아니다" 로 죽고 원인이 안 보인다.
#
#   ⚠⚠ **그 대가: L2·L3 를 실기에서 다시 돌리려면 파티션 3 의 blob.bin 을
#      치워야 한다.** 두 칸의 골든값은 boot 파티션의 시험 GGUF(3,872 B)에서
#      나온 것이다. 지금 순서로는 실제 모델이 먼저 올라가므로
#        · L3 의 `[0a] 블롭 매직` 은 **통과한다** (실제 모델도 'GGUF' 다)
#        · 그런데 역양자화 골든값이 다른 데이터에서 나와 **[1]·[2] 가 실패**한다
#      매직 점검이 통과해서 더 헷갈린다. L2·L3 는 2026-11-07 에 이미 실기
#      통과를 기록했으니(docs/llm-ladder-status.md) 다시 돌릴 일은 드물지만,
#      돌릴 때는 이 줄을 기억한다:
#        sudo mdel -i /dev/rdisk4s3 ::blob.bin    (또는 이름을 바꿔 둔다)
#
#   ⚠ 508 MiB 를 SD 에서 읽으므로 **부팅이 30~50초 늦어진다.** 아래 echo 가
#     먼저 나가니 멈춘 것으로 보이지 않는다.
#
# fatload 는 크기 인자를 안 주면 **파일 전체**를 읽는다 — fatwrite 와 달리
# 고정 크기가 필요 없다(헤더에서 크기를 읽을 필요도 없다).
# ⚠ **`elif` 를 쓰지 않는다.** U-Boot hush 가 `elif` 를 받는지 확인할 방법이
#   없다 — 이 보드의 U-Boot 는 **1.5 Mbps** 로 말하고 손에 있는 CP2102 는 그
#   속도를 못 낸다(docs/hardware-verified.md:9,124,1434). 즉 U-Boot 출력을
#   **한 줄도 읽을 수 없다.** 문법 오류가 나도 증상은 "조용히 안 된다" 뿐이고
#   원인을 가릴 수단이 없다. 그래서 가장 보수적인 꼴로만 쓴다 —
#   `if … then … else … fi` 중첩이다.
# ── 클럭·전압 조사용 **읽기 전용** 중계 (2026-11-08) ────────────────────
#
# 왜 필요한가: 남은 성능 레버가 CPU 클럭(816 -> 1608 MHz, 1.97배)이고, 그것은
# vdd_cpu 를 I2C 로 올려야 한다. 쓰기 전에 **지금 전압과 어느 VSEL 이 살아
# 있는지**를 알아야 한다 — fan53555 는 VSEL0/VSEL1 둘을 갖고 어느 쪽이
# 활성인지는 칩의 VSEL 핀이 결정한다(소스로는 알 수 없다).
#
# ⚠ U-Boot 는 i2c 명령이 있으나 콘솔이 1.5 Mbps 라 **출력을 읽을 수 없다.**
#   Zephyr 는 출력이 읽히나 I2C 드라이버가 없다. 그래서 U-Boot 가 읽은 값을
#   **메모리에 놓아 두고** Zephyr(apps/llm_l9a)가 그것을 읽어 찍는다.
#
# 주소 0x51000000: sram0(0x40000000~0x50000000) 밖 · OTA 슬롯
#   (0x50000000 + 8 MiB) 밖 · 모델 창(0x58000000~) 밖. DRAM 안이다.
#
# ⚠ **이 블록은 읽기만 한다.** 레귤레이터에도 CRU 에도 쓰지 않는다.
# ⚠ i2c 명령이 이 U-Boot 에 없으면 `if` 가 그냥 넘어간다. 그 경우
#   0x51000008 이 0xdeadbeef 로 남아 L9a 가 "i2c 가 없다" 고 찍는다 —
#   조용히 틀리지 않는다. 그것이 표식을 두는 이유다.
# ⚠ elif 를 쓰지 않는다 (docs/bringup-rules.md 규칙 7).
mw.l 0x51000000 0x4d4f5255 1
mw.l 0x51000008 0xdeadbeef 1
if i2c dev 0; then
	i2c read 0x40 0 3 0x51000008
fi

# ── A단계: vdd_cpu 를 1.025 V 로 (2026-11-08) ───────────────────────────
#
# 목적은 **1416 MHz 를 위한 전압 확보**다. 그런데 이 단계에서는 **클럭을
# 올리지 않는다** (아래 블록은 1104 그대로다). 한 번에 한 변수만 바꾼다.
#
# 왜 나누나: 전압과 클럭을 같이 올리면 **I2C 쓰기가 조용히 실패했을 때**
# 1416 MHz 를 900 mV 로 돌리게 된다(125 mV 저전압). 복구는 되지만 증상이
# "무작위로 멈춘다" 로 나와 원인이 안 보인다. 이 단계가 실패하면 1104 에
# **과전압**이 되는데 그것은 무해하고(1.025 > 0.9), L9a 가 읽어 확인한다.
#
# 값: 0x99 = bit7 EN=1 · bit6 MODE=0 · sel=25
#     V = 712500 + 12500 x 25 = **1025000 uV**
#     (linux rk3566.dtsi opp-table 의 1416 MHz 항목 = 1025000 uV)
#     (linux drivers/regulator/fan53555.c: RK8600 vsel_min 712500 ·
#      vsel_step 12500 · VSEL0 0x00 · VSEL1 0x01 · BUCK_EN BIT(7) · MODE BIT(6))
#
# ⚠ bit7(BUCK_EN)을 **반드시 1 로** 유지한다. sel 만 쓰면 레귤레이터가
#   꺼져 보드가 즉사한다.
# ⚠ MODE(bit6)는 **0 으로 유지**한다. L9a 실측이 VSEL0=0x8f · VSEL1=0x97 로
#   둘 다 0 이었다. 보드 출하 설정을 건드리지 않는다.
# ⚠ VSEL0 과 VSEL1 **양쪽에** 쓴다 — 어느 쪽이 활성인지는 칩의 VSEL 핀이
#   정하고 소스로는 알 수 없다. 1.025 V 는 어느 쪽에 써도 과전압이 아니다
#   (DT 상한 1.39 V, 레지스터 표현 한계 1.5 V).
# ⚠ 올리는 폭은 125 mV(VSEL0 기준) 또는 25 mV(VSEL1 기준)다.
#   ramp-delay 2300 uV/us 기준 약 54 us — 아래 crc32 지연이 넉넉히 덮는다.
#
# 되돌리기: sdcard/boot.cmd.readonly 로 boot.scr 재생성 -> 카드.
# ⚠ **정정 (2026-11-08, 1차 시도 결과).** 전에는 주소를 맨 `0` 과 `1` 로 썼다.
#   결과: **VSEL1(1)만 0x99 가 됐고 VSEL0(0)은 0x8f 그대로였다.**
#   `1` 이 먹었으므로 i2c mw 자체는 동작한다 — 맨 `0` 의 파싱이 유력한 원인이다
#   (U-Boot 의 i2c mw 는 `chip addr[.0,.1,.2] value` 꼴이고 맨 0 이 모호할 수 있다).
#   그래서 **0x00 / 0x01 로 명시**한다.
#
# ⚠ 그리고 **쓰기 직후 다시 읽어** 0x51000010 에 놓아 둔다. 그러면 L9a 가
#   "안 써졌다"(0x8f 유지)와 "써졌다가 되돌아갔다"(쓰기 직후엔 0x99)를
#   구분할 수 있다. 1차 시도에서는 그 구분을 못 했다.
mw.l 0x51000010 0xdeadbeef 1
if i2c dev 0; then
	i2c mw 0x40 0x00 0x99
	i2c mw 0x40 0x01 0x99
	# 쓰기 직후 상태를 따로 떠 둔다 (되돌아감을 가린다)
	i2c read 0x40 0 3 0x51000010
	# 전압이 오를 시간을 준다. crc32 는 이 U-Boot 에 있는 것이 확인돼 있다.
	crc32 0x51000000 0x100000
fi

# ── CPU 816 -> 1104 MHz (2026-11-08) ────────────────────────────────────
#
# **전압을 건드리지 않는다.** L9a 실측으로 지금 vdd_cpu 가
#   VSEL0 = 0x8f (EN=1, sel 15) -> 900000 uV
#   VSEL1 = 0x97 (EN=1, sel 23) -> 1000000 uV
# 임이 확인됐다. 1104 MHz 의 OPP 전압은 **900000 uV** 다
# (linux rk3566.dtsi 의 opp-table, 1104 MHz 항목). 어느 VSEL 이 활성이든
# 900 mV 이상이므로 **전압이 이미 충분하다.** I2C 쓰기가 없으니 과전압도,
# BUCK_EN 을 잘못 지워 보드를 죽일 위험도 없다.
#
# **분주도 건드리지 않는다.** 표 원문:
#   RK3568_CPUCLK_RATE( 816000000, 1, 3)
#   RK3568_CPUCLK_RATE(1104000000, 1, 3)   <- 같다
# L9a 가 읽은 clksel_con[3]=0x0303 · [5] aclk 1 이 816 항목과 일치했으므로
# 그대로 두면 1104 에서도 맞다.
#
# 그래서 바뀌는 것은 **APLL 의 fbdiv 하나(68 -> 92)** 다. 표 원문:
#   RK3036_PLL_RATE( 816000000, 1, 68, 2, 1, 1, 0)   <- L9a 실측과 일치
#   RK3036_PLL_RATE(1104000000, 1, 92, 2, 1, 1, 0)
#   (인자 순서: _mhz, _refdiv, _fbdiv, _postdiv1, _postdiv2, _dsmpd, _frac)
#   VCO = 24 x 92 / 1 = 2208 MHz, postdiv1 2 -> 1104 MHz
#
# 순서는 rk3036_pll_set_rate 원문을 따른다:
#   slow 모드 -> power down -> con 쓰기 -> power up -> lock 대기 -> NORMAL
#
# 상위 16비트가 write-enable 이다 (hardware.h 의 rk_clrsetreg). 아래 모든
# mw.l 이 (마스크<<16)|값 꼴이라 **마스크 밖 비트는 건드려지지 않는다.**
#
# 되돌리기: sdcard/boot.cmd.readonly 로 boot.scr 을 다시 만들어 카드에 쓴다.

# 1. APLL 을 slow 모드로 (MODE_CON bits[1:0] = 0 -> 24 MHz OSC 를 낸다)
mw.l 0xfdd200c0 0x00030000 1
# 2. power down (con1 bit13 = 1)
mw.l 0xfdd20004 0x20002000 1
# 3. con0: fbdiv 92 (0x5c) · postdiv1 2 → (2<<12)|92 = 0x205c, 마스크 0x7fff
mw.l 0xfdd20000 0x7fff205c 1
# 4. con1: refdiv 1 · postdiv2 1 · dsmpd 1 → 1|(1<<6)|(1<<12) = 0x1041
#    마스크 0x11ff 는 bit13(PWRDOWN)을 포함하지 않으므로 power down 이 유지된다
mw.l 0xfdd20004 0x11ff1041 1
# 5. power up (con1 bit13 = 0)
mw.l 0xfdd20004 0x20000000 1
# 6. lock 을 기다린다.
#    U-Boot 에 폴링 루프를 쓸 수 없고 `sleep` 이 이 빌드에 있는지 모른다.
#    crc32 는 **있는 것이 확인돼 있다**(boot.cmd 의 OTA 절 주석 참조) —
#    1 MiB 체크섬이 수십 ms 를 쓰므로 그것을 지연으로 쓴다.
crc32 0x51000000 0x100000
sleep 1
# 7. NORMAL 모드로 (MODE_CON bits[1:0] = 1)
mw.l 0xfdd200c0 0x00030001 1

# ── hub: 모델 고르기 · 로그 저장 (2026-11-08, SD 카드 1단계) ──────────────────────
# 카드 BLOB 파티션(:3): hub.conf (hub_model=<파일> · hub_logseq=<16진>) · models/*.gguf · logs/
# RAM 중계 (apps/hubllm/src/hub_relay.c 와 같은 주소 — 재부팅을 건너 남는다):
#   0x510000F0 'HLOG' · 0x51000300 "hub_loglen=<16진> hub_logseq=<다음>" · 0x51100000 로그 글  → logs/hub-<seq>.txt
#   0x510000F8 'HENV' · 0x51000100 "hub_req=<파일> [hub_save=1]" → 이번에 올릴 모델
#   0x510001F8 'MODL' · 0x51000200 "hub_loaded=… hub_logseq=…" → hubllm 이 읽는다
# 어느 단계가 실패해도 아래 blob.bin 경로로 떨어진다.
setenv hub_model
setenv hub_req
setenv hub_save
setenv hub_dirty
setenv hub_logseq 0
setenv hubcfg 0x51000400
mw.l 0x510001F8 0 1
if fatload ${devtype} ${devnum}:3 ${hubcfg} hub.conf; then
	env import -t ${hubcfg} ${filesize}
	echo "=== hub.conf: hub_model=${hub_model} hub_logseq=${hub_logseq} ==="
fi
if itest.l *0x510000F8 == 0x564e4548; then
	env import -t 0x51000100 0x100
	mw.l 0x510000F8 0 1
	echo "=== hubllm 요청: hub_req=${hub_req} hub_save=${hub_save} ==="
	if test "${hub_save}" = "1"; then
		setenv hub_model ${hub_req}
		setenv hub_dirty 1
	fi
	setenv hub_save
fi
if itest.l *0x510000F0 == 0x474f4c48; then
	setenv hub_loglen
	env import -t 0x51000300 0x40
	# ⚠ setexpr 는 이 U-Boot 에서 동작하지 않는다 — 다음 번호(hub_logseq)는 hubllm 이 위 글에 넣었다
	if fatwrite ${devtype} ${devnum}:3 0x51100000 logs/hub-${hub_logseq}.txt ${hub_loglen}; then
		echo "=== 로그 저장: logs/hub-${hub_logseq}.txt (0x${hub_loglen} B) ==="
		setenv hub_dirty 1
	fi
	mw.l 0x510000F0 0 1
fi
if test "${hub_dirty}" = "1"; then
	env export -t ${hubcfg} hub_model hub_logseq
	if fatwrite ${devtype} ${devnum}:3 ${hubcfg} hub.conf ${filesize}; then
		echo "=== hub.conf 갱신 ==="
	fi
fi
setenv hub_load ${hub_model}
if test -n "${hub_req}"; then
	setenv hub_load ${hub_req}
fi
setenv hub_loaded
if test -n "${hub_load}"; then
	echo "=== models/${hub_load} 읽는다 — 큰 파일은 30~50s ==="
	if fatload ${devtype} ${devnum}:3 0x58000000 models/${hub_load}; then
		echo "=== models/${hub_load} -> 0x58000000 (${filesize} bytes) ==="
		setenv hub_loaded ${hub_load}
	fi
fi

if test -z "${hub_loaded}"; then
echo "=== blob.bin 찾는다 (큰 파티션 3 -> boot 파티션) — 큰 파일은 30~50s ==="
if fatload ${devtype} ${devnum}:3 0x58000000 blob.bin; then
	echo "=== blob.bin -> 0x58000000 (${filesize} bytes, 파티션 3) ==="
	setenv hub_loaded blob.bin
else
	if fatload ${devtype} ${devnum}:${distro_bootpart} 0x58000000 blob.bin; then
		echo "=== blob.bin -> 0x58000000 (${filesize} bytes, boot 파티션) ==="
		setenv hub_loaded boot:blob.bin
	else
		echo "=== blob.bin 없음 — 블롭 없이 계속한다 ==="
	fi
fi
fi
if test -n "${hub_loaded}"; then
	env export -t 0x51000200 hub_loaded hub_logseq
	mw.l 0x510001F8 0x4c444f4d 1
fi

if fatload ${devtype} ${devnum}:${distro_bootpart} ${zaddr} zephyr.bin.try; then
	echo "=== one-shot try image ==="
	# ⚠ 뛰기 전에 지운다. 이 순서가 복구의 전부다.
	if fatrm ${devtype} ${devnum}:${distro_bootpart} zephyr.bin.try; then
		echo "  removed zephyr.bin.try — 멈추면 전원 재투입으로 복구된다"
	else
		echo "  ⚠ fatrm 실패 — 이 이미지가 멈추면 카드를 꺼내야 한다"
	fi
	echo "  jumping to ${zentry}"
	mw.l 0x51000018 0x544f4c53 1
	mw.l 0x5100001c 1 1
	go ${zentry}
fi

if fatload ${devtype} ${devnum}:${distro_bootpart} ${zaddr} zephyr.bin; then
	echo "loaded from ${devtype} ${devnum}:${distro_bootpart}, jumping to ${zentry}"
	mw.l 0x51000018 0x544f4c53 1
	mw.l 0x5100001c 2 1
	go ${zentry}
fi

if fatload mmc 1:2 ${zaddr} zephyr.bin; then
	echo "loaded from mmc 1:2, jumping to ${zentry}"
	mw.l 0x51000018 0x544f4c53 1
	mw.l 0x5100001c 3 1
	go ${zentry}
fi

echo "=== zephyr.bin 을 읽지 못했다 — Linux 로 넘어간다 ==="
