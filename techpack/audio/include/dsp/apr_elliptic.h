#pragma once

#include <linux/types.h>
#include <dsp/apr_audio-v2.h>
#include <elliptic/elliptic_data_io.h>
#include <linux/delay.h>

#define ELLIPTIC_SET_PARAMS_SIZE			114
#define ELLIPTIC_ULTRASOUND_MODULE_TX			0x0F010201
#define ELLIPTIC_ULTRASOUND_MODULE_RX			0x0FF10202
#define ULTRASOUND_OPCODE				0x0FF10204

/* fix: original value SLIMBUS_2_TX is wrong for Bengal/SM4350 — SLIMBUS
 * does not exist on this platform.
 *
 * The correct port is AFE_PORT_ID_PSEUDOPORT_01 (0x8001). Evidence from patch:
 *   1. elliptic_data_msm_io.c defines AFE_MSM_RX_PSEUDOPORT_ID = 0x8001,
 *      which is the port opened via afe_start_pseudo_port() — same value as
 *      AFE_PORT_ID_PSEUDOPORT_01 defined in apr_audio-v2.h.
 *   2. q6audio-v2.c: q6audio_validate_port() explicitly whitelists
 *      AFE_PORT_ID_PSEUDOPORT_01 under CONFIG_AUDIO_ELLIPTIC_ULTRASOUND.
 *      SLIMBUS_2_TX is NOT in that whitelist — every afe_set_parameter()
 *      call would silently return -EINVAL, breaking all DSP param writes.
 *   3. q6audio-v2.c: q6audio_get_port_index() maps AFE_PORT_ID_PSEUDOPORT_01
 *      to IDX_AFE_PORT_ID_PSEUDOPORT_01 (newly added enum in q6afe-v2.h).
 *      No mapping exists for SLIMBUS_2_TX — index returns -1 — OOB on ptr_wait[]. */
#define ELLIPTIC_PORT_ID				AFE_PORT_ID_PSEUDOPORT_01

/** Sequence of Elliptic Labs Ultrasound module parameters */
struct afe_ultrasound_set_params_t {
	uint32_t payload[ELLIPTIC_SET_PARAMS_SIZE];
} __packed;

/** Elliptic APR public */

int32_t ultrasound_apr_set_parameter(int32_t port_id, uint32_t param_id,
	u8 *user_params, int32_t length);

int32_t elliptic_process_apr_payload(uint32_t *payload);

int elliptic_notify_gain_change_msg(int component_id, int gaindb);

struct afe_ultrasound_state {
	atomic_t us_apr_state;
	void **ptr_apr;
	atomic_t *ptr_status;
	atomic_t *ptr_state;
	wait_queue_head_t *ptr_wait;
	struct mutex *ptr_afe_apr_lock;
	int timeout_ms;
};
typedef struct afe_ultrasound_state afe_ultrasound_state_t;

extern afe_ultrasound_state_t elus_afe;
