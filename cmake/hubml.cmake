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
		${HUBML_DIR}/src/ledger.c
		${HUBML_DIR}/src/deploy.c
		${HUBML_DIR}/src/edge.c
		${HUBML_DIR}/src/hubml.c
		${HUBML_DIR}/src/ed25519.c
		${HUBML_DIR}/src/approve.c)
	if(CONFIG_SHELL)
		target_sources(app PRIVATE ${HUBML_DIR}/src/ml_shell.c)
	endif()
	# ml_shell.c 는 shadow 요약의 Pearson 을 Python 기준과 비트까지 맞추려고 (FMA 축약이 자릿수를 바꾼다)
# 허브(A55)·Edge(M7·M33)·Studio 가 점수를 비트까지 같게 내려면 FMA 축약을 끈다 (Studio 3자 대조 2026-11-09)
	set_source_files_properties(${HUBML_DIR}/src/xmb.c ${HUBML_DIR}/src/train.c ${HUBML_DIR}/src/ml_shell.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endfunction()
