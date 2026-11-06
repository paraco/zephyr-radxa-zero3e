#!/usr/bin/env bash
# 사다리 전체를 돌려 기준선을 세운다.
#
#   ./tools/run_ladder.sh            # 전부
#   ./tools/run_ladder.sh llm_l4 llm_l5a
#
# 큰 변경(소스 목록·컴파일 플래그·하네스) 뒤에는 이것을 한 번 통과시킨 뒤
# 연관 시험만 돌린다 — 전역 지침의 "전체 성공(기준선) → 연관 시험" 순서.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
GGUF="${L2_GGUF:-/tmp/claude-501/l2_test.gguf}"

# ⚠ 아래에서 앱을 **상대경로**로 넘기므로 저장소 루트에서 돌아야 한다.
#   없으면 다른 디렉터리에서 실행할 때 전부 "빌드 실패" 로 나와 진단이 틀린다.
cd "$REPO"

APPS=("$@")
if [[ ${#APPS[@]} -eq 0 ]]; then
	APPS=(llm_l0 llm_l1 llm_l2 llm_l3 llm_l4 llm_l5a llm_l5b)
fi

# 블롭이 필요한 앱 — **표식 파일**로 판정한다. qemu_run.sh 와 같은 기준이다.
# 하드코딩하면 새 앱(L6)이 빠져 블롭 없이 돌 수 있다.
needs_blob() { [[ -f "$REPO/apps/$1/NEEDS_BLOB" ]]; }

pass=0; failed=0; declare -a SUMMARY=()
for app in "${APPS[@]}"; do
	printf '\n════════ %s ════════\n' "$app"
	if needs_blob "$app" && [[ ! -f $GGUF ]]; then
		# ⚠ 건너뜀을 성공으로 세면 "전체 통과" 가 거짓이 된다. 실패로 센다.
		echo "건너뜀 — 시험 GGUF 가 없다: $GGUF"
		echo "   ./.venv/bin/python tools/l2_make_gguf.py"
		failed=$((failed + 1))
		SUMMARY+=("$app: 건너뜀(GGUF 없음) — 실패로 센다")
		continue
	fi
	if needs_blob "$app"; then
		L_BLOB="$GGUF" "$HERE/qemu_run.sh" "apps/$app" 300 2>&1 | tail -n 40
	else
		"$HERE/qemu_run.sh" "apps/$app" 300 2>&1 | tail -n 40
	fi
	rc=${PIPESTATUS[0]}
	case $rc in
	0) pass=$((pass + 1));   SUMMARY+=("$app: PASS") ;;
	1) failed=$((failed + 1)); SUMMARY+=("$app: FAIL") ;;
	2) failed=$((failed + 1)); SUMMARY+=("$app: 판정 문장 없음 (조용한 죽음 또는 제한시간)") ;;
	*) failed=$((failed + 1)); SUMMARY+=("$app: 빌드 실패 (종료코드 $rc)") ;;
	esac
done

printf '\n════════ 요약 ════════\n'
for l in "${SUMMARY[@]}"; do echo "  $l"; done
printf '통과 %d · 실패 %d\n' "$pass" "$failed"
[[ $failed -eq 0 ]]
