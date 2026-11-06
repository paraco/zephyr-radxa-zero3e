/*
 * 전체 텐서 요약 — 표본 밖 영역에 **상한**을 준다.
 *
 * 큰 출력은 골든 헤더 크기 때문에 보폭 표본만 값 비교한다(예: L5a 의 순환
 * 상태는 262,144개 중 1,024개 = 0.4%). CRC32 는 차이를 **감지**하지만
 * 허용 오차 개념이 없어 libm 1 ulp 로도 달라지고, 그래서 불일치를 실패로
 * 셀 수 없다. 결과적으로 **표본에 안 걸리는 영역이 틀려도 통과**한다.
 *
 * 그래서 전체에 대한 두 가지 요약을 골든에 넣고 허용 오차로 비교한다:
 *   sum    — 부호가 뒤집히거나 큰 값이 어긋나면 드러난다
 *   abssum — 상쇄가 없어 조건이 좋다. 원소 하나가 크게 틀어지면 드러난다
 *
 * 이것으로 표본 밖 영역에도 상한이 생긴다. 완전한 동치는 아니지만
 * "아무 상한도 없는" 상태와는 다르다.
 */
#ifndef LLM_FULLSUM_H
#define LLM_FULLSUM_H

#include <stddef.h>

struct llm_fullsum {
	float sum;
	float abssum;
};

static inline struct llm_fullsum llm_fullsum_of(const float *p, size_t n)
{
	struct llm_fullsum r = { 0.0f, 0.0f };

	/* 누산 순서를 고정한다 — 호스트와 Zephyr 가 같은 순서로 돌아야 한다 */
	for (size_t i = 0; i < n; i++) {
		float v = p[i];

		r.sum += v;
		r.abssum += (v < 0.0f) ? -v : v;
	}
	return r;
}

#endif /* LLM_FULLSUM_H */
