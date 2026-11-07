/*
 * zephyr_future.h — `std::future` / `std::async` 의 **직렬 대체판**.
 *
 * ── 왜 필요한가 ──────────────────────────────────────────────────────────
 *
 * 이 툴체인의 libstdc++ 는 `_GLIBCXX_HAS_GTHREADS` 없이 빌드됐다. 그래서
 * `<future>` 의 내용이 거의 전부 가드 안에 있다 (future:216 `#if defined(…)`).
 * 가드 **밖**에 남는 것은
 *     future_errc · future_error · launch · future_status ·
 *     `class future;` · `class promise;` · `class packaged_task;` ← **전방 선언**
 *     `__async_result_of` 별칭 (future:204)
 *     `async()` 두 과형의 **선언** (future:208, :212)
 * 이고, **정의가 전부 없다.**
 *
 * ⚠ `async` 는 **선언이 이미 있다.** 그래서 같은 일을 하는 과형을 새로 더하면
 *   호출이 **모호해진다** ("call of overloaded 'async(…)' is ambiguous" — 실제로
 *   겪었다). 선언된 그 서명을 **그대로 정의**해야 한다 — 반환형까지
 *   `future<__async_result_of<_Fn, _Args...>>` 로 맞춘다.
 *
 * `llama-model-loader.cpp` 는 그것을 쓴다:
 *     :1579  std::vector<std::future<std::pair<ggml_tensor *, bool>>>
 *     :1730  validation_result.emplace_back(std::async(std::launch::async, …))
 *     :1858  for (auto & future : validation_result) future.get()
 *
 * `:1730` 은 `if (check_tensors)` 안이지만 **런타임 분기**다 — `#if` 가 아니라
 * 늘 컴파일된다. 그래서 `check_tensors=false` 로 둬도 컴파일이 깨진다
 * ("invalid use of incomplete type 'class std::future<int>'" 를 재현했다).
 *
 * ── 왜 이 방식인가 ──────────────────────────────────────────────────────
 *
 * 고른 길: libstdc++ 가 **비워 둔 자리**를 채운다. 가드가 닫혀 있어 충돌할
 * 정의가 애초에 없다. `ggml_zephyr_compat.h` 가 없는 선언을 채우는 것과 같은
 * 수단이고, **상류 수정 0건**을 지킨다.
 *
 * 버린 길:
 *   - 상류의 `check_tensors` 경로를 잘라내기 → `git pull` 마다 충돌한다.
 *   - `_GLIBCXX_HAS_GTHREADS` 를 켜고 gthread 를 Zephyr `k_thread` 로 구현 →
 *     훨씬 크고, 공유 자료 구조에 진짜 병행성을 들인다. 지금은 필요 없다.
 *
 * ⚠ `namespace std` 에 더하는 것은 표준상 미정의 동작이다. 자유 구현
 *   (freestanding/embedded)에서 빠진 부분을 메우는 **의도된 예외**로 쓴다.
 *   libstdc++ 가 이 자리를 채우는 날(가드가 열리는 날) 이 파일은 **중복
 *   정의로 컴파일이 깨진다** — 조용히 어긋나지 않는다. 아래 BUILD 검사가
 *   그 날을 먼저 알려 준다.
 *
 * ── 의미 ────────────────────────────────────────────────────────────────
 *
 * `async(launch, f, args…)` 는 **그 자리에서 f 를 실행**하고 결과를 담아
 * 돌려준다. `launch::async`(즉시) 도 `launch::deferred`(미룸) 도 똑같이
 * 즉시 실행한다 — 코어 하나에 스레드가 없으므로 `deferred` 보다 즉시 실행이
 * **더 정확하다**. `get()` 은 담아 둔 값을 옮겨 준다.
 *
 * 예외는 잡아 두고 `get()` 에서 다시 던진다 — 상류가 `std::runtime_error` 로
 * 올리는 경로(`:1861` 부근)가 그대로 동작한다.
 */
#ifndef ZEPHYR_FUTURE_H
#define ZEPHYR_FUTURE_H

#ifdef __cplusplus

