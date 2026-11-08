/*
 * rk_dw_mshc — RK3566 SD 컨트롤러(Synopsys DesignWare MSHC, sdmmc0) Zephyr SDHC 드라이버.
 *
 * 출처 (줄 단위로 따랐다):
 *   U-Boot drivers/mmc/dw_mmc.c — dwmci_send_cmd_common · dwmci_data_transfer(fifo_mode) ·
 *     dwmci_update_div · dwmci_control_clken · dwmci_init · dwmci_init_fifo
 *   U-Boot include/dwmmc.h — 레지스터·비트 (DATA 는 0x200)
 *   U-Boot drivers/mmc/rockchip_dw_mmc.c — 카드 클럭은 CRU 로 맞추고 CLKDIV 는 0(통과)
 *   U-Boot clk_rk3568.c rk3568_sdmmc_set_clk · cru_rk3568.h — clksel_con[30] [10:8]
 *     0 24M · 1 400M · 2 300M · 3 100M · 4 50M · 5 750K. 400 kHz 요청에 750K, 25 MHz 에 24M,
 *     50 MHz 에 50M 을 고르는 것으로 보아 컨트롤러가 CRU 클럭을 2 로 나눈다(Linux dw_mmc-rockchip
 *     의 RK3288_CLKGEN_DIV 와 같은 뜻) — 실제 카드 클럭 = 선택값 / 2 로 계산한다.
 *   Linux rk356x-base.dtsi sdmmc0 · rk3566-radxa-zero-3.dtsi (4-bit · 3.3 V · cap-sd-highspeed)
 *
 * 범위: PIO(FIFO) · 3.3 V · 기본/고속(최대 50 MHz 카드 클럭) · 4-bit. DMA·UHS 없음.
 * 핀·전원·바이오스 클럭은 U-Boot 가 켜 둔 것을 이어받는다(카드를 읽고 뛰어오므로 켜져 있다).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT rockchip_rk3568_dw_mshc

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sdhc.h>
#include <zephyr/sd/sd_spec.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(rk_dw_mshc, CONFIG_SDHC_LOG_LEVEL);

/* include/dwmmc.h */
#define CTRL      0x000
#define PWREN     0x004
#define CLKDIV    0x008
#define CLKSRC    0x00c
#define CLKENA    0x010
#define TMOUT     0x014
#define CTYPE     0x018
#define BLKSIZ    0x01c
#define BYTCNT    0x020
#define INTMASK   0x024
#define CMDARG    0x028
#define CMD       0x02c
#define RESP0     0x030
#define RESP1     0x034
#define RESP2     0x038
#define RESP3     0x03c
#define RINTSTS   0x044
#define STATUS    0x048
#define FIFOTH    0x04c
#define CDETECT   0x050
#define UHS_REG   0x074
#define BMOD      0x080
#define DATA      0x200

#define INT_RE    BIT(1)
#define INT_CDONE BIT(2)
#define INT_DTO   BIT(3)
#define INT_TXDR  BIT(4)
#define INT_RXDR  BIT(5)
#define INT_RCRC  BIT(6)
#define INT_DCRC  BIT(7)
#define INT_RTO   BIT(8)
#define INT_DRTO  BIT(9)
#define INT_HTO   BIT(10)
#define INT_FRUN  BIT(11)
#define INT_HLE   BIT(12)
#define INT_SBE   BIT(13)
#define INT_ACD   BIT(14)	/* 자동 CMD12 완료 */
#define INT_EBE   BIT(15)
#define DATA_ERR  (INT_EBE | INT_SBE | INT_HLE | INT_FRUN | INT_DCRC)
#define DATA_TOUT (INT_HTO | INT_DRTO)

#define CTRL_RESET      BIT(0)
#define CTRL_FIFO_RESET BIT(1)
#define CTRL_DMA_RESET  BIT(2)
#define CTRL_DMA_EN     BIT(5)
#define CTRL_IDMAC_EN   BIT(25)
#define RESET_ALL       (CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET)

