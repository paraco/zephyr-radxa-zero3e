/*
 * ggml 을 Zephyr 로 빌드하기 위한 호환 층.
 * **모든 ggml 소스에 -include 로 강제 포함된다** (CMakeLists 참고).
 *
 * 원칙: 상류 ggml 소스를 한 줄도 고치지 않는다. 고치면 llama.cpp 를 올릴 때마다
 * 충돌을 손으로 풀어야 하고, 우리 수정과 상류 버그를 구분할 수 없게 된다.
 *
 * 강제 포함이라 **libc 헤더보다 먼저** 들어간다 — 기능 노출 매크로를 여기서
 * 켤 수 있는 이유다.
 */
#ifndef GGML_ZEPHYR_COMPAT_H
#define GGML_ZEPHYR_COMPAT_H

/*
 * ① M_PI.
 * picolibc 의 math.h 는 M_PI 를 `#if __BSD_VISIBLE || __XSI_VISIBLE` 로
 * 감싼다. Zephyr 가 _POSIX_C_SOURCE 를 세우면 __BSD_VISIBLE 이 꺼져 M_PI 가
 * 안 보인다. ggml-cpu/ops.cpp 가 RoPE 에서 쓴다.
 * 손으로 #define M_PI 하면 math.h 와 재정의 충돌이 날 수 있으므로
 * **기능 노출 매크로를 켜서 libc 가 주게 한다.**
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

/*
 * ② _SC_PHYS_PAGES 가 Zephyr 의 sysconf enum 에 없다
 * (zephyr/posix/sys/sysconf.h — _SC_PAGE_SIZE 는 있고 _SC_PHYS_PAGES 는 없다).
 *
 * 쓰는 곳은 ggml-cpu/ggml-cpu.cpp:373 의
 * ggml_backend_cpu_device_get_memory() 하나다 — 장치 정보 보고용이다.
 *
 * 상수만 정의하면 진짜 sysconf 가 -1 을 돌려주고 *total 이 쓰레기가 된다.
 * 그래서 호출 자체를 우리 함수로 돌린다.
 */
#ifndef _SC_PHYS_PAGES
#define _SC_PHYS_PAGES 0x7A50	/* 'zP' — Zephyr 에 없는 옵션이라 우리가 붙인 번호 */
#endif

#ifndef GGML_ZEPHYR_PORT_IMPL
#ifdef __cplusplus
extern "C" {
#endif
long ggml_zephyr_sysconf(int opt);
#ifdef __cplusplus
}
#endif
#define sysconf(x) ggml_zephyr_sysconf(x)
#endif /* GGML_ZEPHYR_PORT_IMPL */

#endif /* GGML_ZEPHYR_COMPAT_H */
