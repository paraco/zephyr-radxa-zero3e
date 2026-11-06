/*
 * L0 관문 — ggml + llama.cpp 그래프 안(B안)이 성립하는지 가린다.
 * 세 기능을 한 파일에 넣는다. 하나라도 막히면 포팅 비용이 두 배가 된다.
 */
#include <zephyr/kernel.h>
#include <vector>
#include <stdexcept>
#include <thread>
#include <cstdio>
#include <pthread.h>

/* ggml 은 std::thread 가 아니라 pthread 를 쓴다 — 그것이 진짜 요구사항이다 */
static void *pworker(void *arg)
{
	auto *v = static_cast<std::vector<float> *>(arg);
	for (auto &x : *v) { x += 1.0f; }
	return nullptr;
}

static void worker(std::vector<float> &v)
{
	for (auto &x : v) {
		x += 1.0f;
	}
}

int main(void)
{
	int fail = 0;

	printk("=== L0: C++17 런타임 관문 ===\n");

	/* ① STL 컨테이너 + 힙 */
	std::vector<float> v(1024, 0.0f);
	printk("  [1] std::vector<float>(1024)  size=%u  ... ", (unsigned)v.size());
	if (v.size() == 1024 && v[0] == 0.0f) { printk("OK\n"); } else { printk("FAIL\n"); fail++; }

	/* ② 예외 — arm64 + 이 libc 에서 되는가 */
	printk("  [2] throw/catch std::runtime_error ... ");
	{
		bool caught = false;
		try {
			throw std::runtime_error("l0");
		} catch (const std::exception &e) {
			caught = (e.what() != nullptr);
		}
		if (caught) { printk("OK\n"); } else { printk("FAIL\n"); fail++; }
	}

	/* ③ pthread — ggml 의 실제 요구사항 */
	printk("  [3] pthread_create + join (ggml 이 쓰는 것) ... ");
	{
		pthread_t th;
		int rc = pthread_create(&th, NULL, pworker, &v);
		if (rc == 0) {
			pthread_join(th, NULL);
			if (v[0] == 1.0f && v[1023] == 1.0f) { printk("OK\n"); }
			else { printk("FAIL (v[0]=%d)\n", (int)v[0]); fail++; }
		} else {
			printk("FAIL (pthread_create rc=%d)\n", rc); fail++;
		}
	}

	/* ④ std::thread — 있으면 좋지만 ggml 필수는 아니다 */
	printk("  [4] std::thread (참고) ... ");
#ifdef _GLIBCXX_HAS_GTHREADS
	{
		std::thread t(worker, std::ref(v));
		t.join();
		printk("%s (v[0]=%d)\n", (v[0] == 2.0f) ? "OK" : "FAIL", (int)v[0]);
	}
#else
	printk("쓸 수 없다 — libstdc++ 가 gthreads 없이 빌드됐다 (ggml 에는 불필요)\n");
	(void)worker;
#endif

	printk("=== L0 %s (실패 %d) ===\n", fail ? "FAIL" : "PASS", fail);
	return fail;
}
