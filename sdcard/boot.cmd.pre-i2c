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
echo "=== blob.bin 찾는다 (큰 파티션 3 -> boot 파티션) — 큰 파일은 30~50s ==="
if fatload ${devtype} ${devnum}:3 0x58000000 blob.bin; then
	echo "=== blob.bin -> 0x58000000 (${filesize} bytes, 파티션 3) ==="
else
	if fatload ${devtype} ${devnum}:${distro_bootpart} 0x58000000 blob.bin; then
		echo "=== blob.bin -> 0x58000000 (${filesize} bytes, boot 파티션) ==="
	else
		echo "=== blob.bin 없음 — 블롭 없이 계속한다 ==="
	fi
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
	go ${zentry}
fi

if fatload ${devtype} ${devnum}:${distro_bootpart} ${zaddr} zephyr.bin; then
	echo "loaded from ${devtype} ${devnum}:${distro_bootpart}, jumping to ${zentry}"
	go ${zentry}
fi

if fatload mmc 1:2 ${zaddr} zephyr.bin; then
	echo "loaded from mmc 1:2, jumping to ${zentry}"
	go ${zentry}
fi

echo "=== zephyr.bin 을 읽지 못했다 — Linux 로 넘어간다 ==="
