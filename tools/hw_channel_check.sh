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

# ③-a tio 가 **지금 열려 있는 어댑터 인스턴스**보다 먼저 시작됐는가
#
# ⚠ 이것이 2026-11-07 ③ 함정의 핵심이다. 어댑터가 빠졌다 다시 꽂히면 노드
#   이름(/dev/cu.usbserial-0001)은 같지만 **다른 장치 인스턴스**다. 먼저
#   시작된 tio 는 **죽은 핸들**을 쥔 채 ps 에는 살아 있다. 노드 존재만
#   보면 "정상" 으로 보인다 — 실제로 내 첫 도구가 그렇게 속았다.
if [[ -n "${TIO:-}" ]]; then
	tio_pid=$(echo "$TIO" | awk '{print $1}')
	tio_start=$(ps -o lstart= -p "$tio_pid" 2>/dev/null | head -1)
	say "tio 시작 시각" "$tio_start"
	say "" "⚠ 어댑터를 다시 꽂았다면 **tio 도 다시 열어야 한다**(ctrl-t q)"
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

# ⑤ **흐름 시험** — --watch N 을 주면 N 초 안에 로그가 자라는지 본다.
#
# 노드 존재·프로세스 존재는 전부 **정적** 확인이고 ③ 함정을 못 잡는다.
# 실제로 바이트가 흐르는지 보는 것이 유일하게 확실하다. 보드가 부팅 중이거나
# 사다리 앱이 도는 중이라면 수 초 안에 무언가 나온다.
#
# ⚠⚠ **쉬고 있는 보드는 아무것도 안 찍는다.** 그것을 "채널이 죽었다" 로
#   단정하면 거짓 음성이다 — 2026-11-07 에 실제로 그랬다(moru 가 부팅을
#   마치고 프롬프트에서 쉬는 중이었는데 12초 무소식을 죽음으로 읽었다).
#
# 그래서 두 단계로 본다:
#   · 최근(기본 120초) 안에 로그가 자랐다면 **그것으로 채널은 증명된다.**
#     흐름 시험을 건너뛴다.
#   · 그렇지 않을 때만 --watch 로 기다린다. 이때도 "보드가 지금 찍어야 하는
#     상태인가" 는 **부르는 쪽이 판단한다** — 적용 직후에만 쓴다.
FRESH=${FRESH:-120}
WATCH=${WATCH:-0}
for a in "$@"; do
	case $a in --watch) WATCH=15 ;; --watch=*) WATCH=${a#*=} ;; esac
done
if (( WATCH > 0 )) && [[ -f $LOG ]]; then
	age=$(( $(date +%s) - $(stat -f %m "$LOG") ))
	if (( age <= FRESH )); then
		echo
		say "흐름" "✅ ${age}초 전에 데이터가 들어왔다 — 채널 증명됨 (대기 생략)"
		say "" "쉬고 있는 보드는 안 찍는다. 적용 직후가 아니면 기다릴 이유가 없다"
		WATCH=0
	fi
fi
if (( WATCH > 0 )) && [[ -f $LOG ]]; then
	before=$(wc -c < "$LOG" | tr -d ' ')
	echo
	echo "── 흐름 시험 (${WATCH}초) — 보드가 뭔가 찍어야 한다"
	for ((t = 0; t < WATCH; t++)); do
		sleep 1
		now=$(wc -c < "$LOG" | tr -d ' ')
		if (( now > before )); then
			say "흐름" "✅ ${t}초에 $((now - before)) 바이트 들어왔다"
			break
		fi
	done
	now=$(wc -c < "$LOG" | tr -d ' ')
	if (( now == before )); then
		say "흐름" "❌ ${WATCH}초 동안 한 바이트도 안 왔다 — **채널이 죽었다**"
		say "" "어댑터를 다시 꽂고 tio 를 **새로** 열어라"
		fail=1
	fi
fi

echo
if (( fail )); then
	echo "❌ 채널에 문제가 있다. **고치기 전에 실기 결과를 해석하지 마라.**"
	exit 1
fi
echo "✅ 채널 정상 — 실기 결과를 신뢰할 수 있다"
