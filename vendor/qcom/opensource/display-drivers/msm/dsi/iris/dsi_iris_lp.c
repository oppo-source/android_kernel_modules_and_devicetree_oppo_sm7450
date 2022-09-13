// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2019, The Linux Foundation. All rights reserved.
 * Copyright (C) 2017-2020, Pixelworks, Inc.
 *
 * These files contain modifications made by Pixelworks, Inc., in 2019-2020.
 */
#include <video/mipi_display.h>
#include <drm/drm_bridge.h>
#include <drm/drm_encoder.h>
#include "dsi_drm.h"
#include <sde_encoder.h>
#include <sde_encoder_phys.h>
#include <sde_trace.h>
#include "dsi_iris_api.h"
#include "dsi_iris_lightup.h"
#include "dsi_iris_lightup_ocp.h"
#include "dsi_iris_lp.h"
#include "dsi_iris_pq.h"
#include "dsi_iris_gpio.h"
#include "dsi_iris_timing_switch.h"
#include "dsi_iris_log.h"

#include "dsi_iris_reg.h"
#include "dsi_iris_i3c.h"

static int _debug_lp_opt;

/* abyp debug option
 * bit[0]: 0 -- light up with PT, 1 -- light up with ABYP
 * bit[1]: 0 -- switch to ABYP after light up, 1 -- switch to PT after light up
 * bit[3]: 0 -- non force, 1 -- force abyp during panel switch
 * bit[6]: 0 -- default, 1 -- use dbp instead of abyp
 */
static int _debug_on_opt;

static bool _need_update_iris_for_qsync_in_pt;
static bool _iris_hdr_power;
static bool _iris_bsram_power; /* BSRAM domain power status */
static bool _dpg_temp_disable = false;
static bool _ulps_temp_disable = false;
static bool _flfp_temp_disable = false;
static int _abyp_mode_config;

#define IRIS_TRACE_FPS       0x01
#define IRIS_TRACE_CADENCE   0X02
#define IRIS_EFIFO_BW_128    0
#define IRIS_EFIFO_BW_256    1

static int debug_trace_opt;

static u32 _regs_dump[] = {
	0xf1a00000, //mipi_rx
	0xf1a00004,
	0xf1a0000c,
	0xf1a0002c,
	0xf1a00034,
	0xf1a00204,
	0xf1a00300,
	0xf1a01494,
	0xf0000064, //sys
	0xf0000068,
	0xf00000a4,
	0xf00000d0,
	0xf0000120,
	0xf0000144,
	0xf000028c,
	0xf0000290,
	0xf0010004,
	0xf1940000, //pwil_0
	0xf1940008,
	0xf1940010,
	0xf194015c,
	0xf195ffe4,
	0xf135ffe4, //dtg
	0xf1a9ffe4, //input_dsc_dec_pwil0
	0xf171ffe4, //hdr
	0xf185ffe4, //scaler_1d
	0xf115ffe4, //pb_interconnect
	0xf12c0000, //dport
	0xf12dffe4,
	0xf225ffe4, //dec_encoder
	0xf13c0074, //mipi_tx
	0xf13dffe4,
	0xf131ffe4, //dpp
	0xf1cdffe4, //lut_wrapper
};

static void _iris_abyp_ctrl_init(bool chain);
static void _iris_flfp_set(bool enable, bool chain);

static void _iris_abyp_stop(void);

static int _iris_one_frame_time(void)
{
	struct iris_cfg *pcfg;
	int fps, frame_interval;

	pcfg = iris_get_cfg();
	fps = pcfg->panel->cur_mode->timing.refresh_rate;
	if (fps != 0)
		frame_interval = 1000/fps + 1;
	else
		frame_interval = 1000/60 + 1;
	IRIS_LOGD("%s, frame_interval is %d ms", __func__, frame_interval);
	return frame_interval;
}

int32_t iris_parse_lp_ctrl(struct device_node *np, struct iris_cfg *pcfg)
{
	int32_t rc = 0;
	u8 vals[3];

	pcfg->lp_ctrl.esd_ctrl = 1;
	rc = of_property_read_u32(np, "pxlw,esd-ctrl", &(pcfg->lp_ctrl.esd_ctrl));
	if (rc) {
		IRIS_LOGE("%s, failed to parse pxlw esd-ctrl, return: %d", __func__, rc);
	}
	IRIS_LOGI("%s: pxlw esd-ctrl %#x", __func__, pcfg->lp_ctrl.esd_ctrl);

	pcfg->lp_ctrl.te_swap = of_property_read_bool(np, "pxlw,te-mask-swaped");
	IRIS_LOGI("%s: pxlw,te-mask-swaped %d", __func__, pcfg->lp_ctrl.te_swap);

	rc = of_property_read_u8_array(np, "pxlw,low-power", vals, 3);
	if (rc) {
		IRIS_LOGE("%s(), failed to parse low power property, return: %d", __func__, rc);
		return 0;
	}

	pcfg->lp_ctrl.dynamic_power = (bool)vals[0];
	pcfg->lp_ctrl.ulps_lp = (bool)vals[1];
	pcfg->lp_ctrl.abyp_lp = (u8)vals[2];
	IRIS_LOGI("%s(), parse low power info: %d %d %d", __func__, vals[0], vals[1], vals[2]);

	return rc;
}

void iris_lp_init(void)
{
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	pcfg->read_path = PATH_DSI;

	pcfg->abyp_ctrl.abypass_mode = ANALOG_BYPASS_MODE;
	pcfg->abyp_ctrl.pending_mode = MAX_MODE;
	mutex_init(&pcfg->abyp_ctrl.abypass_mutex);

	IRIS_LOGI("%s: abypass_mode %d", __func__, __LINE__, pcfg->abyp_ctrl.abypass_mode);
}

void iris_lp_enable_pre(void)
{
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	if (_debug_on_opt & 0x1)
		pcfg->abyp_ctrl.abypass_mode = ANALOG_BYPASS_MODE;

	pcfg->abyp_ctrl.abyp_failed = false;
}

void iris_lp_enable_post(void)
{
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	if (pcfg->rx_mode == DSI_OP_CMD_MODE && pcfg->tx_mode == DSI_OP_CMD_MODE)
		pcfg->lp_ctrl.flfp_enable = true;
	else
		pcfg->lp_ctrl.flfp_enable = false;

	iris_init_update_ipopt_t(IRIS_IP_SYS, ID_SYS_DMA_CTRL, ID_SYS_DMA_CTRL, 1);

	iris_dynamic_power_set(pcfg->lp_ctrl.dynamic_power, true);
	_iris_abyp_ctrl_init(false);
	_abyp_mode_config = pcfg->lp_ctrl.abyp_lp;

	iris_ulps_enable(pcfg->lp_ctrl.ulps_lp, false);

	IRIS_LOGI("%s dynamic_power %d, ulps_lp %d, abyp_lp %d", __func__,
			  pcfg->lp_ctrl.dynamic_power, pcfg->lp_ctrl.ulps_lp,
			  pcfg->lp_ctrl.abyp_lp);

}

/*== PMU related APIs ==*/

/* dynamic power gating set */
void iris_dynamic_power_set(bool enable, bool chain)
{
	struct iris_update_regval regval;
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	regval.ip = IRIS_IP_SYS;
	regval.opt_id = ID_SYS_DPG;
	regval.mask = 0x00000001;
	regval.value = (enable ? 0x1 : 0x0);
	iris_update_bitmask_regval_nonread(&regval, false);
	iris_init_update_ipopt_t(regval.ip, regval.opt_id, regval.opt_id, chain);
	if (!chain)
		iris_update_pq_opt(PATH_DSI, true);

	pcfg->lp_ctrl.dynamic_power = enable;
	IRIS_LOGI("%s %d, chain %d", __func__, enable, chain);
}

/* dynamic power gating get */
bool iris_dynamic_power_get(void)
{
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	return pcfg->lp_ctrl.dynamic_power;
}

static int iris_pmu_power_set(enum iris_pmu_domain domain_id, bool on, bool chain)
{
	struct iris_update_regval regval;

	regval.ip = IRIS_IP_SYS;
	regval.opt_id = ID_SYS_MPG;
	regval.mask = domain_id;
	regval.value = (on ? domain_id : 0x0);
	iris_update_bitmask_regval_nonread(&regval, false);
	iris_init_update_ipopt_t(IRIS_IP_SYS, regval.opt_id, regval.opt_id, chain);
	if (!chain)
		iris_update_pq_opt(PATH_DSI, true);

	return 0;
}

/* power on & off mipi2 domain */
int iris_pmu_mipi2_set(bool on)
{
	int rt = 0;

	if (((_debug_lp_opt & 0x1) == 0x1) && !on) {
		IRIS_LOGI("%s: not power down!", __func__);
		return 0;
	}
	rt = iris_pmu_power_set(MIPI2_PWR, on, 0);
	IRIS_LOGI("%s: on - %d, rt - %d", __func__, on, rt);
	return rt;
}

/* power on & off pq domain */
int iris_pmu_pq_set(bool on)
{
	int rt = 0;

	if (((_debug_lp_opt & 0x1) == 0x1) && !on) {
		IRIS_LOGI("%s: not power down!", __func__);
		return 0;
	}
	rt = iris_pmu_power_set(PQ_PWR, on, 0);
	IRIS_LOGI("%s: on - %d, rt - %d", __func__, on, rt);
	return rt;
}

