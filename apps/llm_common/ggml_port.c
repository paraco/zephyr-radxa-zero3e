/*
 * ggml 의 플랫폼 의존 조각을 Zephyr 로 채운다.
 * 상류 소스를 고치는 대신 **빼고 다시 구현**한 것들이다.
 */
#define GGML_ZEPHYR_PORT_IMPL	/* 호환 층의 sysconf 매크로를 피한다 */
#include "ggml_zephyr_compat.h"

#include <zephyr/kernel.h>
#include <unistd.h>
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
 * ② sysconf — _SC_PHYS_PAGES 를 채운다.
 *
 * "이 장치에 메모리가 얼마나 있나" 에 대한 정직한 답은 **ggml 이 실제로
 * 할당할 수 있는 양**, 즉 libc 힙 아레나다. 모델 가중치 창(640 MiB)은
 * 적재기가 채우는 읽기 전용 영역이라 할당 대상이 아니므로 세지 않는다.
 */
long ggml_zephyr_sysconf(int opt)
{
	switch (opt) {
	case _SC_PHYS_PAGES:
		return (long)(CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE /
			      CONFIG_POSIX_PAGE_SIZE);
	default:
		return sysconf(opt);	/* 여기서는 매크로가 없다 */
	}
}