#define CMD_RESP_EXP     BIT(6)
#define CMD_RESP_LONG    BIT(7)
#define CMD_CHECK_CRC    BIT(8)
#define CMD_DATA_EXP     BIT(9)
#define CMD_RW           BIT(10)
#define CMD_SEND_STOP    BIT(12)	/* 데이터 끝에 CMD12 를 자동으로 (send_auto_stop) */
#define CMD_PRV_DAT_WAIT BIT(13)
#define CMD_ABORT_STOP   BIT(14)
#define CMD_SEND_INIT    BIT(15)	/* 첫 명령 앞 80 클럭 (Linux SDMMC_CMD_INIT) */
#define CMD_UPD_CLK      BIT(21)
#define CMD_USE_HOLD_REG BIT(29)
#define CMD_START        BIT(31)

#define CLKEN_ENABLE  BIT(0)
#define CLKEN_LOW_PWR BIT(16)

#define ST_FIFO_EMPTY BIT(2)
#define ST_FIFO_FULL  BIT(3)
#define ST_BUSY       BIT(9)
#define ST_FIFO_CNT(s) (((s) >> 17) & 0x1fff)

/* CRU clksel_con[30] — CRU 0xfdd20000 + 0x100 + 4*30 */
#define CRU_PHYS        0xfdd20000UL
#define CRU_CLKSEL30    (0x100 + 4 * 30)
#define SDMMC0_SEL_SHIFT 8
#define SDMMC0_SEL_MASK  (7u << SDMMC0_SEL_SHIFT)

struct dwm_cfg {
	DEVICE_MMIO_ROM;
	uint32_t fifo_depth;
};

struct dwm_data {
	DEVICE_MMIO_RAM;
	mm_reg_t cru;
	struct k_mutex lock;
	uint32_t clock_hz;
	bool sent_init;
};

static inline uint32_t rd(const struct device *dev, uint32_t o)
{
	return sys_read32(DEVICE_MMIO_GET(dev) + o);
}

static inline void wr(const struct device *dev, uint32_t o, uint32_t v)
{
	sys_write32(v, DEVICE_MMIO_GET(dev) + o);
}

static bool wait_reset(const struct device *dev, uint32_t bits)
{
	wr(dev, CTRL, rd(dev, CTRL) | bits);
	for (int i = 0; i < 100000; i++) {
		if (!(rd(dev, CTRL) & bits)) { return true; }
		k_busy_wait(1);
	}
	return false;
}

/* dwmci_control_clken + "Inform CIU" */
static int clk_cmd(const struct device *dev, uint32_t clkena)
{
	wr(dev, CLKENA, clkena);
	wr(dev, CMD, CMD_START | CMD_PRV_DAT_WAIT | CMD_UPD_CLK);
	for (int i = 0; i < 100000; i++) {
		if (!(rd(dev, CMD) & CMD_START)) { return 0; }
		k_busy_wait(1);
	}
	LOG_ERR("클럭 갱신 명령이 끝나지 않는다");
	return -ETIMEDOUT;
}

/* 카드 클럭: CRU 선택 → 컨트롤러 /2 → CLKDIV 0(통과). 반환 = 실제 카드 클럭 */
static uint32_t set_card_clock(const struct device *dev, uint32_t want)
{
	struct dwm_data *d = dev->data;
	uint32_t sel, src;

	if (want <= 400000) {        sel = 5; src = 750000; }
	else if (want <= 12000000) { sel = 0; src = 24000000; }
	else if (want <= 25000000) { sel = 4; src = 50000000; }
	else {                       sel = 3; src = 100000000; }	/* 50 MHz (고속) */

	(void)clk_cmd(dev, 0);		/* 끈 채로 바꾼다 — dwmci_update_div */
	sys_write32((SDMMC0_SEL_MASK << 16) | (sel << SDMMC0_SEL_SHIFT), d->cru + CRU_CLKSEL30);
	wr(dev, CLKDIV, 0);
	wr(dev, CLKSRC, 0);
	(void)clk_cmd(dev, CLKEN_ENABLE | CLKEN_LOW_PWR);
	d->clock_hz = src / 2;
	return d->clock_hz;
}