static void _iris_ramctrl_buf_bw_access(int bw)
{
	u32 cmd[2];
	struct iris_cfg *pcfg = iris_get_cfg();

	if (pcfg->proFPGA_detected)
		cmd[0] = IRIS_SRAM_CTRL_ADDR1 + RAMCTRL_EFOB_CTRL;
	else
		cmd[0] = IRIS_SRAM_CTRL_ADDR + RAMCTRL_EFOB_CTRL;

	if (bw == IRIS_EFIFO_BW_128)
		cmd[1] = 0;
	else if (bw == IRIS_EFIFO_BW_256)
		cmd[1] = 0x44;
	else
		IRIS_LOGE("%s: DualFRC BW setting error", __func__);

	iris_ocp_write_mult_vals(2, cmd);
}

/* power on & off bulksram domain */
int iris_pmu_bsram_set(bool on)
{
	int rt = 0;
	int h_res;
	struct iris_cfg *pcfg = iris_get_cfg();

	if (((_debug_lp_opt & 0x2) == 0x2) && !on) {
		IRIS_LOGI("%s: not power down!", __func__);
		return 0;
	}
	if (on != _iris_bsram_power) {
		struct iris_update_regval regval;

		rt = iris_pmu_power_set(BSRAM_PWR, on, 0);
		_iris_bsram_power = on;

		if (on) {
			udelay(200);
			regval.ip = IRIS_IP_SYS;
			regval.opt_id = ID_SYS_MEM_REPAIR;
			regval.mask = 0x000c;
			regval.value = (on ? 0x0004 : 0x0);
			iris_update_bitmask_regval_nonread(&regval, false);
			iris_init_update_ipopt_t(IRIS_IP_SYS, regval.opt_id, regval.opt_id, 0);
			iris_update_pq_opt(PATH_DSI, true);

			h_res = pcfg->panel->cur_mode->timing.h_active;
			IRIS_LOGI("Cur mode h_res = %d", h_res);

			if (h_res < 1440)
				_iris_ramctrl_buf_bw_access(IRIS_EFIFO_BW_128);
			else
				_iris_ramctrl_buf_bw_access(IRIS_EFIFO_BW_256);
		}
	} else {
		IRIS_LOGW("%s: cur %d == on %d", __func__, _iris_bsram_power, on);
		return 2;
	}
	IRIS_LOGI("%s: on - %d, rt - %d", __func__, on, rt);
	return rt;
}
static void _iris_linelock_set(bool enable, bool send)
{
	struct iris_update_regval regval;
	int len;
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	if (pcfg->rx_mode == DSI_OP_VIDEO_MODE && pcfg->tx_mode == DSI_OP_VIDEO_MODE) {
		regval.ip = IRIS_IP_DTG;
		regval.opt_id = ID_DTG_TE_SEL;
		regval.mask = 0x04000000;
		regval.value = (enable ? 0x04000000 : 0x00000000);
		iris_update_bitmask_regval_nonread(&regval, false);
		len = iris_init_update_ipopt_t(regval.ip, regval.opt_id, regval.opt_id, 1);
	}
	regval.ip = IRIS_IP_DTG;
	regval.opt_id = 0xF0;
	regval.mask = 0x0000000F;
	regval.value = 0x2;
	iris_update_bitmask_regval_nonread(&regval, false);
	len = iris_init_update_ipopt_t(IRIS_IP_DTG, 0xF0, 0xF0, 0);

	if (send)
		iris_update_pq_opt(PATH_DSI, true);

	IRIS_LOGI("%s: %s, send: %d", __func__, (enable ? "Line Lock enable" : "Line Lock disable"), send);
}

void iris_linelock_set(bool enable, bool chain)
{
	_iris_linelock_set(enable, chain);
}

/* power on & off frc domain */
int iris_pmu_frc_set(bool on)
{
	int rt = 0;

	if (((_debug_lp_opt & 0x4) == 0x4) && !on) {
		IRIS_LOGI("%s: not power down!", __func__);
		return 0;
	}
	rt = iris_pmu_power_set(FRC_PWR, on, 0);
	IRIS_LOGI("%s: on - %d, rt - %d", __func__, on, rt);
	return rt;
}

/* power on & off dsc unit domain */
int iris_pmu_dscu_set(bool on)
{
	int rt = 0;

	if (((_debug_lp_opt & 0x8) == 0x8) && !on) {
		IRIS_LOGI("%s: not power down!", __func__);
		return 0;
	}
	rt = iris_pmu_power_set(DSCU_PWR, on, 0);
	IRIS_LOGI("%s: on - %d, rt - %d", __func__, on, rt);
	return rt;
}

/* power on & off HDR domain
 *   type: 0 -- power off HDR & HDR_COLOR
 *         1 -- power on HDR, power off HDR_COLOER
 *         2 -- power on HDR & HDR_COLOR
 */
int iris_pmu_hdr_set(bool on, bool chain)
{
	struct iris_cfg *pcfg;
	struct iris_update_regval regval;

	pcfg = iris_get_cfg();

	IRIS_LOGI("%s: on %d, cur pwr %d, chain %d", __func__, on, _iris_hdr_power, chain);

	if (_iris_hdr_power == on) {
		IRIS_LOGI("%s: same type %d", __func__, on);
		return 2;
	}

	regval.ip = IRIS_IP_DMA;
	regval.opt_id = 0xd0; //dpp ch10 power protection
	regval.mask = 0x3;
	regval.value = on ? 0x3 : 0x1;
	iris_update_bitmask_regval_nonread(&regval, false);
	iris_init_update_ipopt_t(regval.ip, regval.opt_id, regval.opt_id, 0x1);

	regval.ip = IRIS_IP_DMA;
	regval.opt_id = 0xd2; //pwil ch12 power protection
	regval.mask = 0x3;
	regval.value = on ? 0x3 : 0x1;
	iris_update_bitmask_regval_nonread(&regval, false);
	iris_init_update_ipopt_t(regval.ip, regval.opt_id, regval.opt_id, 0x1);

	_iris_hdr_power = on;

	iris_pmu_power_set(HDR_PWR, on, chain);

	return 0;
}

/* control dma channels trigger
   input: channels -- bit 0, ch0; bit 1, ch1; bit 2, ch2; bit 3, ch3
          source -- trigger source selection
          chain -- send command with chain or not
 */
void iris_dma_gen_ctrl(int channels, int source, bool chain)
{
	int value = 0;
	uint32_t  *payload = NULL;
	int mask, dma_ctrl;

	if (source > 7) {
		IRIS_LOGE("%s, source %d is wrong!", __func__, source);
		return;
	}

	if(channels & 0x1)
		value |= (0x20 | source);
	if(channels & 0x2)
		value |= ((0x20 | source) << 7);
	if(channels & 0x4)
		value |= ((0x20 | source) << 14);
	if(channels & 0x8)
		value |= ((0x20 | source) << 21);

	payload = iris_get_ipopt_payload_data(IRIS_IP_SYS, ID_SYS_DMA_GEN_CTRL, 4);
	if (!payload) {
		IRIS_LOGE("%s(), can not get pwil ID_SYS_DMA_GEN_CTRL property in pwil setting", __func__);
		return;
	}
	mask = 0x0cf9f3e7; //DMA_CH* ONE_TIME_TRIG_EN, SRC_SEL
	dma_ctrl = payload[0] & (~mask);
	dma_ctrl |= (value & mask);

	IRIS_LOGD("%s: channels 0x%x, source %d, chain %d, value 0x%x, dma_ctrl 0x%x",
		__func__, channels, source, chain, value, dma_ctrl);

	iris_set_ipopt_payload_data(IRIS_IP_SYS, ID_SYS_DMA_GEN_CTRL, 4, dma_ctrl);
	iris_init_update_ipopt_t(IRIS_IP_SYS, ID_SYS_DMA_GEN_CTRL, ID_SYS_DMA_GEN_CTRL, chain);
}

/* control dma trigger
   input: channels -- dma channels. Bit [0] - pq, 1 - hdr, 2 - frc, 3 - dsc_unit, 4 - bulk_sram
                     5 - mipi2, 6 - mipi, .. 10 ~ 15: sw channels
          chain -- send command with chain or not
 */
void iris_dma_trig(int channels, bool chain)
{
	iris_set_ipopt_payload_data(IRIS_IP_DMA, 0xe2, 4, channels);
	iris_init_update_ipopt_t(IRIS_IP_DMA, 0xe2, 0xe2, chain);

	IRIS_LOGD("%s: channels 0x%x, chain %d", __func__, channels, chain);
}

void iris_ulps_enable(bool enable, bool chain)
{
	struct iris_update_regval regval;
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	regval.ip = IRIS_IP_SYS;
	regval.opt_id = ID_SYS_ULPS;
	regval.mask = 0x300;
	regval.value = enable ? 0x100 : 0x0;
	iris_update_bitmask_regval_nonread(&regval, false);
	iris_init_update_ipopt_t(IRIS_IP_SYS, regval.opt_id, regval.opt_id, chain);
	if (!chain)
		iris_update_pq_opt(PATH_DSI, true);

	pcfg->lp_ctrl.ulps_lp = enable;

	IRIS_LOGD("%s %d, chain %d", __func__, enable, chain);
}

