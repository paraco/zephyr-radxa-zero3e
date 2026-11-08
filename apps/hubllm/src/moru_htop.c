/*
 * moru htop — 전체화면 실시간 모니터
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 왜 이것만 만드는가
 * ------------------
 * `ps`·`mem` 은 **만들지 않는다.** CONFIG_THREAD_RUNTIME_STATS 를 켜면
 * 내장 `kernel thread list` 가 이미 스레드별 실행 사이클과 부팅 이후
 * 비율을 찍고(kernel_service/thread/list.c:30-48), `kernel heap` 이 힙을
 * 찍는다. 중복 구현하지 않는다는 원칙에 따른다.
 *
 * 내장 명령이 못 하는 것만 여기서 한다 — **창 단위 CPU%**(부팅 이후 평균이
 * 아니라 '지금' 쓰는 비율)와 **살아서 갱신되는 화면**이다.
 *
 * 같은 일을 PiZZa/sketches/PiZZaHtop(Pi Zero 2 W)에서는 전부 손으로
 * 만들어야 했다 — llext 모듈이라 k_thread_foreach·runtime_stats·mallinfo 가
 * 막혀 스택 페인팅·CPU 누적·MPIDR 직독을 직접 구현했다. 이 앱은 온전한
 * Zephyr 앱이므로 커널에서 그대로 받는다.
 *
 * 세 가지 함정 (실제로 걸릴 수 있어 미리 처리한다)
 * -----------------------------------------------
 * ① k_thread_foreach_unlocked 콜백 안에서 **출력하지 않는다.** 셸 출력은
 *    TX 링버퍼가 차면 블록하므로(z_shell_write 가 버리지 않고 기다린다 —
 *    shell_ops.c:457-474), 스레드 목록을 훑는 동안 블록하면 안 된다.
 *    배열에 모으고 밖에서 찍는다.
 * ② `q` 는 셸의 bypass 로 받을 수 없다. bypass 는 shell_process() 안에서
 *    처리되는데(shell.c:1458-1466), 이 핸들러가 도는 동안에는 그 루프가
 *    돌지 않는다. 그래서 셸이 bypass 때 쓰는 것과 **같은 전송 계층 read** 를
 *    직접 폴링한다. 0x03(Ctrl-C)도 같은 이유로 직접 처리한다 — 셸의
 *    Ctrl-C 처리 역시 shell_process() 안에 있다.
 * ③ 프레임마다 화면 전체를 지우지 않는다(\e[2J). 커서를 홈으로 보내고
 *    줄마다 \e[K 로 지운다 — 깜빡임과 바이트 수가 줄어든다. 마지막 줄
 *    뒤에 \e[J 를 한 번 넣어 **스레드 수가 줄었을 때 남는 옛 줄**을 치운다.
 *
 * ⚠ htop 이 도는 동안 셸 로그 백엔드는 비워지지 않는다 — 그 백엔드를
 *   비우는 것도 shell_process() 다. 로그는 큐에 쌓이고(큐 8192,
 *   타임아웃 1000 ms) 큐가 차면 로그 처리 스레드가 막힌다. 그 스레드는
 *   uart2 백엔드도 함께 돌리므로 긴 htop 중 USB 사건이 몰리면 uart2 로그도
 *   늦어진다. 유휴 상태에서는 일어나지 않는다.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/sys/mem_stats.h>
#include <string.h>
#include <stdlib.h>

#include "moru.h"

/* 이 보드의 스레드는 10여 개다. 넉넉히 두고, 넘치면 넘쳤다고 찍는다. */
#define MORU_HTOP_MAX_THREADS	24
#define MORU_HTOP_MIN_MS	200
#define MORU_HTOP_MAX_MS	10000
/* 터미널이 사라진 채 영원히 돌지 않도록 상한을 둔다 */
#define MORU_HTOP_MAX_SEC	600
/* 키를 얼마나 자주 보는가 — 간격을 이 단위로 쪼개 잔다 */
#define MORU_HTOP_POLL_MS	50

struct htop_row {
	k_tid_t  tid;
	const char *name;
	int      prio;
	char     state[16];
	size_t   stack_size;
	size_t   stack_unused;
	uint64_t cycles;	/* 부팅 이후 누적 */
	uint32_t pct_x10;	/* 이번 창의 CPU 사용률 ×10 */
};

struct htop_prev {
	k_tid_t  tid;
	uint64_t cycles;
};

struct htop_collect {
	struct htop_row *rows;
	int n;
	int overflow;
};