static int dwm_reset(const struct device *dev)
{
	return wait_reset(dev, RESET_ALL) ? 0 : -ETIMEDOUT;
}

static int dwm_get_card_present(const struct device *dev)
{
	return (rd(dev, CDETECT) & BIT(0)) ? 0 : 1;	/* 0 = 꽂힘 */
}

static int dwm_card_busy(const struct device *dev)
{
	return (rd(dev, STATUS) & ST_BUSY) ? 1 : 0;
}

static int dwm_get_host_props(const struct device *dev, struct sdhc_host_props *p)
{
	ARG_UNUSED(dev);
	memset(p, 0, sizeof(*p));
	p->f_min = 375000;
	p->f_max = 50000000;
	p->power_delay = 10;
	p->max_current_330 = 800;
	p->bus_4_bit_support = true;
	p->host_caps.vol_330_support = 1;
	p->host_caps.high_spd_support = 1;
	return 0;
}

static int dwm_set_io(const struct device *dev, struct sdhc_io *ios)
{
	struct dwm_data *d = dev->data;

	if (ios->signal_voltage == SD_VOL_1_8_V) {
		return -ENOTSUP;	/* UHS 전압 전환은 하지 않는다 */
	}
	k_mutex_lock(&d->lock, K_FOREVER);
	if (ios->power_mode == SDHC_POWER_ON) {
		wr(dev, PWREN, 1);
	}
	if (ios->clock) {
		const uint32_t got = set_card_clock(dev, ios->clock);
		LOG_DBG("카드 클럭 요청 %u → %u Hz", ios->clock, got);
	}
	wr(dev, CTYPE, ios->bus_width == SDHC_BUS_WIDTH4BIT ? BIT(0) :
		       (ios->bus_width == SDHC_BUS_WIDTH8BIT ? BIT(16) : 0));
	wr(dev, UHS_REG, rd(dev, UHS_REG) & ~BIT(16));	/* DDR 끔 */
	k_mutex_unlock(&d->lock);
	return 0;
}

/* dwmci_data_transfer — FIFO 모드 */
static int xfer_data(const struct device *dev, struct sdhc_data *data, bool write)
{
	const struct dwm_cfg *c = dev->config;
	uint32_t *buf = data->data;
	uint32_t words = data->block_size * data->blocks / 4;
	const int64_t end = k_uptime_get() + (data->timeout_ms > 0 ? data->timeout_ms : 2000) +
			    (int64_t)data->blocks * 10;
	uint32_t m;
	bool dto = false;	/* ⚠ RXDR 과 함께 지운 DTO 를 잃지 않게 기억한다 (U-Boot 는 지우기 전 mask 로 판정) */

	for (;;) {
		m = rd(dev, RINTSTS);
		dto |= (m & INT_DTO) != 0;
		if (m & (DATA_ERR | DATA_TOUT)) {
			wr(dev, RINTSTS, m);
			LOG_ERR("데이터 오류 RINTSTS 0x%08x", m);
			return -EIO;
		}
		if (words && !write && (m & (INT_RXDR | INT_DTO))) {
			wr(dev, RINTSTS, m & (INT_RXDR | INT_DTO));
			uint32_t n = ST_FIFO_CNT(rd(dev, STATUS));
			n = MIN(n, words);
			for (uint32_t i = 0; i < n; i++) { *buf++ = rd(dev, DATA); }
			words -= n;
			if (n) { continue; }
		}
		if (words && write && (m & INT_TXDR)) {
			uint32_t n = c->fifo_depth - ST_FIFO_CNT(rd(dev, STATUS));
			n = MIN(n, words);
			for (uint32_t i = 0; i < n; i++) { wr(dev, DATA, *buf++); }
			words -= n;
			wr(dev, RINTSTS, INT_TXDR);
		}
		if (dto && words == 0) {
			/* ⚠ ACD(자동 CMD12 완료)는 남긴다 — 호출자가 기다린다 */
			wr(dev, RINTSTS, m & ~INT_ACD);
			data->bytes_xfered = data->block_size * data->blocks;
			return 0;
		}
		if (k_uptime_get() > end) {
			LOG_ERR("데이터 시간 초과 (남은 %u 워드, RINTSTS 0x%08x)", words, m);
			return -ETIMEDOUT;
		}
	}
}

