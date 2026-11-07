/*
 * ggml_backend_reg_zephyr.cpp — 상류 `ggml-backend-reg.cpp` 의 **대체판**.
 *
 * 포팅 항목 7번. 근거는 docs/l6-decisions.md 결정 2.
 *
 * ── 왜 대체하나 ──────────────────────────────────────────────────────────
 *
 * 상류 파일은 `<filesystem>`(`:7`)과 `dlopen`(`:112`)에 의존하고 백엔드 헤더
 * 15개를 `#ifdef` 로 끌어온다(`:30~90`). Zephyr 에는 파일 시스템도 동적
 * 링크도 없고, 이 툴체인의 libstdc++ 는 `<filesystem>` 을 링크하지 못한다
 * (`ggml-threading.cpp` 의 `std::mutex` 와 같은 사정이다).
 *
 * 그런데 **밖으로 내보내는 함수는 15개뿐**이고 전부 레지스트리 vector 위의
 * 얇은 래퍼다. 필요한 것은 CPU 백엔드 **하나**를 등록하는 것이다.
 *
 * 상류를 고치면 `git pull` 마다 충돌한다. 지금까지 **상류 수정 0건**으로
 * 왔으므로(ggml_port.c · ggml_zephyr_compat.h · zephyr_future.h) 같은 방식을
 * 쓴다 — `cmake/ggml-sources.txt` 에서 상류 파일을 빼고 이 파일을 링크한다.
 *
 * ⚠ `ggml_backend_dev_name` 같은 **장치 래퍼는 이 파일의 몫이 아니다.**
 *   그것들은 `ggml-backend.cpp` 에 있고 그 파일은 그대로 쓴다.
 *
 * ── 동적 적재는 조용히 실패시키지 않는다 ────────────────────────────────
 *
 * `ggml_backend_load` 류가 `nullptr` 만 돌려주면, 백엔드를 못 찾은 이유가
 * "없다" 인지 "이 포팅이 못 한다" 인지 알 수 없다. `k_panic()` 으로 막는다.
 * 검토에서 `ggml_zephyr_sysconf` 의 무한 재귀를 잡아낸 그 규율이다 —
 * **모르는 입력에는 멈춘다.**
 */
#include "ggml-backend.h"
#include "ggml-backend-impl.h"
#include "ggml-impl.h"
#include "ggml-cpu.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <cstring>
#include <vector>

namespace {

struct zephyr_registry {
	std::vector<ggml_backend_reg_t> backends;
	std::vector<ggml_backend_dev_t> devices;

	zephyr_registry()
	{
		/*
		 * 상류는 `#ifdef GGML_USE_CPU` 안에서 이것을 부른다
		 * (ggml-backend-reg.cpp:338 부근). 우리는 CPU 가 유일한
		 * 백엔드이므로 조건 없이 부른다. 정의는
		 * ggml-cpu/ggml-cpu.cpp 에 있고 `<filesystem>` 을 쓰지 않는다.
		 */
		register_backend(ggml_backend_cpu_reg());
	}

	void register_backend(ggml_backend_reg_t reg)
	{
		if (reg == nullptr) {
			printk("ggml_backend_reg_zephyr: ggml_backend_cpu_reg() 가 "
			       "nullptr 를 돌려줬다 — CPU 백엔드가 없으면 아무것도 못 한다\n");
			k_panic();
			return;
		}
		for (auto r : backends) {
			if (r == reg) {
				return;
			}
		}
		backends.push_back(reg);
		for (size_t i = 0; i < ggml_backend_reg_dev_count(reg); i++) {
			devices.push_back(ggml_backend_reg_dev_get(reg, i));
		}
	}

	void register_device(ggml_backend_dev_t dev)
	{
		if (dev == nullptr) {
			return;
		}
		for (auto d : devices) {
			if (d == dev) {
				return;
			}
		}
		devices.push_back(dev);
	}
};

/*
 * 함수 지역 static — 첫 호출에서 만들어진다. 상류의 `get_reg()` 와 같은 꼴이고,
 * 전역 생성자 순서에 매달리지 않는다(Zephyr 에서 C++ 전역 생성자는
 * CONFIG_CPP_STATIC_INIT_GNU 에 달려 있어 피하는 쪽이 안전하다).
 *
 * ⚠ 스레드 안전하지 않다. 상류도 그렇다. 첫 `ggml_backend_dev_*` 호출은
 *   main 에서 한 번 하고 그 뒤에 일꾼 스레드를 띄워야 한다 — L6 앱이 그렇게 한다.
 */
zephyr_registry &get_reg()
{
	static zephyr_registry reg;
	return reg;
}

bool striequals(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
		char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
		if (ca != cb) {
			return false;
		}
	}
	return *a == *b;
}