bool iris_ulps_enable_get(void)
{
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	IRIS_LOGI("ulps ap:%d, iris:%d",
			pcfg->display->panel->ulps_feature_enabled, pcfg->lp_ctrl.ulps_lp);

	if (pcfg->display->panel->ulps_feature_enabled && pcfg->lp_ctrl.ulps_lp)
		return true;
	else
		return false;
}

/*== Analog bypass related APIs ==*/

void iris_abyp_mode_set(int mode)
{
	if (mode == 1 || mode == 2) {
		_abyp_mode_config = mode;
		IRIS_LOGI("%s() %s ABP mode", __func__, mode == 1 ? "Standby" : "Sleep");
	} else
		IRIS_LOGW("%s() mode %d is not supported!", __func__, mode);
}

void iris_abyp_mode_get(u32 count, u32 *values)
{
	struct iris_cfg *pcfg;
	pcfg = iris_get_cfg();

	values[0] = pcfg->abyp_ctrl.abypass_mode;
	if (count >= 3) {
		values[1] = pcfg->lp_ctrl.abyp_lp << 4;
		values[2] = _abyp_mode_config << 4;
	}
}

static void _iris_abyp_ctrl_init(bool chain)
{
	struct iris_cfg *pcfg;
	struct iris_update_regval regval;

	pcfg = iris_get_cfg();

	regval.ip = IRIS_IP_SYS;
	regval.opt_id = ID_SYS_ABYP_CTRL;
	regval.mask = 0x0CD00000;
	if (pcfg->lp_ctrl.abyp_lp == 1)
		regval.value = 0x00800000;
	else if (pcfg->lp_ctrl.abyp_lp == 2)
		regval.value = 0x00400000;
	else
		regval.value = 0x00000000;

	/* Set digital_bypass_a2i_en/digital_bypass_i2a_en = 1 for video mode */
	if (pcfg->panel->panel_mode == DSI_OP_VIDEO_MODE) {
		regval.value |= 0x0C000000;
		if (pcfg->lp_ctrl.abyp_lp == 0)
			regval.value |= 0x00100000;
	}

	iris_update_bitmask_regval_nonread(&regval, false);
	iris_init_update_ipopt_t(regval.ip, regval.opt_id, regval.opt_id, chain);
	if (!chain)
		iris_update_pq_opt(PATH_DSI, true);
	IRIS_LOGD("%s abyp_lp mode %d", pcfg->lp_ctrl.abyp_lp);
}

/* Switch ABYP by GRCP commands
 * enter_abyp: true -- Enter ABYP, false -- Exit ABYP
*/
static void _iris_send_grcp_abyp(bool enter_abyp)
{
	struct iris_cfg *pcfg = iris_get_cfg();
	uint8_t vc_id_bak = pcfg->vc_ctrl.to_iris_vc_id;

	if (enter_abyp) {
		iris_send_ipopt_cmds(IRIS_IP_SYS, 4);
		IRIS_LOGI("%s, Enter ABYP.", __func__);
	} else {
		pcfg->vc_ctrl.to_iris_vc_id = 2;/*iris7 set vc to 2 when exit abyp by grcp*/
		//first need to power on mipi
		iris_send_ipopt_cmds(IRIS_IP_SYS, 5);
		pcfg->vc_ctrl.to_iris_vc_id = vc_id_bak;
		IRIS_LOGI("%s, Exit ABYP.", __func__);
	}
}

/* set Two Wire 0 interface enable */
void iris_set_two_wire0_enable(void)
{
	struct iris_cfg *pcfg = iris_get_cfg();
	uint8_t vc_id_bak = pcfg->vc_ctrl.to_iris_vc_id;

	pcfg->vc_ctrl.to_iris_vc_id = 2;/*iris7 set vc to 2 when send grcp cmd*/
	//first need to power on mipi
	iris_send_ipopt_cmds(IRIS_IP_SYS, 0xF5);
	pcfg->vc_ctrl.to_iris_vc_id = vc_id_bak;
	IRIS_LOGI("%s, enable Two Wire 0 interface", __func__);
}

static int _iris_set_max_return_size(void)
{
	int rc;
	struct iris_cfg *pcfg;
	static char max_pktsize[2] = {0x01, 0x00}; /* LSB tx first, 2 bytes */
	static struct dsi_cmd_desc pkt_size_cmd = {
		{0, MIPI_DSI_SET_MAXIMUM_RETURN_PACKET_SIZE, MIPI_DSI_MSG_REQ_ACK, 0, 0,
		 sizeof(max_pktsize), max_pktsize, 0, NULL},
		1,
		0};
	struct dsi_panel_cmd_set cmdset = {
		.state = DSI_CMD_SET_STATE_HS,
		.count = 1,
		.cmds = &pkt_size_cmd,
	};

	pcfg = iris_get_cfg();

	IRIS_LOGD("%s", __func__);

	rc = iris_dsi_send_cmds(pcfg->panel, cmdset.cmds, cmdset.count,
							cmdset.state, pcfg->vc_ctrl.to_iris_vc_id);
	if (rc)
		IRIS_LOGE("failed to send max return size packet, rc=%d", rc);

	return rc;
}

static int _iris_lp_check_gpio_status(int cnt, int target_status)
{
	int i;
	int abyp_status_gpio;

	if (cnt <= 0) {
		IRIS_LOGE("invalid param, cnt is %d", cnt);
		return -EINVAL;
	}

	IRIS_LOGD("%s, cnt = %d, target_status = %d", __func__, cnt, target_status);

	/* check abyp gpio status */
	for (i = 0; i < cnt; i++) {
		abyp_status_gpio = iris_check_abyp_ready();
		IRIS_LOGD("%s, %d, ABYP status: %d.", __func__, i, abyp_status_gpio);
		if (abyp_status_gpio == target_status)
			break;
		udelay(3 * 1000);
	}

	return abyp_status_gpio;
}

static void _iris_abyp_stop(void)
{
	int i = 0;
	if (_debug_on_opt & 0x20) {
		while (1) {
			IRIS_LOGI("ABYP switch stop here! cnt: %d", i++);
			msleep(3000);
			if (!(_debug_on_opt & 0x20)) {
				IRIS_LOGI("ABYP switch stop Exit!");
				break;
			}
		}
	} else {
		IRIS_LOGI("ABYP debug option not enable.");
	}
}

static struct drm_encoder *iris_get_drm_encoder_handle(void)
{
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	if (pcfg->display->bridge == NULL || pcfg->display->bridge->base.encoder == NULL) {
		IRIS_LOGE("Can not get drm encoder");
		return NULL;
	}

	return pcfg->display->bridge->base.encoder;
}

static void _iris_wait_prev_frame_done(void)
{
	int i = 0;
	struct drm_encoder *drm_enc = iris_get_drm_encoder_handle();
	struct sde_encoder_virt *sde_enc = NULL;

	if (!drm_enc) {
		IRIS_LOGE("invalid encoder\n");
		return;
	}

	sde_enc = to_sde_encoder_virt(drm_enc);
	for (i = 0; i < sde_enc->num_phys_encs; i++) {
		struct sde_encoder_phys *phys = sde_enc->phys_encs[i];
		int pending_cnt = 0;

		if (phys->split_role != ENC_ROLE_SLAVE) {
			int j = 0;

			pending_cnt = atomic_read(&phys->pending_kickoff_cnt);
			for (j = 0; j < pending_cnt; j++)
				sde_encoder_wait_for_event(phys->parent, MSM_ENC_TX_COMPLETE);

			break;
		}
	}
}

int iris_lp_abyp_enter(void)
{
	struct iris_cfg *pcfg;
	// struct iris_update_regval regval;
	int abyp_status_gpio, toler_cnt;
	int rc = 0;
	int fps, frame_interval;
	ktime_t lp_ktime0;

	pcfg = iris_get_cfg();
	fps = pcfg->panel->cur_mode->timing.refresh_rate;
	if (fps != 0)
		frame_interval = 1000/fps + 1;
	else
		frame_interval = 1000/60 + 1;
	IRIS_LOGI("%s, frame_interval is %d ms", __func__, frame_interval);

	lp_ktime0 = ktime_get();
	toler_cnt = 0;

	if (pcfg->lp_ctrl.abyp_lp != _abyp_mode_config) {
		pcfg->lp_ctrl.abyp_lp = _abyp_mode_config;
		_iris_abyp_ctrl_init(false);
	}
	IRIS_LOGI("Enter abyp mode %d start", pcfg->lp_ctrl.abyp_lp);

enter_abyp_begin:
	_iris_send_grcp_abyp(true);

	/*get the gpio status in 2-3 frames*/
	abyp_status_gpio = _iris_lp_check_gpio_status((frame_interval*3/3 + 1), 1);
	if (_debug_lp_opt & 0x04) {
		// for error process debug
		_debug_lp_opt &= ~0x04;
		abyp_status_gpio = 0;
	}
	if (abyp_status_gpio == 1) {
		pcfg->abyp_ctrl.abypass_mode = IRIS_ABYP_MODE;
	} else {
		if (toler_cnt > 0) {
			IRIS_LOGE("Enter abyp failed, %d, toler_cnt = %d", __LINE__, toler_cnt);
			// iris_reset();
			// iris_lightup_exit_abyp(true, true);
			// iris_preload();
			toler_cnt--;
			goto enter_abyp_begin;
		} else {
			_iris_abyp_stop();
			// pcfg->abyp_ctrl.abyp_failed = true;
			IRIS_LOGE("Enter abyp failed, %d, toler_cnt = %d", __LINE__, toler_cnt);
			rc = -1;
			return rc;
		}
	}
	IRIS_LOGI("Enter abyp done, spend time %d us",
			(u32)ktime_to_us(ktime_get()) - (u32)ktime_to_us(lp_ktime0));

	//pcfg->abyp_ctrl.abyp_failed = false;
	return rc;
}

