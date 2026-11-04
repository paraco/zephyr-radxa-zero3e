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

echo ""
echo "=== PiZZa Zephyr loader ==="

if fatload ${devtype} ${devnum}:${distro_bootpart} ${zaddr} zephyr.bin; then
	echo "loaded from ${devtype} ${devnum}:${distro_bootpart}, jumping to ${zentry}"
	go ${zentry}
fi

if fatload mmc 1:2 ${zaddr} zephyr.bin; then
	echo "loaded from mmc 1:2, jumping to ${zentry}"
	go ${zentry}
fi

echo "=== zephyr.bin 을 읽지 못했다 — Linux 로 넘어간다 ==="