[[noreturn]] void no_dynamic_loading(const char *what)
{
	printk("ggml_backend_reg_zephyr: %s — Zephyr 에는 동적 적재가 없다.\n"
	       "  백엔드를 늘리려면 zephyr_registry 생성자에 정적으로 더해라.\n"
	       "  (조용히 nullptr 를 돌려주면 원인을 가릴 수 없어 여기서 멈춘다)\n",
	       what);
	k_panic();
	CODE_UNREACHABLE;
}

} /* namespace */

/* ── 내부 API ─────────────────────────────────────────────────────────── */

void ggml_backend_register(ggml_backend_reg_t reg)
{
	get_reg().register_backend(reg);
}

void ggml_backend_device_register(ggml_backend_dev_t device)
{
	get_reg().register_device(device);
}

/* ── 백엔드 열거 ──────────────────────────────────────────────────────── */

size_t ggml_backend_reg_count(void)
{
	return get_reg().backends.size();
}

ggml_backend_reg_t ggml_backend_reg_get(size_t index)
{
	GGML_ASSERT(index < ggml_backend_reg_count());
	return get_reg().backends[index];
}

ggml_backend_reg_t ggml_backend_reg_by_name(const char *name)
{
	for (size_t i = 0; i < ggml_backend_reg_count(); i++) {
		ggml_backend_reg_t reg = ggml_backend_reg_get(i);
		if (striequals(ggml_backend_reg_name(reg), name)) {
			return reg;
		}
	}
	return nullptr;
}

/* ── 장치 열거 ────────────────────────────────────────────────────────── */

size_t ggml_backend_dev_count(void)
{
	return get_reg().devices.size();
}

ggml_backend_dev_t ggml_backend_dev_get(size_t index)
{
	GGML_ASSERT(index < ggml_backend_dev_count());
	return get_reg().devices[index];
}

ggml_backend_dev_t ggml_backend_dev_by_name(const char *name)
{
	for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
		ggml_backend_dev_t dev = ggml_backend_dev_get(i);
		if (striequals(ggml_backend_dev_name(dev), name)) {
			return dev;
		}
	}
	return nullptr;
}

ggml_backend_dev_t ggml_backend_dev_by_type(enum ggml_backend_dev_type type)
{
	for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
		ggml_backend_dev_t dev = ggml_backend_dev_get(i);
		if (ggml_backend_dev_type(dev) == type) {
			return dev;
		}
	}
	return nullptr;
}

/* ── 편의 함수 ────────────────────────────────────────────────────────── */

ggml_backend_t ggml_backend_init_by_name(const char *name, const char *params)
{
	ggml_backend_dev_t dev = ggml_backend_dev_by_name(name);
	return dev ? ggml_backend_dev_init(dev, params) : nullptr;
}

ggml_backend_t ggml_backend_init_by_type(enum ggml_backend_dev_type type, const char *params)
{
	ggml_backend_dev_t dev = ggml_backend_dev_by_type(type);
	return dev ? ggml_backend_dev_init(dev, params) : nullptr;
}

ggml_backend_t ggml_backend_init_best(void)
{
	/* GPU·IGPU 는 없다. 상류의 폴백 순서를 그대로 따른다. */
	ggml_backend_dev_t dev = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
	return dev ? ggml_backend_dev_init(dev, nullptr) : nullptr;
}

/* ── 동적 적재 — 전부 멈춘다 ──────────────────────────────────────────── */

ggml_backend_reg_t ggml_backend_load(const char *path)
{
	GGML_UNUSED(path);
	no_dynamic_loading("ggml_backend_load()");
}

void ggml_backend_unload(ggml_backend_reg_t reg)
{
	GGML_UNUSED(reg);
	no_dynamic_loading("ggml_backend_unload()");
}

void ggml_backend_load_all(void)
{
	/*
	 * ⚠ 이것만은 **멈추지 않는다.** 상류 예제·도구가 "있으면 좋은" 백엔드를
	 *   긁어모으려고 시작할 때 무조건 부르는 함수다. 정적 백엔드만 있는
	 *   우리에게는 할 일이 없는 것이 **정상**이다. 멈추면 쓸 수 없다.
	 *   대신 레지스트리를 만들어 CPU 가 등록되게 한다.
	 */
	(void)get_reg();
}

void ggml_backend_load_all_from_path(const char *dir_path)
{
	GGML_UNUSED(dir_path);
	/*
	 * 이쪽은 **경로를 명시**한 호출이다 — 호출자가 그 디렉터리의 백엔드를
	 * 기대한다. 조용히 넘기면 "왜 안 올라왔지" 가 된다. 멈춘다.
	 */
	no_dynamic_loading("ggml_backend_load_all_from_path()");
}
