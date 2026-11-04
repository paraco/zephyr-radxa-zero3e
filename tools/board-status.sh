#!/bin/bash
# Radxa Zero 3E 연결 상태 — 3초마다 갱신. Ctrl-C 로 종료.
# 시리얼(CP2102)이 1.5 Mbps 를 못 내므로 USB 가젯과 이더넷으로 상태를 본다.

SUBNET=192.168.219
GREEN=$'\033[32m'; RED=$'\033[31m'; YEL=$'\033[33m'; DIM=$'\033[2m'; B=$'\033[1m'; R=$'\033[0m'

ok()   { printf "  %s%-26s%s %s%s%s %s\n" "$B" "$1" "$R" "$GREEN" "●" "$R" "$2"; }
bad()  { printf "  %s%-26s%s %s%s%s %s\n" "$B" "$1" "$R" "$RED"   "○" "$R" "$2"; }
warn() { printf "  %s%-26s%s %s%s%s %s\n" "$B" "$1" "$R" "$YEL"   "◐" "$R" "$2"; }

# ECM 가젯이 만든 인터페이스를 이름이 아니라 드라이버로 찾는다(번호가 바뀐다)
find_ecm() {
  ioreg -rc AppleUserECM -l 2>/dev/null \
    | grep -o '"BSD Name" = "en[0-9]*"' | head -1 \
    | sed 's/.*"\(en[0-9]*\)"/\1/'
}

while :; do
  clear
  printf "%sRadxa Zero 3E 연결 상태%s   %s%s%s\n\n" "$B" "$R" "$DIM" "$(date '+%H:%M:%S')" "$R"

  # --- USB 가젯 ---
  if ioreg -p IOUSB -w0 2>/dev/null | grep -q "OTG Utils"; then
    ok "USB 가젯 (OTG Utils)" "보드 Linux 부팅 완료 — 가젯 올라옴"
    # ADB 인터페이스: 클래스 255 / 서브클래스 0x42(66)
    if ioreg -rc IOUSBHostInterface -l 2>/dev/null \
         | grep -A3 "OTG Utils" | grep -q '"bInterfaceSubClass" = 66'; then
      if command -v adb >/dev/null; then
        ok "ADB" "$(adb devices 2>/dev/null | sed -n 2p | tr -s '\t ' ' ')"
      else
        warn "ADB" "보드는 내놓음 · Mac 에 adb 없음 (brew install android-platform-tools)"
      fi
    else
      bad "ADB" "가젯에 ADB 인터페이스 없음"
    fi
  else
    bad "USB 가젯" "안 보임 — 전원·USB-C 케이블 확인 (데이터 되는 케이블인지)"
  fi

  # --- USB 이더넷 (ECM) ---
  ECM=$(find_ecm)
  if [ -n "$ECM" ]; then
    MEDIA=$(ifconfig "$ECM" 2>/dev/null | grep -o 'media: .*' | head -1)
    IP4=$(ifconfig "$ECM" 2>/dev/null | awk '/inet /{print $2}')
    IP6=$(ifconfig "$ECM" 2>/dev/null | awk '/inet6 fe80/{print $2}')
    if [ -n "$IP4" ]; then
      ok "USB 이더넷 ($ECM)" "$IP4"
    elif [ -n "$IP6" ]; then
      warn "USB 이더넷 ($ECM)" "링크로컬만: $IP6"
    else
      warn "USB 이더넷 ($ECM)" "주소 없음 · ${MEDIA:-media 미정} — 설정 필요"
    fi
    PEER=$(ndp -an 2>/dev/null | awk -v d="$ECM" '$NF==d && $1 ~ /^fe80/ {print $1}' | head -3 | tr '\n' ' ')
    [ -n "$PEER" ] && ok "  └ 보드 링크로컬" "$PEER"
  else
    bad "USB 이더넷" "ECM 인터페이스 없음"
  fi

  # --- 보드 쪽 인터페이스 (ADB 로 직접 읽는다) ---
  if command -v adb >/dev/null && [ "$(adb get-state 2>/dev/null)" = device ]; then
    BOARD=$(adb shell 'for n in /sys/class/net/*/; do i=${n%/}; i=${i##*/}; [ "$i" = lo ] && continue; printf "%s %s %s %s\n" "$i" "$(cat $n/operstate 2>/dev/null)" "$(cat $n/carrier 2>/dev/null)" "$(ip -o addr show $i 2>/dev/null | awk "/inet /{print \$4}" | head -1)"; done' 2>/dev/null | tr -d '\r')
    while read -r ifc st car ip; do
      [ -z "$ifc" ] && continue
      case "$ifc:$car" in
        eth0:0) bad "  보드 $ifc" "NO-CARRIER — 랜선이 안 붙었습니다" ;;
        eth0:1) if [ -n "$ip" ]; then ok "  보드 $ifc" "$ip"; else warn "  보드 $ifc" "링크 있음 · IP 없음 (DHCP 실패)"; fi ;;
        usb0:*) if [ -n "$ip" ]; then ok "  보드 $ifc" "$ip"; else warn "  보드 $ifc" "state=$st · 보드가 올리지 않음 (root 필요)"; fi ;;
        *)      printf "  %s%-26s%s %s·%s %s\n" "$B" "  보드 $ifc" "$R" "$DIM" "$R" "state=$st ${ip:-IP없음}" ;;
      esac
    done <<< "$BOARD"
  fi

  # --- 유선 이더넷 (Mac 쪽에서 본 새 호스트) ---
  # 브로드캐스트(.255)와 네트워크 주소(.0)는 호스트가 아니다
  NEWHOST=$(arp -an 2>/dev/null | grep "$SUBNET\." \
            | grep -viE "incomplete" | awk '{print $2}' | tr -d '()' \
            | grep -vE "\.(0|255)$" \
            | grep -vxF -f /tmp/.board_arp_baseline 2>/dev/null | tr '\n' ' ')
  if [ -n "$NEWHOST" ]; then
    ok "유선 이더넷 (새 호스트)" "$NEWHOST"
    for ip in $NEWHOST; do
      nc -z -G1 "$ip" 22 2>/dev/null && ok "  └ SSH $ip" "22번 열림 — ssh radxa@$ip"
    done
  else
    warn "유선 이더넷" "$SUBNET.0/24 에 새 호스트 없음 (랜선·공유기 확인)"
  fi

  # --- 시리얼 ---
  if ls /dev/cu.usbserial* >/dev/null 2>&1; then
    warn "시리얼 (CP2102)" "있음 · 보드 콘솔은 1500000 baud — 이 칩은 최대 1 Mbps 라 못 읽음"
  else
    printf "  %s%-26s%s %s%s%s %s\n" "$B" "시리얼 (CP2102)" "$R" "$DIM" "·" "$R" "안 꽂힘"
  fi

  printf "\n%s3초마다 갱신 · Ctrl-C 로 종료%s\n" "$DIM" "$R"
  sleep 3
done