static int dwm_request(const struct device *dev, struct sdhc_command *cmd, struct sdhc_data *data)
{
	struct dwm_data *d = dev->data;
	const uint32_t rt = cmd->response_type & SDHC_NATIVE_RESPONSE_MASK;
	const bool write = data && (cmd->opcode == SD_WRITE_SINGLE_BLOCK ||
				    cmd->opcode == SD_WRITE_MULTIPLE_BLOCK);
	uint32_t flags = 0, m = 0;
	int ret = 0;

	k_mutex_lock(&d->lock, K_FOREVER);

	/* dwmci_wait_while_busy (500 ms) */
	for (int i = 0; i < 500000 && (rd(dev, STATUS) & ST_BUSY); i++) { k_busy_wait(1); }
	wr(dev, RINTSTS, 0xffffffff);

	/* ⚠ Zephyr SD 스택은 CMD18·CMD25 뒤 CMD12 를 호스트가 알아서 보내길 기대한다
	 *   (subsys/sd/sd_ops.c card_read 주석) — 자동 정지를 켠다 */
	const bool multi = data && (cmd->opcode == SD_READ_MULTIPLE_BLOCK ||
				    cmd->opcode == SD_WRITE_MULTIPLE_BLOCK);
	if (data) {
		wr(dev, BLKSIZ, data->block_size);
		wr(dev, BYTCNT, data->block_size * data->blocks);
		(void)wait_reset(dev, CTRL_FIFO_RESET);
		flags |= CMD_DATA_EXP | (write ? CMD_RW : 0) | (multi ? CMD_SEND_STOP : 0);
	}
	wr(dev, CMDARG, cmd->arg);

	flags |= (cmd->opcode == SD_STOP_TRANSMISSION) ? CMD_ABORT_STOP : CMD_PRV_DAT_WAIT;
	if (!d->sent_init || cmd->opcode == SD_GO_IDLE_STATE) {
		flags |= CMD_SEND_INIT;
		d->sent_init = true;
	}
	switch (rt) {
	case SD_RSP_TYPE_NONE:
		break;
	case SD_RSP_TYPE_R2:
		flags |= CMD_RESP_EXP | CMD_RESP_LONG | CMD_CHECK_CRC;
		break;
	case SD_RSP_TYPE_R3:
	case SD_RSP_TYPE_R4:
		flags |= CMD_RESP_EXP;	/* CRC 없음 */
		break;
	default:	/* R1 R1b R5 R5b R6 R7 */
		flags |= CMD_RESP_EXP | CMD_CHECK_CRC;
		break;
	}
	flags |= cmd->opcode | CMD_START | CMD_USE_HOLD_REG;
	wr(dev, CMD, flags);

	const int64_t end = k_uptime_get() + (cmd->timeout_ms > 0 ? cmd->timeout_ms : 1000);
	for (;;) {
		m = rd(dev, RINTSTS);
		if (m & INT_CDONE) { break; }
		if (k_uptime_get() > end) { ret = -ETIMEDOUT; goto out; }
	}
	if (!data) { wr(dev, RINTSTS, m); }
	if (m & INT_RTO) { ret = -ETIMEDOUT; goto out; }	/* CMD8·CMD55 등은 정상적으로 날 수 있다 */
	if (m & INT_RE) { ret = -EIO; goto out; }
	if ((flags & CMD_CHECK_CRC) && (m & INT_RCRC)) { ret = -EIO; goto out; }

	if (flags & CMD_RESP_EXP) {
		if (flags & CMD_RESP_LONG) {
			/* Zephyr 는 response[3] 이 최상위 32 비트(CSD[127:96]) — sd_ops.c sdmmc_decode_csd */
			cmd->response[0] = rd(dev, RESP0);
			cmd->response[1] = rd(dev, RESP1);
			cmd->response[2] = rd(dev, RESP2);
			cmd->response[3] = rd(dev, RESP3);
		} else {
			cmd->response[0] = rd(dev, RESP0);
		}
	}
	if (data) {
		ret = xfer_data(dev, data, write);
		if (!ret && multi) {	/* 자동 CMD12 가 끝날 때까지 */
			const int64_t e2 = k_uptime_get() + 1000;
			while (!(rd(dev, RINTSTS) & INT_ACD)) {
				if (k_uptime_get() > e2) { LOG_ERR("자동 CMD12 완료가 오지 않는다"); ret = -ETIMEDOUT; break; }
			}
			wr(dev, RINTSTS, INT_ACD);
		}
	}
	if (rt == SD_RSP_TYPE_R1b || rt == SD_RSP_TYPE_R5b) {
		for (int i = 0; i < 500000 && (rd(dev, STATUS) & ST_BUSY); i++) { k_busy_wait(1); }
	}
out:
	if (ret && !(cmd->opcode == 8 || cmd->opcode == 55 || cmd->opcode == 5 || cmd->opcode == 52)) {
		/* CMD8·CMD55·SDIO 의 응답 없음은 카드 종류 판별 중 정상이다 */
		LOG_ERR("CMD%u 실패 %d · RINTSTS 0x%08x · STATUS 0x%08x · flags 0x%08x", cmd->opcode, ret,
			m, rd(dev, STATUS), flags);
	}
	if (ret) {
		wr(dev, RINTSTS, 0xffffffff);
		if (data) { (void)wait_reset(dev, CTRL_FIFO_RESET); }
	}
	k_mutex_unlock(&d->lock);
	return ret;
}

