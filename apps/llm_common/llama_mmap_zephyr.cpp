/*
 * llama_mmap_zephyr.cpp — 상류 `src/llama-mmap.cpp` 의 **대체판**.
 *
 * 포팅 항목 8번. 근거는 docs/l6-decisions.md 결정 3 (제자리 적재).
 *
 * ── 왜 대체하나 ──────────────────────────────────────────────────────────
 *
 * 상류 파일은 이 툴체인에서 **컴파일은 된다.** 그런데
 * `_POSIX_MAPPED_FILES` 가 정의되지 않아(`-E -dM` 으로 확인) fallback 가지로
 * 간다:
 *
 *   llama-mmap.cpp:691  const bool llama_mmap::SUPPORTED = false;
 *   llama-mmap.cpp:664  throw std::runtime_error("mmap not supported");
 *
 * `SUPPORTED=false` 면 로더가 `use_mmap` 을 끄고
 * (`llama-model-loader.cpp:835-837`) 텐서마다 **새 버퍼에 fread** 한다
 * (`:1685` 이후). 모델이 507.85 MiB 인데 창(512 MiB) 여유가 4.15 MiB 다 —
 * **또 508 MiB** 를 쓸 자리가 없다.
 *
 * 그래서 매핑을 "블롭이 **이미 그 주소에 있다**" 로 바꾼다. U-Boot 가
 * `fatload`(또는 `ext4load`)로 DT `model_blob` 의 주소에 올려 두었고,
 * MMU 가 `zephyr,memory-region` + `zephyr,memory-attr` 로 그 창을 부팅 때
 * 플랫 매핑한다(`arch/arm64/core/mmu.c:907`, C 코드 0줄). 그러니
 * **매핑은 이미 끝나 있다.** `addr()` 이 그 주소를 돌려주면 로더가
 * `mapping->addr() + w.offs` 를 그대로 쓰고(`:1549`, `:1717`) **복사가 0** 이다.
 *
 * ── 왜 이 꼴인가 ────────────────────────────────────────────────────────
 *
 * `llama-mmap.h` 는 셋 다 완전한 pimpl 이고 `SUPPORTED` 가
 * `static const bool` 이다. 즉 상류가 **플랫폼 구현을 갈아 끼우라고 만든
 * 이음매**다. 헤더도 호출부도 한 줄 고치지 않는다.
 * `cmake/llama-sources.txt` 에서 상류 파일을 빼고 이 파일을 링크한다.
 *
 * ⚠ 주소·크기의 **정본은 DT** 다. `build.sh` 가 이미
 *   `apps/llm_common/model_blob.overlay` 의 `reg` 와 `sdcard/boot.cmd` 의
 *   적재 주소를 대조해 어긋나면 종료한다. 여기서 숫자를 적으면 **세 번째
 *   사본**이 되므로 DT 에서 읽는다.
 */
#include "llama-mmap.h"
#include "llama-impl.h"

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#define BLOB_NODE DT_NODELABEL(model_blob)
#if !DT_NODE_EXISTS(BLOB_NODE)
#error "DT 에 model_blob 노드가 없다 — apps/llm_common/model_blob.overlay 를 \
EXTRA_DTC_OVERLAY_FILE 로 넣어라."
#endif
#define BLOB_ADDR ((uintptr_t)DT_REG_ADDR(BLOB_NODE))
#define BLOB_SIZE ((size_t)DT_REG_SIZE(BLOB_NODE))

namespace {

[[noreturn]] void not_on_zephyr(const char *what)
{
	printk("llama_mmap_zephyr: %s — 이 포팅에는 없다.\n"
	       "  모델은 DT model_blob(0x%08lx)에 **이미 올라와 있다**. 읽기만 한다.\n"
	       "  (조용히 넘기면 어디서 틀렸는지 알 수 없어 여기서 멈춘다)\n",
	       what, (unsigned long)BLOB_ADDR);
	k_panic();
	CODE_UNREACHABLE;
}

} /* namespace */

