# SPDX-License-Identifier: Apache-2.0
#
# ⚠ 모듈 수준에서 넣으므로 **이 보드로 빌드하는 모든 앱**에 적용된다.
#   앱마다 더하면 빠뜨리는 앱이 생기고, 그 앱만 조용히 느려진다.
zephyr_library_named(rk3566_soc_init)
zephyr_library_sources(smpen.S dw_uart_busy.c boot_slot.c)
zephyr_library_sources_ifdef(CONFIG_MORU_LED moru_led.c)
zephyr_library_sources_ifdef(CONFIG_RK3566_TSADC rk3566_tsadc.c)
# moru_led.h 는 모든 앱이 쓴다 (꺼져 있으면 빈 inline 이 된다)
zephyr_include_directories(include)
