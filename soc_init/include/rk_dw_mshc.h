/* rk_dw_mshc — 셸에서 DMA 를 켜고 끄고 세어 본다 (drivers/sdhc/rk_dw_mshc.c). SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
extern volatile bool rk_dw_mshc_dma_rd, rk_dw_mshc_dma_wr;
/* 끝난 전송 수(DMA·PIO) · 정렬이 안 돼 PIO 로 간 수 · 링이 비어 IDMAC 가 멈췄다 이어 간 수 */
extern volatile uint32_t rk_dw_mshc_n_dma, rk_dw_mshc_n_pio, rk_dw_mshc_n_unaligned, rk_dw_mshc_n_du;