int iris_lp_abyp_exit(void)
{
	struct iris_cfg *pcfg;
	int abyp_status_gpio;
	int toler_cnt = 0;
	int rc = 0;
	int fps, frame_interval;
	ktime_t lp_ktime0;

	pcfg = iris_get_cfg();
	fps = pcfg->panel->cur_mode->timing.refresh_rate;
	if (fps != 0)
		frame_interval = 1000/fps + 1;
	else
		frame_interval = 1000/60 + 1;
	IRIS_LOGI("%s, frame_interval is %d ms", __func__, frame_interval);
	IRIS_LOGI("Exit abyp mode %d start", pcfg->lp_ctrl.abyp_lp);

	lp_ktime0 = ktime_get();

	if (pcfg->lp_ctrl.abyp_lp == 2)
		_iris_wait_prev_frame_done();

	/* exit analog bypass */
	iris_send_one_wired_cmd(IRIS_EXIT_ANALOG_BYPASS);
	SDE_ATRACE_BEGIN("iris_abyp_exit_cmd");
	if (pcfg->rx_mode == 1 && pcfg->display->panel->ulps_feature_enabled
		&& iris_platform_get() != 0) {
		udelay(1000);
		_iris_set_max_return_size();
	}
	SDE_ATRACE_END("iris_abyp_exit_cmd");
	//mutex_lock(&pcfg->abyp_ctrl.abyp_mutex);

exit_abyp_loop:
	/*get the gpio status in 2-3 frames*/
	abyp_status_gpio = _iris_lp_check_gpio_status((frame_interval*3/3 + 1), 0);
	if (abyp_status_gpio == 0)
		pcfg->abyp_ctrl.abypass_mode = IRIS_PT_MODE;

	if (_debug_lp_opt & 0x04) {
		// for error process debug
		_debug_lp_opt &= ~0x04;
		abyp_status_gpio = 1;
	}

	if (abyp_status_gpio != 0 || rc == -2) {
		if (toler_cnt > 0) {
			IRIS_LOGW("Exit abyp with preload, %d", __LINE__);
			iris_dump_regs(_regs_dump, ARRAY_SIZE(_regs_dump));
			// iris_reset();
			// iris_lightup_exit_abyp(true, true);
			// iris_preload();
			toler_cnt--;
			goto exit_abyp_loop;
		} else {
			IRIS_LOGE("Exit abyp failed, %d", __LINE__);
			_iris_abyp_stop();
			return -1;
		}

	} else {
		if (pcfg->lp_ctrl.abyp_lp == 2) {
			IRIS_LOGI("abyp light up iris");
			iris_lightup(pcfg->panel);
			IRIS_LOGI("Light up time %d us",
				(u32)ktime_to_us(ktime_get()) - (u32)ktime_to_us(lp_ktime0));
		} else {
			iris_switch_from_abyp_to_pt();
		}
	}

	IRIS_LOGI("Exit abyp done, spend time %d us",
		(u32)ktime_to_us(ktime_get()) - (u32)ktime_to_us(lp_ktime0));

	//pcfg->abyp_ctrl.abyp_failed = false;
	return rc;
}

int iris_dbp_switch(bool enter, bool chain)
{
	struct iris_cfg *pcfg = iris_get_cfg();
	struct iris_update_regval regval;
	int rc = 0;

	if (pcfg->lp_ctrl.dbp_mode == enter) {
		IRIS_LOGW("%s same mode:%d!", __func__, enter);
		return rc;
	}
	IRIS_LOGI("%s, enter: %d, chain: %d", __func__, enter, chain);

	regval.ip = IRIS_IP_TX;
	regval.opt_id = ID_TX_BYPASS_CTRL;
	regval.mask = 0x2;
	regval.value = enter ? 0x2 : 0x0;
	iris_update_bitmask_regval_nonread(&regval, false);
	iris_init_update_ipopt_t(regval.ip, regval.opt_id, regval.opt_id, chain);
	if (!chain)
		iris_update_pq_opt(PATH_DSI, true);
	pcfg->lp_ctrl.dbp_mode = enter;

	return rc;
}

/* Return: true is PT, false is Bypass */
bool iris_abyp_switch_proc(struct dsi_display *display, int mode)
{
	struct iris_cfg *pcfg;
	int rc = 0;
	ktime_t ktime0 = 0;
	int prev_mode = 0;

	pcfg = iris_get_cfg();
	prev_mode = pcfg->abyp_ctrl.abypass_mode;

	if (pcfg->rx_mode != pcfg->tx_mode) {
		IRIS_LOGE("abyp can't be supported! rx_mode != tx_mode!");
		return -1;
	}

	if ((mode & 0x7F) == pcfg->abyp_ctrl.abypass_mode) {
		IRIS_LOGW("%s same mode:%d!", __func__, mode);
		// return rc;
	}

	if (_debug_on_opt & 0x40) {
		IRIS_LOGW("%s use dbp instead of abyp", __func__);
		if ((mode & BIT(0)) == ANALOG_BYPASS_MODE) {
			rc = iris_dbp_switch(true, false);
			pcfg->abyp_ctrl.abypass_mode = IRIS_ABYP_MODE;
		} else if ((mode & BIT(0)) == PASS_THROUGH_MODE) {
			rc = iris_dbp_switch(false, false);
			pcfg->abyp_ctrl.abypass_mode = IRIS_PT_MODE;
		} else
			IRIS_LOGE("%s: switch mode: %d not supported!", __func__, mode);
		return rc;
	}

	if (_debug_on_opt & 0x1000)
		ktime0 = ktime_get();

	mutex_lock(&pcfg->abyp_ctrl.abypass_mutex);

	// Check GPIO or mipi inside abyp_enter, abyp_exit
	if ((mode & BIT(0)) == ANALOG_BYPASS_MODE) {
		SDE_ATRACE_BEGIN("iris_abyp_enter");
		rc = iris_lp_abyp_enter();
		SDE_ATRACE_END("iris_abyp_enter");
	} else if ((mode & BIT(0)) == PASS_THROUGH_MODE) {
		SDE_ATRACE_BEGIN("iris_abyp_exit");
		rc = iris_lp_abyp_exit();
		SDE_ATRACE_END("iris_abyp_exit");
		if (rc == 0) {
			if (_need_update_iris_for_qsync_in_pt) {
				if (display->panel->qsync_mode > 0)
					iris_qsync_set(true);
				else
					iris_qsync_set(false);
				IRIS_LOGD("Update iris for QSYNC, qsync_mode: %d", display->panel->qsync_mode);
			}
		}
	} else
		IRIS_LOGE("%s: switch mode: %d not supported!", __func__, mode);
	mutex_unlock(&pcfg->abyp_ctrl.abypass_mutex);

	if (_debug_on_opt & 0x1000) {
		IRIS_LOGI("%s mode: %d -> %d spend time %d", __func__, prev_mode, mode,
			(u32)ktime_to_us(ktime_get()) - (u32)ktime_to_us(ktime0));
	}

	return rc;
}

int iris_exit_abyp(bool one_wired)
{
	int i = 0;
	int abyp_status_gpio;

	/* try to exit abyp */
	if (one_wired) {
		iris_send_one_wired_cmd(IRIS_EXIT_ANALOG_BYPASS);
		udelay(2000);
	} else {
		_iris_send_grcp_abyp(false); /* switch by MIPI command */
		udelay(100);
	}
	IRIS_LOGI("send exit abyp, one_wired:%d.", one_wired);

	/* check abyp gpio status */
	for (i = 0; i < 50; i++) {
		abyp_status_gpio = iris_check_abyp_ready();
		IRIS_LOGD("%s, ABYP status: %d.", __func__, abyp_status_gpio);
		if (abyp_status_gpio == 0)
			break;
		udelay(3 * 1000);
	}

	if (abyp_status_gpio == 1) {
		IRIS_LOGE("%s(), failed to exit abyp!", __func__);
		_iris_abyp_stop();
	}

	return abyp_status_gpio;
}

int iris_lightup_opt_get(void)
{
	return _debug_on_opt;
}

#define CHECK_KICKOFF_FPS_CADNENCE
#if defined(CHECK_KICKOFF_FPS_CADNENCE)
int getCadenceDiff60(long timeDiff)
{
	int cadDiff = 0;

	while (1) {
		if (timeDiff < (((cadDiff + 1) * 100 / 6) - 8))
			break;
		cadDiff++;
		if (cadDiff >= 15)
			break;
	}
	return cadDiff;
}