static int dwm_init(const struct device *dev)
{
	const struct dwm_cfg *c = dev->config;
	struct dwm_data *d = dev->data;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);
	device_map(&d->cru, CRU_PHYS, 0x1000, K_MEM_CACHE_NONE);
	k_mutex_init(&d->lock);

	/* dwmci_init */
	wr(dev, PWREN, 1);
	if (!wait_reset(dev, RESET_ALL)) {
		LOG_ERR("리셋이 끝나지 않는다");
		return -EIO;
	}
	wr(dev, RINTSTS, 0xffffffff);
	wr(dev, INTMASK, 0);		/* 폴링만 */
	wr(dev, TMOUT, 0xffffffff);
	wr(dev, BMOD, 1);		/* IDMAC 리셋 · 쓰지 않는다 */
	wr(dev, CTRL, rd(dev, CTRL) & ~(CTRL_DMA_EN | CTRL_IDMAC_EN));
	/* dwmci_init_fifo */
	const uint32_t thr = c->fifo_depth / 2;
	wr(dev, FIFOTH, (0x2u << 28) | ((thr - 1) << 16) | thr);
	set_card_clock(dev, 400000);
	LOG_INF("sdmmc0 준비 · 카드 %s · 클럭 %u Hz", dwm_get_card_present(dev) ? "있음" : "없음",
		d->clock_hz);
	return 0;
}

static DEVICE_API(sdhc, dwm_api) = {
	.reset = dwm_reset,
	.request = dwm_request,
	.set_io = dwm_set_io,
	.get_card_present = dwm_get_card_present,
	.card_busy = dwm_card_busy,
	.get_host_props = dwm_get_host_props,
};

#define DWM_INIT(n)								\
	static const struct dwm_cfg dwm_cfg_##n = {				\
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)),				\
		.fifo_depth = DT_INST_PROP(n, fifo_depth),			\
	};									\
	static struct dwm_data dwm_data_##n;					\
	DEVICE_DT_INST_DEFINE(n, dwm_init, NULL, &dwm_data_##n, &dwm_cfg_##n,	\
			      POST_KERNEL, CONFIG_SDHC_INIT_PRIORITY, &dwm_api);

DT_INST_FOREACH_STATUS_OKAY(DWM_INIT)