#include <future>	/* launch · future_status · future_error · future 선언 */

#if defined(_GLIBCXX_HAS_GTHREADS)
/*
 * 이 툴체인이 진짜 future 를 갖게 됐다. 그러면 이 파일은 할 일이 없고,
 * 아래 정의는 **중복**이 된다. 지우는 것이 맞다.
 */
#error "libstdc++ 가 이제 <future> 를 제공한다 — zephyr_future.h 를 삭제하고 \
빌드에서 빼라 (cmake/llm_ggml.cmake 의 -include 목록)."
#endif

#include <exception>
#include <memory>
#include <functional>	/* std::__invoke */
#include <type_traits>
#include <utility>

namespace std {

template <typename _Res>
class future {
public:
	future() = default;
	future(future &&) = default;
	future &operator=(future &&) = default;
	future(const future &) = delete;
	future &operator=(const future &) = delete;

	/* async() 가 쓰는 생성자 — 값 또는 예외를 담는다 */
	explicit future(_Res &&__v)
		: _M_val(new _Res(std::move(__v))) {}
	explicit future(exception_ptr __e)
		: _M_exc(__e) {}

	_Res get()
	{
		if (_M_exc) {
			exception_ptr __e = _M_exc;
			_M_exc = nullptr;
			rethrow_exception(__e);
		}
		/* 상류와 같게 — get() 은 한 번만 유효하다 */
		unique_ptr<_Res> __v = std::move(_M_val);
		return std::move(*__v);
	}

	bool valid() const noexcept { return _M_val != nullptr || _M_exc != nullptr; }
	void wait() const noexcept {}

private:
	unique_ptr<_Res> _M_val;
	exception_ptr    _M_exc = nullptr;
};

/* void 특수화 — 상류가 쓰지는 않지만 비우면 void 작업에서 조용히 깨진다 */
template <>
class future<void> {
public:
	future() = default;
	future(future &&) = default;
	future &operator=(future &&) = default;
	future(const future &) = delete;

	explicit future(int /*done*/) : _M_ok(true) {}
	explicit future(exception_ptr __e) : _M_exc(__e) {}

	void get()
	{
		if (_M_exc) {
			exception_ptr __e = _M_exc;
			_M_exc = nullptr;
			rethrow_exception(__e);
		}
		_M_ok = false;
	}
	bool valid() const noexcept { return _M_ok || _M_exc != nullptr; }
	void wait() const noexcept {}

private:
	bool          _M_ok = false;
	exception_ptr _M_exc = nullptr;
};

/*
 * 그 자리에서 실행한다. launch 값은 무시한다 — 코어 하나에 스레드가 없으므로
 * async 든 deferred 든 결과가 같다.
 *
 * ⚠ 서명은 future:208 · :212 의 **선언과 글자 그대로 같아야** 한다.
 *   `__async_result_of` 를 쓰지 않고 `decltype(__f(__a...))` 로 쓰면 다른
 *   템플릿이 되어 과형이 둘이 되고 호출이 모호해진다.
 */
template <typename _Fn, typename... _Args>
future<__async_result_of<_Fn, _Args...>>
async(launch /*__policy — 무시한다*/, _Fn &&__fn, _Args &&...__args)
{
	using _Res = __async_result_of<_Fn, _Args...>;
	try {
		if constexpr (is_void<_Res>::value) {
			std::__invoke(std::forward<_Fn>(__fn),
				      std::forward<_Args>(__args)...);
			return future<void>(1);
		} else {
			return future<_Res>(
				std::__invoke(std::forward<_Fn>(__fn),
					      std::forward<_Args>(__args)...));
		}
	} catch (...) {
		return future<_Res>(current_exception());
	}
}

template <typename _Fn, typename... _Args>
future<__async_result_of<_Fn, _Args...>>
async(_Fn &&__fn, _Args &&...__args)
{
	return async(launch::async, std::forward<_Fn>(__fn),
		     std::forward<_Args>(__args)...);
}

} /* namespace std */

#endif /* __cplusplus */
#endif /* ZEPHYR_FUTURE_H */