int getCadenceDiff90(long timeDiff)
{
	int cadDiff = 0;

	while (1) {
		if (timeDiff < (((cadDiff + 1) * 100 / 9) - 5))
			break;
		cadDiff++;
		if (cadDiff >= 15)
			break;
	}
	return cadDiff;
}

int getCadenceDiff120(long timeDiff)
{
	int cadDiff = 0;

	while (1) {
		if (timeDiff < (((cadDiff + 1) * 100 / 12) - 3))
			break;
		cadDiff++;
		if (cadDiff >= 15)  // 4-bit
			break;
	}
	return cadDiff;
}

int getFrameDiff(long timeDiff)
{
	int panel_rate = 60;
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	if (pcfg->panel) {
		if (pcfg->panel->cur_mode
			&& pcfg->panel->panel_initialized)
			panel_rate = pcfg->panel->cur_mode->timing.refresh_rate;
	}
	if (panel_rate == 90)
		return getCadenceDiff90(timeDiff);
	else if (panel_rate == 120)
		return getCadenceDiff120(timeDiff);
	else
		return getCadenceDiff60(timeDiff);

}

#define CHECK_KICKOFF_FPS_DURATION      5 /*EVERY 5s*/

void iris_check_kickoff_fps_cadence(void)
{
	static u32 kickoff_cnt;
	u32 timeusDelta = 0;
	static ktime_t ktime_kickoff_start;
	static u32 us_last_kickoff;
	ktime_t ktime_kickoff;
	static u32 cadence[10];
	static int cdIndex;
	u32 us_timediff;

	if (kickoff_cnt == 0) {
		kickoff_cnt++;
		ktime_kickoff_start = ktime_get();
		memset(cadence, 0, sizeof(cadence));
		cdIndex = 0;
		cadence[cdIndex++] = 0;
		us_last_kickoff = (u32)ktime_to_us(ktime_kickoff_start);
	} else {
		kickoff_cnt++;
		ktime_kickoff = ktime_get();
		timeusDelta = (u32)ktime_to_us(ktime_kickoff) - (u32)ktime_to_us(ktime_kickoff_start);
		us_timediff = (u32)ktime_to_us(ktime_kickoff) - us_last_kickoff;
		us_last_kickoff = (u32)ktime_to_us(ktime_kickoff);
		if (cdIndex > 9)
			cdIndex = 0;

		cadence[cdIndex++] = getFrameDiff((us_timediff+500)/1000);//16667
		if (timeusDelta > 1000000*CHECK_KICKOFF_FPS_DURATION) {
			if ((debug_trace_opt&IRIS_TRACE_FPS) == IRIS_TRACE_FPS)
				IRIS_LOGI("iris: kickoff fps % d", kickoff_cnt/CHECK_KICKOFF_FPS_DURATION);
			if ((debug_trace_opt&IRIS_TRACE_CADENCE) == IRIS_TRACE_CADENCE)
				IRIS_LOGI("iris: Latest cadence: %d %d %d %d %d, %d %d %d %d %d",
						cadence[0], cadence[1], cadence[2], cadence[3], cadence[4],
						cadence[5], cadence[6], cadence[7], cadence[8], cadence[9]);
			kickoff_cnt = 0;
		}
	}
}

void iris_check_kickoff_fps_cadence_2nd(void)
{
	static u32 kickoff_cnt;
	u32 timeusDelta = 0;
	static ktime_t ktime_kickoff_start;
	static u32 us_last_kickoff;
	ktime_t ktime_kickoff;
	static u32 cadence[10];
	static int cdIndex;
	u32 us_timediff;

	if (kickoff_cnt == 0) {
		kickoff_cnt++;
		ktime_kickoff_start = ktime_get();
		memset(cadence, 0, sizeof(cadence));
		cdIndex = 0;
		cadence[cdIndex++] = 0;
		us_last_kickoff = (u32)ktime_to_us(ktime_kickoff_start);
	} else {
		kickoff_cnt++;
		ktime_kickoff = ktime_get();
		timeusDelta = (u32)ktime_to_us(ktime_kickoff) - (u32)ktime_to_us(ktime_kickoff_start);
		us_timediff = (u32)ktime_to_us(ktime_kickoff) - us_last_kickoff;
		us_last_kickoff = (u32)ktime_to_us(ktime_kickoff);
		if (cdIndex > 9)
			cdIndex = 0;

		cadence[cdIndex++] = getFrameDiff((us_timediff+500)/1000);//16667
		if (timeusDelta > 1000000*CHECK_KICKOFF_FPS_DURATION) {
			if ((debug_trace_opt&IRIS_TRACE_FPS) == IRIS_TRACE_FPS)
				IRIS_LOGI("iris:2nd kickoff fps % d", kickoff_cnt/CHECK_KICKOFF_FPS_DURATION);
			if ((debug_trace_opt&IRIS_TRACE_CADENCE) == IRIS_TRACE_CADENCE)
				IRIS_LOGI("iris:2nd Latest cadence: %d %d %d %d %d, %d %d %d %d %d",
						cadence[0], cadence[1], cadence[2], cadence[3], cadence[4],
						cadence[5], cadence[6], cadence[7], cadence[8], cadence[9]);
			kickoff_cnt = 0;
		}
	}
}
#endif

void iris_set_metadata(bool panel_lock)
{
	struct iris_cfg *pcfg = iris_get_cfg();
	uint32_t dport_meta = pcfg->metadata & 0x3;
	uint32_t osd_meta = (pcfg->metadata >> 2) & 0x3;
	uint32_t dpp_meta = (pcfg->metadata >> 4) & 0x3;

	if (pcfg->metadata == 0)
		return;
	if (pcfg->iris_initialized == false) {
		pcfg->metadata = 0;
		IRIS_LOGI("clean metadata when iris not initialized");
		return;
	}
	IRIS_LOGD("dport_meta: %x, osd_meta: %x, dpp_meta: %x", dport_meta, osd_meta, dpp_meta);
	pcfg->metadata = 0;

	if (panel_lock)
		mutex_lock(&pcfg->panel->panel_lock);
	// bit: 0x: nothing, 10: disable dport, 11: enable dport
	switch (dport_meta) {
	case 0x2:
		iris_dom_set(0);
		break;
	case 0x3:
		iris_dom_set(2);
		break;
	default:
		break;
	}
	// bit: 0x: nothing, 10: disable dual, 11: enable dual
	switch (osd_meta) {
	case 0x2:
		break;
	case 0x3:
		break;
	default:
		break;
	}

	switch (dpp_meta) {
	case 0x2:
		iris_pwil_dpp_en(false);
		break;
	case 0x3:
		iris_pwil_dpp_en(true);
		break;
	default:
		break;
	}

	if (panel_lock)
		mutex_unlock(&pcfg->panel->panel_lock);
}

int iris_prepare_for_kickoff(void *phys_enc)
{
	struct sde_encoder_phys *phys_encoder = phys_enc;
	struct sde_connector *c_conn = NULL;
	struct dsi_display *display = NULL;
	struct iris_cfg *pcfg;
	int mode;

	if (phys_encoder == NULL)
		return -EFAULT;
	if (phys_encoder->connector == NULL)
		return -EFAULT;

	c_conn = to_sde_connector(phys_encoder->connector);
	if (c_conn == NULL)
		return -EFAULT;

	if (c_conn->connector_type != DRM_MODE_CONNECTOR_DSI)
		return 0;

	display = c_conn->display;
	if (display == NULL)
		return -EFAULT;

	pcfg = iris_get_cfg();
	if (pcfg->valid < PARAM_PARSED)
		return 0;

#if defined(CHECK_KICKOFF_FPS_CADNENCE)
	if (iris_virtual_display(display)) {
		if (debug_trace_opt > 0)
			iris_check_kickoff_fps_cadence_2nd();
		return 0;
	}

	if (debug_trace_opt > 0)
		iris_check_kickoff_fps_cadence();
#endif
	mutex_lock(&pcfg->abyp_ctrl.abypass_mutex);
	if (pcfg->abyp_ctrl.pending_mode != MAX_MODE) {
		mode = pcfg->abyp_ctrl.pending_mode;
		pcfg->abyp_ctrl.pending_mode = MAX_MODE;
		mutex_unlock(&pcfg->abyp_ctrl.abypass_mutex);
		mutex_lock(&pcfg->panel->panel_lock);
		iris_abyp_switch_proc(pcfg->display, mode);
		mutex_unlock(&pcfg->panel->panel_lock);
	} else
		mutex_unlock(&pcfg->abyp_ctrl.abypass_mutex);

	iris_set_metadata(true);
	return 0;
}

void iris_power_up_mipi(void)
{
	struct iris_cfg *pcfg = iris_get_cfg();
	struct dsi_display *display = pcfg->display;

	if (iris_virtual_display(display))
		return;

	IRIS_LOGI("%s(%d), power up mipi", __func__, __LINE__);
	iris_send_one_wired_cmd(IRIS_POWER_UP_MIPI);
	udelay(3500);
}

int iris_get_abyp_mode(struct dsi_panel *panel)
{
	struct iris_cfg *pcfg = iris_get_cfg();

	IRIS_LOGD("%s(%d), secondary: %d abyp mode: %d",
			__func__, __LINE__,
			panel->is_secondary,
			pcfg->abyp_ctrl.abypass_mode);
	return (!panel->is_secondary) ?
		pcfg->abyp_ctrl.abypass_mode : ANALOG_BYPASS_MODE;
}

