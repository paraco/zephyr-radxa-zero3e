# SPDX-License-Identifier: Apache-2.0
add_subdirectory_ifdef(CONFIG_UDC_DRIVER usb/udc)
add_subdirectory_ifdef(CONFIG_HWINFO_RK3566_OTP hwinfo)
add_subdirectory_ifdef(CONFIG_SDHC_RK_DW_MSHC sdhc)