/* ── llama_file — 블롭 위의 커서 ──────────────────────────────────────── */
/*
 * 상류는 `fopen`/`fread` 를 쓴다. 우리는 블롭 위를 **직접** 걷는다.
 *
 * ⚠ `fmemopen` 을 써서 상류 `llama_file` 을 그대로 두는 길도 있었다
 *   (picolibc 에 있다 — 확인했다). 쓰지 않는다: 어차피 이 파일 하나를
 *   대체하므로 커서를 직접 두는 쪽이 짧고, `fread` 한 번의 memcpy 를 없앤다.
 *   오프셋 정본은 `pos` 하나다.
 */
struct llama_file::impl {
	const uint8_t *base;
	size_t         len;
	size_t         pos = 0;

	impl(const char *fname, const char *mode, bool use_direct_io)
		: base((const uint8_t *)BLOB_ADDR), len(BLOB_SIZE)
	{
		GGML_UNUSED(use_direct_io);
		/*
		 * 파일 이름은 **무시한다** — 올릴 수 있는 블롭이 하나뿐이다.
		 * 그래도 쓰기 모드는 막는다(쓸 곳이 없다).
		 */
		if (mode != nullptr && std::strchr(mode, 'r') == nullptr) {
			printk("llama_file: 쓰기 모드 '%s' — 블롭은 읽기 전용이다\n", mode);
			k_panic();
		}
		LLAMA_LOG_INFO("llama_file(zephyr): '%s' -> 블롭 0x%08lx + %zu bytes\n",
			       fname ? fname : "(null)", (unsigned long)BLOB_ADDR, len);
	}

	explicit impl(FILE *file)
		: base((const uint8_t *)BLOB_ADDR), len(BLOB_SIZE)
	{
		GGML_UNUSED(file);
	}

	size_t tell() const { return pos; }

	void seek(size_t offset, int whence) const
	{
		size_t np;
		switch (whence) {
		case SEEK_SET: np = offset; break;
		case SEEK_CUR: np = pos + offset; break;
		case SEEK_END: np = len + offset; break;
		default:
			throw std::runtime_error("llama_file::seek: 모르는 whence");
		}
		if (np > len) {
			throw std::runtime_error("llama_file::seek: 블롭 밖");
		}
		const_cast<impl *>(this)->pos = np;
	}

	void read_raw(void *ptr, size_t n)
	{
		if (n == 0) {
			return;
		}
		if (pos + n > len) {
			throw std::runtime_error("llama_file::read_raw: 블롭 밖 "
						 "(블롭이 모델보다 작다 — 적재가 잘렸나?)");
		}
		std::memcpy(ptr, base + pos, n);
		pos += n;
	}

	void read_raw_unsafe(void *ptr, size_t n) { read_raw(ptr, n); }

	uint32_t read_u32()
	{
		uint32_t v;
		read_raw(&v, sizeof v);
		return v;
	}

	/* 정렬 읽기는 직접 I/O 용이다. 메모리에는 뜻이 없다. */
	size_t read_alignment() const { return 1; }
	bool   has_direct_io() const { return false; }
	void   read_aligned_chunk(void *dest, size_t n) { read_raw(dest, n); }

	void write_raw(const void *, size_t) const { not_on_zephyr("llama_file::write_raw()"); }
	void write_u32(uint32_t) const { not_on_zephyr("llama_file::write_u32()"); }

	size_t size;	/* 헤더가 pimpl->size 를 보지 않으므로 쓰이지 않는다 */
};

llama_file::llama_file(const char *fname, const char *mode, const bool use_direct_io)
	: pimpl(std::make_unique<impl>(fname, mode, use_direct_io)) {}
llama_file::llama_file(FILE *file) : pimpl(std::make_unique<impl>(file)) {}
llama_file::~llama_file() = default;

size_t   llama_file::tell() const { return pimpl->tell(); }
size_t   llama_file::size() const { return pimpl->len; }
size_t   llama_file::read_alignment() const { return pimpl->read_alignment(); }
bool     llama_file::has_direct_io() const { return pimpl->has_direct_io(); }
void     llama_file::seek(size_t o, int w) const { pimpl->seek(o, w); }
void     llama_file::read_raw(void *p, size_t n) { pimpl->read_raw(p, n); }
void     llama_file::read_raw_unsafe(void *p, size_t n) { pimpl->read_raw_unsafe(p, n); }
void     llama_file::read_aligned_chunk(void *d, size_t n) { pimpl->read_aligned_chunk(d, n); }
uint32_t llama_file::read_u32() { return pimpl->read_u32(); }
void     llama_file::write_raw(const void *p, size_t n) const { pimpl->write_raw(p, n); }
void     llama_file::write_u32(uint32_t v) const { pimpl->write_u32(v); }