static void htop_collect_cb(const struct k_thread *thread, void *user_data)
{
	struct htop_collect *c = user_data;
	k_tid_t tid = (k_tid_t)thread;
	struct htop_row *r;
	k_thread_runtime_stats_t st;
	size_t unused = 0;

	if (c->n >= MORU_HTOP_MAX_THREADS) {
		c->overflow++;
		return;
	}
	r = &c->rows[c->n++];

	r->tid = tid;
	r->name = k_thread_name_get(tid);
	if (r->name == NULL || r->name[0] == '\0') {
		r->name = "(이름 없음)";
	}
	r->prio = k_thread_priority_get(tid);
	(void)k_thread_state_str(tid, r->state, sizeof(r->state));

	/*
	 * 스택 전체 크기는 stack_info 에서, 쓰지 않은 양은
	 * k_thread_stack_space_get 이 센다 (CONFIG_INIT_STACKS 로 칠해 둔
	 * 값이 남아 있는 구간을 센다 — prj.conf 가 켜 둔다).
	 */
	r->stack_size = thread->stack_info.size;
	/*
	 * ⚠ k_thread_stack_space_get() 은 **CONFIG_INIT_STACKS 가 있어야
	 *   링크된다** (z_impl_k_thread_stack_space_get 미정의로 링크가 깨진다 —
	 *   실제로 깨졌다). 쓰지 않은 양을 세는 방식이 '칠해 둔 값이 남아
	 *   있는 구간' 이라 당연하다.
	 *   없는 구성에서는 사용량을 모른다고 표시하고(바 대신 '—') 나머지
	 *   (우선순위·상태·CPU%·힙)는 그대로 쓴다.
	 */
#ifdef CONFIG_INIT_STACKS
	if (k_thread_stack_space_get(thread, &unused) != 0) {
		unused = 0;
	}
#else
	unused = 0;
	r->stack_size = 0;	/* 0 = 모른다는 표시 */
#endif
	r->stack_unused = unused;

	/*
	 * ⚠ CONFIG_THREAD_RUNTIME_STATS 가 없으면 **구조체에 멤버 자체가
	 *   없다.** 함수는 링크되고 0 을 돌려주지만(kernel/thread.c:1161-1175)
	 *   execution_cycles 를 참조하면 컴파일이 깨진다 — 실제로 깨졌다.
	 *   그래서 가드가 필요하다. 없는 구성에서는 CPU% 가 0.0 으로 나오고
	 *   htop 의 나머지(스택·우선순위·상태·힙)는 그대로 쓸 수 있다.
	 */
	r->cycles = 0;
#ifdef CONFIG_THREAD_RUNTIME_STATS
	if (k_thread_runtime_stats_get(tid, &st) == 0) {
		r->cycles = st.execution_cycles;
	}
#else
	ARG_UNUSED(st);
#endif
	r->pct_x10 = 0;
}

/** 지난 프레임의 누적값과 비교해 이번 창의 비율을 낸다. */
static void htop_window_pct(struct htop_row *rows, int n,
			    struct htop_prev *prev, int *prev_n,
			    uint64_t win_cycles, bool first)
{
	for (int i = 0; i < n; i++) {
		uint64_t used = rows[i].cycles;

		if (!first) {
			uint64_t before = 0;
			bool found = false;

			for (int j = 0; j < *prev_n; j++) {
				if (prev[j].tid == rows[i].tid) {
					before = prev[j].cycles;
					found = true;
					break;
				}
			}
			/*
			 * 처음 보는 스레드는 창 전체를 쓴 것으로 치지 않는다 —
			 * 생긴 시점을 모르므로 0 으로 둔다. 다음 프레임부터
			 * 정상 값이 나온다.
			 */
			if (!found) {
				rows[i].pct_x10 = 0;
				continue;
			}
			used = (rows[i].cycles >= before)
				? rows[i].cycles - before : 0;
		}

		if (win_cycles != 0) {
			rows[i].pct_x10 =
				(uint32_t)((used * 1000ULL) / win_cycles);
			if (rows[i].pct_x10 > 1000U) {
				rows[i].pct_x10 = 1000U;
			}
		}
	}

	*prev_n = n;
	for (int i = 0; i < n; i++) {
		prev[i].tid = rows[i].tid;
		prev[i].cycles = rows[i].cycles;
	}
}

