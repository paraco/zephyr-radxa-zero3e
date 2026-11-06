#!/usr/bin/env bash
# tools/host_golden.sh 의 얇은 래퍼. 규약과 이유는 그 파일에 있다.
exec "$(dirname "${BASH_SOURCE[0]}")/host_golden.sh" llm_l4