/*
 * `file_id()` 는 `fileno` 다. 로더는 직접 I/O 와 prefetch 에만 쓴다.
 * 둘 다 없으므로 멈춘다 — -1 을 돌려주면 그 뒤가 조용히 틀린다.
 */
int llama_file::file_id() const { not_on_zephyr("llama_file::file_id()"); }

/* ── llama_mmap — 이미 매핑돼 있다 ────────────────────────────────────── */

struct llama_mmap::impl {
	void  *addr;
	size_t size;

	impl(struct llama_file *file, size_t prefetch, bool numa,
	     const llama_mmap::ranges &lazy_ranges)
	{
		GGML_UNUSED(prefetch);	/* 이미 RAM 에 있다 */
		GGML_UNUSED(numa);	/* 코어 하나, 노드 하나 */
		GGML_UNUSED(lazy_ranges);	/* 전부 올라와 있다 */

		addr = (void *)BLOB_ADDR;
		size = file ? file->size() : BLOB_SIZE;

		if (size > BLOB_SIZE) {
			printk("llama_mmap: 파일이 %zu bytes 인데 창이 %zu bytes 다 — "
			       "모델이 창보다 크다\n", size, BLOB_SIZE);
			k_panic();
		}
		LLAMA_LOG_INFO("llama_mmap(zephyr): 제자리 적재 0x%08lx + %zu bytes "
			       "(복사 없음)\n", (unsigned long)BLOB_ADDR, size);
	}

	/*
	 * 상류는 안 쓰는 조각을 munmap 해 RSS 를 줄인다. 우리 창은 물리 RAM
	 * 그대로라 돌려줄 곳이 없다. **무동작이 올바르다** — 멈추지 않는다.
	 */
	void unmap_fragment(size_t first, size_t last)
	{
		GGML_UNUSED(first);
		GGML_UNUSED(last);
	}
};

llama_mmap::llama_mmap(struct llama_file *file, size_t prefetch, bool numa,
		       const ranges &lazy_ranges)
	: pimpl(std::make_unique<impl>(file, prefetch, numa, lazy_ranges)) {}
llama_mmap::~llama_mmap() = default;

size_t llama_mmap::size() const { return pimpl->size; }
void  *llama_mmap::addr() const { return pimpl->addr; }
void   llama_mmap::unmap_fragment(size_t f, size_t l) { pimpl->unmap_fragment(f, l); }

/*
 * ⚠ **이 한 줄이 결정 3 의 전부다.** false 면 로더가
 *   llama-model-loader.cpp:835-837 에서 use_mmap 을 끄고 508 MiB 를 또 복사한다.
 */
const bool llama_mmap::SUPPORTED = true;

/* ── llama_mlock — 고정할 것이 없다 ──────────────────────────────────── */
/*
 * mlock 은 페이지가 스왑으로 나가는 것을 막는 것이다. 스왑도 페이징도 없다.
 * `SUPPORTED=false` 가 **정확한 사실**이고, 로더는 그러면 쓰지 않는다.
 */
struct llama_mlock::impl {
	void init(void *ptr) { GGML_UNUSED(ptr); }
	void grow_to(size_t n) { GGML_UNUSED(n); }
};

llama_mlock::llama_mlock() : pimpl(std::make_unique<impl>()) {}
llama_mlock::~llama_mlock() = default;
void llama_mlock::init(void *ptr) { pimpl->init(ptr); }
void llama_mlock::grow_to(size_t n) { pimpl->grow_to(n); }
const bool llama_mlock::SUPPORTED = false;

/* ── 나머지 ───────────────────────────────────────────────────────────── */

/* 미리 끌어올 캐시 계층이 없다. 무동작이 올바르다. */
void llama_prefetch(llama_memory_ranges mr) { GGML_UNUSED(mr); }

/* 경로가 없지만 상류가 버퍼 크기로 쓴다. 넉넉한 값을 준다. */
size_t llama_path_max() { return 256; }
