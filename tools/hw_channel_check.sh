#!/usr/bin/env bash
# 실기 투입 **전에** 관측 채널이 살아 있는지 확인한다.
#
#   ./tools/hw_channel_check.sh
#
# 왜 필요한가: 2026-11-07 하루에 **세 번** 같은 함정을 밟았다.
#   ① UART2 젬퍼가 빠져 있었다 → "펌웨어가 부팅을 막는다" 로 오진
#   ② printk 가 1 KB 지연 버퍼로 우회돼 출력 꼬리만 남았다 → "조용하다" 로 오독
#   ③ CP2102 어댑터가 USB 에서 빠졌다 → tio 는 살아 있는데 포트가 없었다
# docs/bringup-rules.md 규칙 6: **관측 채널이 조용하다 ≠ 피검체가 조용하다.**
set -uo pipefail

fail=0
say()  { printf "  %-34s %s\n" "$1" "$2"; }

echo "── 관측 채널 점검 ──"

# ① UART 어댑터가 USB 에 있는가
UARTS=(/dev/cu.usbserial*)
if [[ -e "${UARTS[0]}" ]]; then
	say "UART 어댑터" "✅ ${UARTS[*]}"
else
	say "UART 어댑터" "❌ /dev/cu.usbserial* 없음 — CP2102 를 꽂아라"
	fail=1
fi

# ② tio 가 **존재하는** 포트를 쥐고 있는가
TIO=$(ps -Ao pid,command | grep -E "[t]io /dev/cu.usbserial" || true)
if [[ -n "$TIO" ]]; then
	port=$(echo "$TIO" | grep -oE "/dev/cu\.usbserial[^ ]*" | head -1)
	if [[ -e "$port" ]]; then
		say "tio" "✅ $port 를 쥐고 있다"
	else
		say "tio" "❌ **없는 포트($port)를 쥐고 있다** — ctrl-t q 로 닫고 다시 열어라"
		fail=1
	fi
else
	say "tio" "⚠ 안 돌고 있다 — 로그를 받을 수 없다"
	fail=1
fi

# ③ 로그 파일이 **최근에** 자랐는가 (채널이 실제로 흐르는가)
LOG=${HW_LOG:-/tmp/l0-hw.log}
if [[ -f $LOG ]]; then
	age=$(( $(date +%s) - $(stat -f %m "$LOG") ))
	say "로그 파일" "$LOG ($(wc -c < "$LOG" | tr -d ' ') B, ${age}초 전 갱신)"
	if (( age > 600 )); then
		say "" "⚠ 10분 넘게 조용하다 — 채널이 끊겼을 수 있다"
	fi
else
	say "로그 파일" "⚠ $LOG 없음"
fi

# ④ 보드 셸(USB CDC)이 보이는가 — moru 가 돌고 있다는 뜻
if compgen -G "/dev/cu.usbmodem*" >/dev/null; then
	say "보드 셸 (USB CDC)" "✅ moru 가 돌고 있다 — 전송·적용 가능"
else
	say "보드 셸 (USB CDC)" "⚠ 없다 — 사다리 앱이 돌고 있거나 멈췄다 (전원 재투입)"
fi

echo
if (( fail )); then
	echo "❌ 채널에 문제가 있다. **고치기 전에 실기 결과를 해석하지 마라.**"
	exit 1
fi
echo "✅ 채널 정상 — 실기 결과를 신뢰할 수 있다"