/*== ESD ==*/
int iris_esd_ctrl_get(void)
{
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	return pcfg->lp_ctrl.esd_ctrl;
}

static int _iris_esd_check(void)
{
	int rc = 1;
	unsigned int run_status = 0x00;
	struct iris_cfg *pcfg;

	char get_diag_result[1] = {0x0f};
	char rbuf[16] = {0};
	struct dsi_cmd_desc cmds = {
		{0, MIPI_DSI_DCS_READ, MIPI_DSI_MSG_REQ_ACK, 0, 0,
			sizeof(get_diag_result), get_diag_result, 2, rbuf},
		1,
		0};
	struct dsi_panel_cmd_set cmdset = {
		.state = DSI_CMD_SET_STATE_HS,
		.count = 1,
		.cmds = &cmds,
	};

	pcfg = iris_get_cfg();

	rc = iris_dsi_send_cmds(pcfg->panel, cmdset.cmds, cmdset.count,
							cmdset.state, pcfg->vc_ctrl.to_iris_vc_id);

	run_status = rbuf[1] & 0x7;
	if ((iris_esd_ctrl_get() & 0x8) || IRIS_IF_LOGD()) {
		IRIS_LOGI("dsi read iris esd value: 0x%02x 0x%02x. run_status:0x%x. rc:%d.",
					rbuf[0], rbuf[1], run_status, rc);
	}
	if (rc) {
		IRIS_LOGI("%s dsi read iris esd err: %d", __func__, rc);
		rc = -1;
		goto exit;
	}

	if (iris_esd_ctrl_get() & 0x10) {
		run_status = 0xff;
		pcfg->lp_ctrl.esd_ctrl &= ~0x10;
		IRIS_LOGI("iris esd %s force trigger", __func__);
	}

	if (run_status != 0) {
		IRIS_LOGI("iris esd err status 0x%x. ctrl: %d", run_status, pcfg->lp_ctrl.esd_ctrl);
		iris_dump_regs(_regs_dump, ARRAY_SIZE(_regs_dump));
		rc = -2;
	} else {
		rc = 1;
	}

exit:
	if (rc < 0) {
		pcfg->lp_ctrl.esd_cnt_iris++;
		IRIS_LOGI("iris esd err cnt: %d. rc %d", pcfg->lp_ctrl.esd_cnt_iris, rc);
	}

	IRIS_LOGD("%s rc:%d", __func__, rc);

	return rc;
}

int iris_status_get(struct dsi_display_ctrl *ctrl, struct dsi_panel *panel)
{
	int rc = 0;
	int mode;
	struct iris_cfg *pcfg;
	ktime_t lp_ktime0;

	pcfg = iris_get_cfg();

	// check abyp mode
	mode = pcfg->abyp_ctrl.abypass_mode;

	if ((iris_esd_ctrl_get() & 0x8) || IRIS_IF_LOGD()) {
		IRIS_LOGI("esd %s, mode: %d", __func__, mode);
		lp_ktime0 = ktime_get();
	}

	if ((mode != PASS_THROUGH_MODE) && (mode != ANALOG_BYPASS_MODE)) {
		rc = 1;
		goto exit;
	}

	if ((iris_esd_ctrl_get() & 0x1) && (mode == PASS_THROUGH_MODE)) {
		// iris esd check in pt mode
		rc = _iris_esd_check();
		if (rc <= 0)
			goto exit;
	}

	if (iris_esd_ctrl_get() & 0x2)
		rc = 2;
	else
		rc = 1;

exit:
	//mutex_unlock(&pcfg->panel->panel_lock);
	if (rc <= 0) {
		if ((iris_esd_ctrl_get() & 0x4) == 0)
			rc = 1; /* Force not return error */
	} else {
		if ((iris_esd_ctrl_get() & 0x8) ||
			((iris_esd_ctrl_get() & 0x3) && IRIS_IF_LOGD())) {
			IRIS_LOGI("esd %s done rc: %d, spend time %d us", __func__,
				rc,(u32)ktime_to_us(ktime_get()) - (u32)ktime_to_us(lp_ktime0));
			}
	}
	return rc;
}

void iris_dump_regs(u32 regs[], u32 len)
{
	u32 value, i;
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	if (iris_esd_ctrl_get() & 0x20)
		return;

	for (i = 0; i < len; i++) {
		//if (pcfg->read_path == PATH_I2C) //use i2c to read
		//	iris_ioctl_i2c_read(regs[i], &value);
		//else
		//{
		value = iris_ocp_read(regs[i], DSI_CMD_SET_STATE_HS);
		//}
		IRIS_LOGI("[%02d] %08x : %08x", i, regs[i], value);
	}
}

bool iris_check_reg_read(struct dsi_panel *panel)
{
	int i, j = 0;
	int len = 0, *lenp;
	int group = 0, count = 0;
	struct drm_panel_esd_config *config;
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	if (!panel)
		return false;

	if (!(iris_esd_ctrl_get() & 0x8) && !(IRIS_IF_LOGD()))
		return false;

	config = &(panel->esd_config);

	lenp = config->status_valid_params ?: config->status_cmds_rlen;
	count = config->status_cmd.count;

	for (i = 0; i < count; i++)
		len += lenp[i];

	for (j = 0; j < config->groups; ++j) {
		for (i = 0; i < len; ++i) {
			IRIS_LOGI("panel esd [%d] - [%d] : 0x%x", j, i, config->return_buf[i]);

			if (config->return_buf[i] != config->status_value[group + i]) {
				pcfg->lp_ctrl.esd_cnt_panel++;
				IRIS_LOGI("mismatch: 0x%x != 0x%x. Cnt:%d", config->return_buf[i],
					config->status_value[group + i], pcfg->lp_ctrl.esd_cnt_panel);
				break;
			}
		}

		if (i == len)
			return true;
		group += len;
	}

	return false;
}

/*== Low Power Misc ==*/

static void _iris_tx_buf_to_vc_set(bool enable, bool chain)
{
	struct iris_update_regval regval;
	int len;
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	/* MIPI_TX */
	regval.ip = IRIS_IP_TX;
	regval.opt_id = ID_TX_BYPASS_CTRL;
	regval.mask = 0x00000008;
	regval.value = (enable ? 0x00000008 : 0x0);

	iris_update_bitmask_regval_nonread(&regval, false);
	len = iris_init_update_ipopt_t(regval.ip, regval.opt_id, regval.opt_id, chain);
	if (!chain)
		iris_update_pq_opt(PATH_DSI, true);
	pcfg->vc_ctrl.vc_enable = enable;

	IRIS_LOGI("%s vc %d, chain: %d", __func__, enable, chain);
}

void iris_tx_buf_to_vc_set(bool enable)
{
	_iris_tx_buf_to_vc_set(enable, false);
}

static void _iris_flfp_set(bool enable, bool chain)
{
	struct iris_update_regval regval;
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	if (pcfg->lp_ctrl.te_swap) {
		/* SYS */
		regval.ip = IRIS_IP_SYS;
		regval.opt_id = ID_SYS_TE_SWAP;
		regval.mask = 0x00800000;
		regval.value = (enable ? 0x00800000 : 0x0);

		iris_update_bitmask_regval_nonread(&regval, false);
		iris_init_update_ipopt_t(regval.ip, regval.opt_id, regval.opt_id, 1);
	} else {
		/* SYS */
		regval.ip = IRIS_IP_SYS;
		regval.opt_id = ID_SYS_TE_BYPASS;
		regval.mask = 0x02000000;  //EXT_TE_SEL
		regval.value = (enable ? 0x02000000 : 0x0);

		iris_update_bitmask_regval_nonread(&regval, false);
		iris_init_update_ipopt_t(regval.ip, regval.opt_id, regval.opt_id, 1);
	}

	if (pcfg->rx_mode == DSI_OP_CMD_MODE && pcfg->tx_mode == DSI_OP_CMD_MODE) {
		/* DTG change for CMD mode only
		* TE_DLY need to 0, DTG TE2 using pad_te
		* enable:  TE using mipi_rx
		* disable: TE using pad_te
		*/
		regval.ip = IRIS_IP_DTG;
		regval.opt_id = ID_DTG_TE_SEL;
		regval.mask = 0x000001C0;
		regval.value = (enable ? 0x00000140 : 0x00000040);
		iris_update_bitmask_regval_nonread(&regval, false);
		iris_init_update_ipopt_t(regval.ip, regval.opt_id, regval.opt_id, 1);
		iris_init_update_ipopt_t(IRIS_IP_DTG, 0xF0, 0xF0, 1); /* Use force update */
	}

	/* MIPI_TX */
	regval.ip = IRIS_IP_TX;
	regval.opt_id = ID_TX_TE_FLOW_CTRL;
	regval.mask = 0x00000003;
	regval.value = (enable ? 0x2 : 0x0); // 0x2: DTG, 0x0: GPIO

	iris_update_bitmask_regval_nonread(&regval, false);
	iris_init_update_ipopt_t(regval.ip, regval.opt_id, regval.opt_id, chain);

	if (!chain)
		iris_update_pq_opt(PATH_DSI, true);

	pcfg->lp_ctrl.flfp_enable = enable;

	IRIS_LOGI("%s %d, chain: %d", __func__, enable, chain);
}

