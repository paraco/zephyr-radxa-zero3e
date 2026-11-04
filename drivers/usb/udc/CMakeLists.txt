# SPDX-License-Identifier: Apache-2.0

zephyr_library()

# udc_common.h 는 인트리 drivers/usb/udc/ 의 private 헤더다. 전역이 아니라
# 이 라이브러리 스코프로만 노출해서 다른 라이브러리를 오염시키지 않는다.
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/usb/udc)

zephyr_library_sources_ifdef(CONFIG_UDC_DWC3 udc_dwc3.c)
