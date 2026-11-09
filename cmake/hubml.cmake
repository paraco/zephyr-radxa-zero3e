# hubml — HUB AI 의 ML 허브 모듈 (수집 · 저장 · 학습 · 평가 · 원장 · 배포).
# 앱 CMakeLists 에서:  include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/hubml.cmake)
#                      hubml_add()
# 필요 Kconfig: FILE_SYSTEM · FAT_FILESYSTEM_ELM (쓰기) · CRC · HEAP_MEM_POOL_SIZE(scan 의 비트표 128 KiB)
set(HUBML_DIR "${CMAKE_CURRENT_LIST_DIR}/../subsys/hubml")
function(hubml_add)
	target_include_directories(app PRIVATE ${HUBML_DIR}/include)
	target_sources(app PRIVATE
		${HUBML_DIR}/src/mcol.c
		${HUBML_DIR}/src/ingest.c
		${HUBML_DIR}/src/xmb.c
		${HUBML_DIR}/src/train.c
		${HUBML_DIR}/src/sha256.c
		${HUBML_DIR}/src/ledger.c)
endfunction()