void iris_tx_pb_req_set(bool enable, bool chain)
{
	struct iris_cfg *pcfg;
	struct iris_update_regval regval;

	pcfg = iris_get_cfg();

	regval.ip = IRIS_IP_TX;
	regval.opt_id = ID_TX_TE_FLOW_CTRL;
	regval.mask = 0x00030000; //PB_FLOW_MODE_NORMAL
	regval.value = (enable ? 0x30000 : 0x20000);
	iris_update_bitmask_regval_nonread(&regval, false);
	iris_init_update_ipopt_t(regval.ip, regval.opt_id, regval.opt_id, chain);

	if (!chain)
		iris_update_pq_opt(PATH_DSI, true);
}

/* switch low power setting for frc/pt mode
	frc_mode: 0 -- pt mode, 1 -- frc/dual mode
*/
void iris_frc_lp_switch(bool frc_mode, bool chain)
{
	struct iris_cfg *pcfg;
	u32  dpg_wait_ms;

	pcfg = iris_get_cfg();
	dpg_wait_ms = _iris_one_frame_time();

	if (frc_mode) {
		if (iris_dynamic_power_get()) {
			iris_dynamic_power_set(false, false);
			usleep_range(dpg_wait_ms * 1000,
					((dpg_wait_ms * 1000) + 10));//Wait one frame for frc dma autoload done.
			_dpg_temp_disable = true;
		}
		if (pcfg->lp_ctrl.flfp_enable) {
			_iris_flfp_set(false, true);
			_flfp_temp_disable = true;
		}
		if (pcfg->lp_ctrl.ulps_lp) {
			iris_ulps_enable(false, true);
			_ulps_temp_disable = true;
		}
		/* disable virtual channel before memc */
		_iris_tx_buf_to_vc_set(pcfg->vc_ctrl.vc_arr[VC_FRC], chain);
	} else {
		iris_tx_pb_req_set(true, true);
		if (_dpg_temp_disable) {
			if (!iris_dynamic_power_get())
				iris_dynamic_power_set(true, true);
			_dpg_temp_disable = false;
		}
		if (_flfp_temp_disable) {
			if (!pcfg->lp_ctrl.flfp_enable)
				_iris_flfp_set(true, true);
			_flfp_temp_disable = false;
		}
		if (_ulps_temp_disable) {
			if (!pcfg->lp_ctrl.ulps_lp)
				iris_ulps_enable(true, true);
			_ulps_temp_disable = false;
		}
		/* enable vritual chanel after exit frc */
		_iris_tx_buf_to_vc_set(pcfg->vc_ctrl.vc_arr[VC_PT], chain);
	}
	IRIS_LOGD("%s %d chain: %d.", __func__, frc_mode, chain);
}

bool iris_qsync_update_need(void)
{
	return _need_update_iris_for_qsync_in_pt;
}

void iris_qsync_set(bool enable)
{
	struct iris_cfg *pcfg;
	struct iris_update_regval regval;

	if (!iris_is_chip_supported())
		return;

	pcfg = iris_get_cfg();
	IRIS_LOGD("%s, mode: %d. enable: %d", __func__, pcfg->abyp_ctrl.abypass_mode, enable);

	if (pcfg->abyp_ctrl.abypass_mode == PASS_THROUGH_MODE) {
		IRIS_LOGI("%s, Qsync %d.", __func__, enable);
		regval.ip = IRIS_IP_DTG;
		regval.opt_id = 0xf2; //ivsa filter ctrl
		regval.mask = 0x80000000;
		regval.value = (enable ? 0x0 : 0x80000000);
		iris_update_bitmask_regval_nonread(&regval, false);
		iris_init_update_ipopt_t(regval.ip, regval.opt_id, regval.opt_id, 1);

		regval.ip = IRIS_IP_DTG;
		regval.opt_id = 0xf0; //dtg update
		iris_init_update_ipopt_t(regval.ip, regval.opt_id, regval.opt_id, 1);

		//pwil te_reset
		//Fixme: WA for two continous frames in one TE
		regval.ip = IRIS_IP_PWIL;
		regval.opt_id = 0x90;
		regval.mask = 0x00000100;
		regval.value = (enable ? 0x0 : 0x100);
		iris_update_bitmask_regval_nonread(&regval, false);
		iris_init_update_ipopt_t(regval.ip, regval.opt_id, regval.opt_id, 1);

		iris_dma_trig((1<<12), 0);

		iris_update_pq_opt(PATH_DSI, true);
		_need_update_iris_for_qsync_in_pt = false;
	} else {
		_need_update_iris_for_qsync_in_pt = true;
		IRIS_LOGI("Need update iris for Qsync, mode: %d", pcfg->panel->qsync_mode);
	}
}

void iris_lp_setting_off(void)
{
	struct iris_cfg *pcfg = iris_get_cfg();

	pcfg->abyp_ctrl.pending_mode = MAX_MODE;
	_iris_bsram_power = false;
	iris_pmu_power_set(BSRAM_PWR, 0, 1);
	iris_pmu_power_set(FRC_PWR, 0, 1);
	iris_pmu_power_set(MIPI2_PWR, 0, 1);
	iris_pmu_power_set(DSCU_PWR, 0, 1);

	iris_dbp_switch(false, true);

	iris_frc_lp_switch(false, true);
}

static int _iris_mipi_queue(int value)
{
	int rc = 1;
	struct iris_cfg *pcfg;

	char mipi_queue[2] = {0x0, 0x0};
	struct dsi_cmd_desc cmds = {
		{0, MIPI_DSI_EXECUTE_QUEUE, 0, 0, 0,
			sizeof(mipi_queue), mipi_queue, 0, NULL},
		1,
		0};
	struct dsi_panel_cmd_set cmdset = {
		.state = DSI_CMD_SET_STATE_HS,
		.count = 1,
		.cmds = &cmds,
	};

	pcfg = iris_get_cfg();

	mipi_queue[0] = value;

	IRIS_LOGI("%s, value: 0x%02x 0x%02x", __func__, mipi_queue[0], mipi_queue[2]);

	rc = iris_dsi_send_cmds(pcfg->panel, cmdset.cmds, cmdset.count,
							cmdset.state, pcfg->vc_ctrl.to_iris_vc_id);

	return rc;
}

/*== Low Power debug related ==*/

static ssize_t _iris_abyp_dbg_write(struct file *file,
		const char __user *buff, size_t count, loff_t *ppos)
{
	unsigned long val;
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	if (kstrtoul_from_user(buff, count, 0, &val))
		return -EFAULT;

	if (!mutex_trylock(&pcfg->panel->panel_lock))
		return -EFAULT;

	if (val == 0) {
		mutex_lock(&pcfg->abyp_ctrl.abypass_mutex);
		iris_lp_abyp_exit();
		mutex_unlock(&pcfg->abyp_ctrl.abypass_mutex);
	} else if (val == 1) {
		mutex_lock(&pcfg->abyp_ctrl.abypass_mutex);
		iris_lp_abyp_enter();
		mutex_unlock(&pcfg->abyp_ctrl.abypass_mutex);
	} else if (val >= 11 && val <= 19) {
		IRIS_LOGI("%s one wired %d", __func__, (int)(val - 11));
		iris_send_one_wired_cmd((int)(val - 11));
	} else if (val == 20) {
		_iris_send_grcp_abyp(false);
		IRIS_LOGI("grcp abyp->pt");
	} else if (val == 21) {
		_iris_send_grcp_abyp(true);
		IRIS_LOGI("grcp pt->abyp");
	} else if (val == 23) {
		iris_lightup(pcfg->panel);
		IRIS_LOGI("lightup Iris abyp_panel_cmds");
	} else if (val == 24) {
		iris_abyp_switch_proc(pcfg->display, PASS_THROUGH_MODE);
	} else if (val == 25) {
		iris_abyp_switch_proc(pcfg->display, ANALOG_BYPASS_MODE);
	}
	mutex_unlock(&pcfg->panel->panel_lock);

	return count;
}

static ssize_t _iris_abyp_dbg_read(struct file *file, char __user *buff,
								  size_t count, loff_t *ppos)
{
	int tot = 0;
	char bp[512];
	int buf_len;
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	buf_len = sizeof(bp);

	if (*ppos)
		return 0;

	tot = scnprintf(bp, buf_len,
					"abyp status gpio: %d\n", iris_check_abyp_ready());
	tot += scnprintf(bp + tot, buf_len - tot,
					 "abyp mode: %d\n", pcfg->abyp_ctrl.abypass_mode);
	tot += scnprintf(bp + tot, buf_len - tot,
					 "abyp lp: %d\n", pcfg->lp_ctrl.abyp_lp);
	if (copy_to_user(buff, bp, tot))
		return -EFAULT;
	*ppos += tot;

	return tot;
}