/** CPU% 내림차순. 스레드가 20여 개라 삽입 정렬로 충분하다. */
static void htop_sort(struct htop_row *rows, int n)
{
	for (int i = 1; i < n; i++) {
		struct htop_row key = rows[i];
		int j = i - 1;

		while (j >= 0 && rows[j].pct_x10 < key.pct_x10) {
			rows[j + 1] = rows[j];
			j--;
		}
		rows[j + 1] = key;
	}
}

static void htop_bar(char *out, size_t out_sz, unsigned int pct)
{
	const unsigned int width = 10;
	unsigned int fill = (pct * width + 50U) / 100U;

	if (fill > width) {
		fill = width;
	}
	if (out_sz < width + 3U) {
		if (out_sz) {
			out[0] = '\0';
		}
		return;
	}
	out[0] = '[';
	for (unsigned int i = 0; i < width; i++) {
		out[1 + i] = (i < fill) ? '#' : '.';
	}
	out[1 + width] = ']';
	out[2 + width] = '\0';
}

/*
 * ⚠ 셸이 bypass 에서 쓰는 것과 같은 전송 계층 read 를 직접 부른다.
 *   핸들러가 도는 동안 shell_process() 가 멈춰 bypass 콜백도 Ctrl-C 처리도
 *   돌지 않으므로, 키를 받을 다른 길이 없다.
 */
static bool htop_quit(const struct shell *sh)
{
	uint8_t buf[16];
	size_t cnt = 0;

	if (sh->iface->api->read(sh->iface, buf, sizeof(buf), &cnt) != 0) {
		return false;
	}
	for (size_t i = 0; i < cnt; i++) {
		if (buf[i] == 'q' || buf[i] == 'Q' || buf[i] == 0x03) {
			return true;
		}
	}
	return false;
}

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS) && (K_HEAP_MEM_POOL_SIZE > 0)
/*
 * 내장 `kernel heap` 도 같은 심볼을 쓴다
 * (kernel_service/heap.c:11). 공개 API 로는 커널 힙에 닿을 길이 없다.
 */
extern struct k_heap _system_heap;

static void htop_heap_line(const struct shell *sh)
{
	struct sys_memory_stats st;

	if (sys_heap_runtime_stats_get(&_system_heap.heap, &st) != 0) {
		shell_fprintf(sh, SHELL_NORMAL, "  힙  읽지 못했다\033[K\r\n");
		return;
	}
	shell_fprintf(sh, SHELL_NORMAL,
		      "  힙  여유 %zu B · 사용 %zu B · 최대 %zu B\033[K\r\n",
		      st.free_bytes, st.allocated_bytes,
		      st.max_allocated_bytes);
}
#else
static void htop_heap_line(const struct shell *sh)
{
	shell_fprintf(sh, SHELL_NORMAL,
		      "  힙  통계 없음 (SYS_HEAP_RUNTIME_STATS·HEAP_MEM_POOL_SIZE)\033[K\r\n");
}
#endif

