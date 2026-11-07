/*
 * llama_split_zephyr.cpp — `llama_split_path` · `llama_split_prefix` 두 개.
 *
 * ── 왜 이 파일이 있나 ───────────────────────────────────────────────────
 *
 * `llama-model-loader.cpp:90,102` 가 이 둘을 부른다. 정의는 **`src/llama.cpp`**
 * 에 있다(`:544`, `:569`). 그런데 그 파일을 넣으면 `llama_model_load` ·
 * `llama_context` 를 통해 `llama-model.cpp` → `src/models/*.cpp` **175개
 * 46,741줄**이 줄줄이 따라온다. L6a 는 메타데이터와 어휘만 가리는 칸이다.
 * 그리고 `src/llama.cpp` 는 cmake 가 만드는 `llama-version.h` 를 요구한다.
 *
 * 두 함수는 **여러 조각으로 나뉜 모델**(`model-00001-of-00003.gguf`)의 경로를
 * 다루는 순수 문자열 함수다. 우리는 조각을 쓰지 않으므로 호출돼도
 * `split_count == 1` 경로만 지난다. 상류 구현을 **그대로** 옮겼다 — 동작이
 * 갈리지 않게 한다.
 *
 * ⚠ 나중에 `src/llama.cpp` 를 소스 목록에 넣으면 **중복 정의로 링크가
 *   깨진다.** 그때는 이 파일을 목록에서 빼면 된다. 조용히 어긋나는 쪽이
 *   아니라 **시끄럽게 멈추는 쪽**이라 괜찮다.
 *
 * ⚠ 상류 수정은 아니다 — 상류 파일을 고치지 않는다. 다만 상류가 다른 곳에
 *   정의한 심볼을 **대신 채우는** 것이므로, 포팅 항목 중 성격이 다르다.
 *   l6a-compile-matrix.md 에 그렇게 적는다.
 */
#include "llama.h"

#include <algorithm>
#include <cstdio>
#include <string>

int32_t llama_split_path(char *split_path, size_t maxlen, const char *path_prefix,
			 int32_t split_no, int32_t split_count)
{
	static const char *const SPLIT_PATH_FORMAT = "%s-%05d-of-%05d.gguf";

	const int written = snprintf(split_path, maxlen, SPLIT_PATH_FORMAT,
				     path_prefix, split_no + 1, split_count);

	if (written < 0 || (size_t)written >= maxlen) {
		return 0;
	}
	return (int32_t)written;
}

int32_t llama_split_prefix(char *split_prefix, size_t maxlen, const char *split_path,
			   int32_t split_no, int32_t split_count)
{
	const std::string str_split_path(split_path);

	char postfix[32];
	snprintf(postfix, sizeof(postfix), "-%05d-of-%05d.gguf", split_no + 1, split_count);

	const std::string str_postfix(postfix);
	if (str_split_path.size() <= str_postfix.size()) {
		return 0;
	}

	const size_t size_prefix = str_split_path.size() - str_postfix.size();

	if (str_split_path.compare(size_prefix, std::string::npos, str_postfix) == 0) {
		const size_t copy_len = std::min(size_prefix + 1, maxlen);
		snprintf(split_prefix, copy_len, "%s", split_path);
		return (int32_t)size_prefix;
	}
	return 0;
}
