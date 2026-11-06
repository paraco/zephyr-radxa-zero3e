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
setenv zentry 0x40001084

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

	if fatwrite ${devtype} ${devnum}:${distro_bootpart} ${ota_body} zephyr.bin 0x40000; then
		echo "  applied zephyr.bin (256 KiB fixed)"
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
	if fatwrite ${devtype} ${devnum}:${distro_bootpart} ${ota_body} zephyr.bin.try 0x40000; then
		echo "  staged zephyr.bin.try (one-shot, 256 KiB fixed)"
		mw.l ${ota_addr} 0 1
		echo "  magic cleared"
	else
		echo "  ⚠ fatwrite 실패 — 실험 슬롯을 쓰지 못했다"
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
