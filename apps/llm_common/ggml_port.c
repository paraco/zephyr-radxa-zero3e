/*
 * ggml 의 플랫폼 의존 조각을 Zephyr 로 채운다.
 * 상류 소스를 고치는 대신 **빼고 다시 구현**한 것들이다.
 */
#include <zephyr/kernel.h>
#include <unistd.h>	/* _SC_PAGE_SIZE 는 Zephyr 의 enum 에 있다 */
#include "ggml-threading.h"

/*
 * ① 임계구역 — 상류 ggml-threading.cpp 를 **빌드하지 않는다.**
 *
 * 그 파일은 12줄이고 전부 std::mutex 다. 그런데 이 툴체인의 libstdc++ 는
 * gthreads 없이 빌드돼 std::mutex 가 없다 (L0 에서 확인한 사실 ②가
 * 여기서 실제로 문제가 됐다 — std::thread 와 같은 원인이다).
 *
 * <mutex> 를 흉내 내는 대신 k_mutex 로 다시 쓴다. 의미가 같고 더 작다.
 */
static K_MUTEX_DEFINE(ggml_cs);

void ggml_critical_section_start(void)
{
	k_mutex_lock(&ggml_cs, K_FOREVER);
}

void ggml_critical_section_end(void)
{
	k_mutex_unlock(&ggml_cs);
}

/*
 * ② sysconf — Zephyr 의 sysconf enum 에 없는 값을 채운다.
 *
 * ggml_zephyr_compat.h 가 `#define sysconf(x) ggml_zephyr_sysconf(x)` 로
 * 호출을 여기로 돌린다. 우리가 컴파일하는 ggml 소스 전체에서 sysconf 를
 * 쓰는 곳은 **두 줄**뿐이다 (ggml-cpu/ggml-cpu.cpp:373-374, 장치 메모리 보고):
 *     sysconf(_SC_PHYS_PAGES)
 *     sysconf(_SC_PAGE_SIZE)
 * 그래서 둘을 직접 구현하고 **아무것도 위임하지 않는다.**
 *
 * ⚠⚠ 처음에는 기본 분기를 `return sysconf(opt);` 로 두고, 파일 앞에
 *    `#define GGML_ZEPHYR_PORT_IMPL` 를 선언해 compat.h 의 매크로를 피하려
 *    했다. **그 방법은 통하지 않는다.** `-include` 강제 포함은 소스 1행보다
 *    먼저 처리되므로 compat.h 가 이미 매크로를 정의한 뒤이고, 파일 안의
 *    재포함은 include guard 로 무시된다. 결과는 **자기 자신 무한 재귀**였다.
 *    objdump 로 `b <자기 주소>` 를 확인했다. L4·L5 는 이 함수를 부르지 않아
 *    초록이었고, llama.cpp 를 올리는 L6 에서 보드가 멈췄을 것이다.
 *    그래서 **위임을 없앴다.** 모르는 opt 는 조용히 틀린 값을 주는 대신
 *    큰 소리로 죽는다.
 */
long ggml_zephyr_sysconf(int opt)
{
	switch (opt) {
	case GGML_ZEPHYR_SC_PHYS_PAGES:
		/*
		 * "이 장치에 메모리가 얼마나 있나" 의 정직한 답은 **ggml 이 실제로
		 * 할당할 수 있는 양**, 즉 libc 힙 아레나다. 모델 가중치 창은
		 * 적재기가 채우는 읽기 전용 영역이라 할당 대상이 아니므로 세지 않는다.
		 */
		return (long)(CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE /
			      CONFIG_POSIX_PAGE_SIZE);
	case _SC_PAGE_SIZE:
		/* 이것은 Zephyr enum 에 있는 값이다 — 우리가 붙인 번호가 아니다 */
		return (long)CONFIG_POSIX_PAGE_SIZE;
	default:
		printk("ggml_zephyr_sysconf: 모르는 opt=%d — 상류가 새 sysconf 를 "
		       "쓰기 시작했다. apps/llm_common/ggml_port.c 에 추가하라\n", opt);
		k_panic();
		return -1;
	}
}