static void htop_render(const struct shell *sh, struct htop_row *rows, int n,
			int overflow, uint32_t interval_ms, uint32_t frame)
{
	int64_t up = k_uptime_get();
	uint32_t freq = sys_clock_hw_cycles_per_sec();

	/* 커서를 홈으로 — 화면 전체를 지우지 않는다 (함정 ③) */
	shell_fprintf(sh, SHELL_NORMAL, "\033[H");
	shell_fprintf(sh, SHELL_NORMAL,
		      "MoruLab htop — 가동 %ud %02u:%02u:%02u · %u.%03u MHz · 갱신 %u ms · 프레임 %u%s\033[K\r\n",
		      (unsigned int)(up / 86400000),
		      (unsigned int)((up / 3600000) % 24),
		      (unsigned int)((up / 60000) % 60),
		      (unsigned int)((up / 1000) % 60),
		      freq / 1000000U, (freq % 1000000U) / 1000U,
		      interval_ms, frame,
		      IS_ENABLED(CONFIG_THREAD_RUNTIME_STATS)
			      ? (IS_ENABLED(CONFIG_INIT_STACKS)
					 ? ""
					 : " · 스택 없음(INIT_STACKS 꺼짐)")
			      : (IS_ENABLED(CONFIG_INIT_STACKS)
					 ? " · CPU% 없음(THREAD_RUNTIME_STATS 꺼짐)"
					 : " · CPU%·스택 없음"));
	htop_heap_line(sh);
	shell_fprintf(sh, SHELL_NORMAL,
		      "  스레드 %d개%s · q 또는 Ctrl-C 로 나간다\033[K\r\n",
		      n, overflow ? " (+넘침)" : "");
	shell_fprintf(sh, SHELL_NORMAL, "\033[K\r\n");
	shell_fprintf(sh, SHELL_NORMAL,
		      " 우선  CPU%%   스택 사용            상태        이름\033[K\r\n");

	for (int i = 0; i < n; i++) {
		struct htop_row *r = &rows[i];
		size_t used = (r->stack_size > r->stack_unused)
			? r->stack_size - r->stack_unused : 0;
		unsigned int spct = r->stack_size
			? (unsigned int)((used * 100U) / r->stack_size) : 0;
		char bar[16];

		if (r->stack_size == 0U) {
			shell_fprintf(sh, SHELL_NORMAL,
				      " %4d  %3u.%1u  %-22s %-11s %s\033[K\r\n",
				      r->prio, r->pct_x10 / 10U,
				      r->pct_x10 % 10U, "—",
				      r->state, r->name);
			continue;
		}
		htop_bar(bar, sizeof(bar), spct);
		shell_fprintf(sh, SHELL_NORMAL,
			      " %4d  %3u.%1u  %s %3u%% %5zu/%-5zu %-11s %s\033[K\r\n",
			      r->prio, r->pct_x10 / 10U, r->pct_x10 % 10U,
			      bar, spct, used, r->stack_size,
			      r->state, r->name);
	}
	if (overflow) {
		shell_fprintf(sh, SHELL_NORMAL,
			      " … %d개 더 있다 (MORU_HTOP_MAX_THREADS=%d)\033[K\r\n",
			      overflow, MORU_HTOP_MAX_THREADS);
	}
	/* 스레드 수가 줄었을 때 남는 옛 줄을 치운다 (함정 ③) */
	shell_fprintf(sh, SHELL_NORMAL, "\033[J");
}

int cmd_moru_htop(const struct shell *sh, size_t argc, char **argv)
{
	static struct htop_row rows[MORU_HTOP_MAX_THREADS];
	static struct htop_prev prev[MORU_HTOP_MAX_THREADS];
	int prev_n = 0;
	uint32_t interval_ms = 1000;
	uint64_t last_cyc;
	uint32_t frame = 0;
	bool first = true;
	int64_t deadline;

	if (argc > 1) {
		long v = strtol(argv[1], NULL, 10);

		if (v < MORU_HTOP_MIN_MS || v > MORU_HTOP_MAX_MS) {
			shell_error(sh, "갱신 간격은 %d~%d ms",
				    MORU_HTOP_MIN_MS, MORU_HTOP_MAX_MS);
			return -EINVAL;
		}
		interval_ms = (uint32_t)v;
	}

	/* 화면을 한 번만 지우고 커서를 숨긴다 */
	shell_fprintf(sh, SHELL_NORMAL, "\033[2J\033[H\033[?25l");

	last_cyc = k_cycle_get_64();
	deadline = k_uptime_get() + (int64_t)MORU_HTOP_MAX_SEC * 1000;

	while (true) {
		struct htop_collect c = { .rows = rows, .n = 0, .overflow = 0 };
		uint64_t now_cyc;
		uint64_t win;
		int64_t slept = 0;

		/* 함정 ①: 모으기만 한다. 출력은 밖에서. */
		k_thread_foreach_unlocked(htop_collect_cb, &c);

		now_cyc = k_cycle_get_64();
		win = now_cyc - last_cyc;
		last_cyc = now_cyc;

		htop_window_pct(rows, c.n, prev, &prev_n, win, first);
		first = false;
		htop_sort(rows, c.n);
		htop_render(sh, rows, c.n, c.overflow, interval_ms, ++frame);

		/* 간격을 쪼개 자면서 키를 본다 (함정 ②) */
		while (slept < (int64_t)interval_ms) {
			if (htop_quit(sh)) {
				goto done;
			}
			k_msleep(MORU_HTOP_POLL_MS);
			slept += MORU_HTOP_POLL_MS;
		}
		if (k_uptime_get() > deadline) {
			shell_fprintf(sh, SHELL_NORMAL, "\033[?25h\r\n");
			shell_warn(sh, "%d초 상한에 걸려 멈춘다 (다시 켜면 된다)",
				   MORU_HTOP_MAX_SEC);
			return 0;
		}
	}

done:
	/* 커서를 되살리고 화면을 치운다 */
	shell_fprintf(sh, SHELL_NORMAL, "\033[?25h\033[2J\033[H");
	shell_print(sh, "htop 종료 — 프레임 %u개", frame);
	return 0;
}