static ssize_t _iris_lp_dbg_write(struct file *file,
								 const char __user *buff, size_t count, loff_t *ppos)
{
	unsigned long val;
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	if (kstrtoul_from_user(buff, count, 0, &val))
		return -EFAULT;

	if (!mutex_trylock(&pcfg->panel->panel_lock))
		return -EFAULT;

	if (val == 0) {
		iris_dynamic_power_set(false, false);
		iris_ulps_enable(false, false);
		IRIS_LOGI("disable dynamic & ulps low power.");
	} else if (val == 1) {
		iris_dynamic_power_set(true, false);
		iris_ulps_enable(true, false);
		IRIS_LOGI("enable dynamic & ulps low power.");
	} else if (val == 2) {
		IRIS_LOGI("dynamic power: %d", iris_dynamic_power_get());
		IRIS_LOGI("abyp_lp: %u", pcfg->lp_ctrl.abyp_lp);
		IRIS_LOGI("ulps enable: %d", iris_ulps_enable_get());
	} else if (val == 3) {
		pcfg->lp_ctrl.abyp_lp = 1;
		IRIS_LOGI("set abyp_lp: %d", pcfg->lp_ctrl.abyp_lp);
	} else if (val == 4) {
		pcfg->lp_ctrl.abyp_lp = 2;
		IRIS_LOGI("set abyp_lp: %d", pcfg->lp_ctrl.abyp_lp);
	} else if (val == 11) {
		iris_pmu_mipi2_set(true);
	} else if (val == 12) {
		iris_pmu_mipi2_set(false);
	} else if (val == 13) {
		iris_pmu_bsram_set(true);
	} else if (val == 14) {
		iris_pmu_bsram_set(false);
	} else if (val == 15) {
		iris_pmu_frc_set(true);
	} else if (val == 16) {
		iris_pmu_frc_set(false);
	} else if (val == 17) {
		iris_pmu_dscu_set(true);
	} else if (val == 18) {
		iris_pmu_dscu_set(false);
	} else if (val == 19) {
		_iris_flfp_set(true, false);
	} else if (val == 20) {
		_iris_flfp_set(false, false);
	} else if (val == 21) {
		iris_dma_gen_ctrl(_debug_lp_opt & 0xf, (_debug_lp_opt >> 4) & 0xf, 0);
		iris_update_pq_opt(PATH_DSI, true);
	} else if (val == 22) {
		iris_dma_trig(_debug_lp_opt, 0);
		iris_update_pq_opt(PATH_DSI, true);
	} else if (val == 23) {
		_iris_mipi_queue(1);
	} else if (val == 24) {
		_iris_mipi_queue(2);
	} else if (val == 255) {
		IRIS_LOGI("lp debug usages:");
		IRIS_LOGI("0  -- disable dynamic & ulps low power.");
		IRIS_LOGI("1  -- enable dynamic & ulps low power.");
		IRIS_LOGI("2  -- show low power flag.");
		IRIS_LOGI("3  -- enable abyp.");
		IRIS_LOGI("4  -- disable abyp.");
		IRIS_LOGI("11 -- enable mipi2 power.");
		IRIS_LOGI("12 -- disable mipi2 power.");
		IRIS_LOGI("13 -- enable bsram power.");
		IRIS_LOGI("14 -- disable bram power.");
		IRIS_LOGI("15 -- enable frc power.");
		IRIS_LOGI("16 -- disable frc power.");
		IRIS_LOGI("17 -- enable dsc unit power.");
		IRIS_LOGI("18 -- disable dsc unit power.");
		IRIS_LOGI("19 -- enable flfp.");
        IRIS_LOGI("20 -- disable flfp.");
		IRIS_LOGI("21 -- dma gen ctrl.");
		IRIS_LOGI("22 -- dma trig ctrl.");
		IRIS_LOGI("255 -- show debug usages.");
	}
	mutex_unlock(&pcfg->panel->panel_lock);
	return count;
}

static ssize_t _iris_lp_dbg_read(struct file *file, char __user *buff,
								 size_t count, loff_t *ppos)
{
	int tot = 0;
	char bp[512];
	int buf_len;
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	buf_len = sizeof(bp);

	if (*ppos)
		return 0;

	IRIS_LOGI("dynamic power: %d", iris_dynamic_power_get());
	IRIS_LOGI("abyp lp: %d", pcfg->lp_ctrl.abyp_lp);
	IRIS_LOGI("ulps enable: %d", iris_ulps_enable_get());

	tot = scnprintf(bp, buf_len,
					"dpg: %d\n", iris_dynamic_power_get());
	tot += scnprintf(bp + tot, buf_len - tot,
					 "ulps enable: %d\n", iris_ulps_enable_get());
	tot += scnprintf(bp + tot, buf_len - tot,
					 "abyp lp: %d\n", pcfg->lp_ctrl.abyp_lp);
	if (copy_to_user(buff, bp, tot))
		return -EFAULT;
	*ppos += tot;

	return tot;
}

static ssize_t _iris_esd_dbg_write(struct file *file,
								 const char __user *buff, size_t count, loff_t *ppos)
{
	unsigned long val;
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	if (kstrtoul_from_user(buff, count, 0, &val))
		return -EFAULT;

	if (!mutex_trylock(&pcfg->panel->panel_lock))
		return -EFAULT;

	if (val >= 0 && val <= 128) {
		pcfg->lp_ctrl.esd_ctrl = val;
	} else if (val == 200) {
		IRIS_LOGI("clear ESD count!");
		pcfg->lp_ctrl.esd_cnt_iris = 0;
		pcfg->lp_ctrl.esd_cnt_panel = 0;
	}
	mutex_unlock(&pcfg->panel->panel_lock);
	return count;
}

static ssize_t _iris_esd_dbg_read(struct file *file, char __user *buff,
								 size_t count, loff_t *ppos)
{
	int tot = 0;
	char bp[512];
	int buf_len;
	struct iris_cfg *pcfg;

	pcfg = iris_get_cfg();

	buf_len = sizeof(bp);

	if (*ppos)
		return 0;

	IRIS_LOGI("esd ctrl %d, esd cnt: iris %d panel %d", pcfg->lp_ctrl.esd_ctrl,
			pcfg->lp_ctrl.esd_cnt_iris, pcfg->lp_ctrl.esd_cnt_panel);
	IRIS_LOGI("abyp lp: %d", pcfg->lp_ctrl.abyp_lp);
	IRIS_LOGI("ulps enable: %d", iris_ulps_enable_get());

	tot = scnprintf(bp, buf_len,
					"esd ctrl: %d\n", pcfg->lp_ctrl.esd_ctrl);
	tot += scnprintf(bp + tot, buf_len - tot,
					"iris esd cnt: %d\n", pcfg->lp_ctrl.esd_cnt_iris);
	tot += scnprintf(bp + tot, buf_len - tot,
					"panel esd cnt: %d\n", pcfg->lp_ctrl.esd_cnt_panel);
	if (copy_to_user(buff, bp, tot))
		return -EFAULT;
	*ppos += tot;

	return tot;
}

int iris_dbgfs_lp_init(struct dsi_display *display)
{
	struct iris_cfg *pcfg;
	static const struct file_operations iris_abyp_dbg_fops = {
		.open = simple_open,
		.write = _iris_abyp_dbg_write,
		.read = _iris_abyp_dbg_read,
	};

	static const struct file_operations iris_lp_dbg_fops = {
		.open = simple_open,
		.write = _iris_lp_dbg_write,
		.read = _iris_lp_dbg_read,
	};

	static const struct file_operations iris_esd_dbg_fops = {
		.open = simple_open,
		.write = _iris_esd_dbg_write,
		.read = _iris_esd_dbg_read,
	};

	pcfg = iris_get_cfg();

	if (pcfg->dbg_root == NULL) {
		pcfg->dbg_root = debugfs_create_dir("iris", NULL);
		if (IS_ERR_OR_NULL(pcfg->dbg_root)) {
			IRIS_LOGE("debugfs_create_dir for iris_debug failed, error %ld",
					PTR_ERR(pcfg->dbg_root));
			return -ENODEV;
		}
	}

	debugfs_create_u32("lp_opt", 0644, pcfg->dbg_root,
			(u32 *)&_debug_lp_opt);

	debugfs_create_u32("abyp_opt", 0644, pcfg->dbg_root,
			(u32 *)&_debug_on_opt);

	debugfs_create_u8("vc_enable", 0644, pcfg->dbg_root,
					  (u8 *)&(pcfg->vc_ctrl.vc_enable));

	debugfs_create_u32("trace", 0644, pcfg->dbg_root,
			(u32 *)&debug_trace_opt);

	if (debugfs_create_file("abyp", 0644, pcfg->dbg_root, display,
				&iris_abyp_dbg_fops) == NULL) {
		IRIS_LOGE("%s(%d): debugfs_create_file: index fail",
				__FILE__, __LINE__);
		return -EFAULT;
	}

	if (debugfs_create_file("lp", 0644, pcfg->dbg_root, display,
				&iris_lp_dbg_fops) == NULL) {
		IRIS_LOGE("%s(%d): debugfs_create_file: index fail",
				__FILE__, __LINE__);
		return -EFAULT;
	}

	if (debugfs_create_file("esd", 0644, pcfg->dbg_root, display,
				&iris_esd_dbg_fops) == NULL) {
		IRIS_LOGE("%s(%d): debugfs_create_file: index fail",
				__FILE__, __LINE__);
		return -EFAULT;
	}

	return 0;
}
