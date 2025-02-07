/* Spa
 *
 * Copyright © 2022 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#include <spa/support/plugin.h>
#include <spa/support/cpu.h>
#include <spa/support/log.h>
#include <spa/utils/result.h>
#include <spa/utils/list.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/node/keys.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/filter.h>
#include <spa/debug/types.h>
#include <spa/debug/pod.h>

#include "volume-ops.h"
#include "fmt-ops.h"
#include "channelmix-ops.h"
#include "resample.h"

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT log_topic
static struct spa_log_topic *log_topic = &SPA_LOG_TOPIC(0, "spa.audioconvert");

#define DEFAULT_RATE		48000
#define DEFAULT_CHANNELS	2

#define MAX_ALIGN	FMT_OPS_MAX_ALIGN
#define MAX_BUFFERS	32
#define MAX_DATAS	SPA_AUDIO_MAX_CHANNELS
#define MAX_PORTS	(SPA_AUDIO_MAX_CHANNELS+1)

#define DEFAULT_MUTE	false
#define DEFAULT_VOLUME	VOLUME_NORM

struct volumes {
	bool mute;
	uint32_t n_volumes;
	float volumes[SPA_AUDIO_MAX_CHANNELS];
};

static void init_volumes(struct volumes *vol)
{
	uint32_t i;
	vol->mute = DEFAULT_MUTE;
	vol->n_volumes = 0;
	for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++)
		vol->volumes[i] = DEFAULT_VOLUME;
}

struct props {
	float volume;
	uint32_t n_channels;
	uint32_t channel_map[SPA_AUDIO_MAX_CHANNELS];
	struct volumes channel;
	struct volumes soft;
	struct volumes monitor;
	unsigned int have_soft_volume:1;
	unsigned int mix_disabled:1;
	unsigned int resample_quality;
	unsigned int resample_disabled:1;
	double rate;
};

static void props_reset(struct props *props)
{
	uint32_t i;
	props->volume = DEFAULT_VOLUME;
	props->n_channels = 0;
	for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++)
		props->channel_map[i] = SPA_AUDIO_CHANNEL_UNKNOWN;
	init_volumes(&props->channel);
	init_volumes(&props->soft);
	init_volumes(&props->monitor);
	props->mix_disabled = false;
	props->rate = 1.0;
	props->resample_quality = RESAMPLE_DEFAULT_QUALITY;
	props->resample_disabled = false;
}

struct buffer {
	uint32_t id;
#define BUFFER_FLAG_QUEUED	(1<<0)
	uint32_t flags;
	struct spa_list link;
	struct spa_buffer *buf;
	void *datas[MAX_DATAS];
};

struct port {
	uint32_t direction;
	uint32_t id;

	struct spa_io_buffers *io;

	uint64_t info_all;
	struct spa_port_info info;
#define IDX_EnumFormat	0
#define IDX_Meta	1
#define IDX_IO		2
#define IDX_Format	3
#define IDX_Buffers	4
#define IDX_Latency	5
#define N_PORT_PARAMS	6
	struct spa_param_info params[N_PORT_PARAMS];
	char position[16];

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_audio_info format;
	unsigned int have_format:1;
	unsigned int is_dsp:1;
	unsigned int is_monitor:1;
	unsigned int is_control:1;

	uint32_t blocks;
	uint32_t stride;

	const struct spa_pod_sequence *ctrl;
	uint32_t ctrl_offset;

	struct spa_list queue;
};

struct dir {
	struct port *ports[MAX_PORTS];
	uint32_t n_ports;

	enum spa_param_port_config_mode mode;

	struct spa_audio_info format;
	unsigned int have_format:1;
	unsigned int have_profile:1;
	struct spa_latency_info latency;

	uint32_t remap[MAX_PORTS];

	struct convert conv;
	unsigned int need_remap:1;
	unsigned int is_passthrough:1;
	unsigned int control:1;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_cpu *cpu;

	uint32_t cpu_flags;
	uint32_t max_align;
	uint32_t quantum_limit;
	enum spa_direction direction;

	struct props props;

	struct spa_io_position *io_position;
	struct spa_io_rate_match *io_rate_match;

	uint64_t info_all;
	struct spa_node_info info;
#define IDX_EnumPortConfig	0
#define IDX_PortConfig		1
#define IDX_PropInfo		2
#define IDX_Props		3
#define N_NODE_PARAMS		4
	struct spa_param_info params[N_NODE_PARAMS];

	struct spa_hook_list hooks;

	unsigned int monitor:1;
	unsigned int monitor_channel_volumes:1;

	struct dir dir[2];
	struct channelmix mix;
	struct resample resample;
	struct volume volume;
	double rate_scale;

	uint32_t in_offset;
	uint32_t out_offset;
	unsigned int started:1;
	unsigned int peaks:1;
	unsigned int is_passthrough:1;
	unsigned int drained:1;

	uint32_t empty_size;
	float *empty;
	float *scratch;
	float *tmp[2];
	float *tmp_datas[2][MAX_PORTS];
};

#define CHECK_PORT(this,d,p)		((p) < this->dir[d].n_ports)
#define GET_PORT(this,d,p)		(this->dir[d].ports[p])
#define GET_IN_PORT(this,p)		GET_PORT(this,SPA_DIRECTION_INPUT,p)
#define GET_OUT_PORT(this,p)		GET_PORT(this,SPA_DIRECTION_OUTPUT,p)

#define PORT_IS_DSP(this,d,p)		(GET_PORT(this,d,p)->is_dsp)
#define PORT_IS_CONTROL(this,d,p)	(GET_PORT(this,d,p)->is_control)

static void set_volume(struct impl *this);

static void emit_node_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;
	uint32_t i;

	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		if (this->info.change_mask & SPA_NODE_CHANGE_MASK_PARAMS) {
			for (i = 0; i < SPA_N_ELEMENTS(this->params); i++) {
				if (this->params[i].user > 0) {
					this->params[i].flags ^= SPA_PARAM_INFO_SERIAL;
					this->params[i].user = 0;
				}
			}
		}
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
}

static void emit_port_info(struct impl *this, struct port *port, bool full)
{
	uint64_t old = full ? port->info.change_mask : 0;
	uint32_t i;

	if (full)
		port->info.change_mask = port->info_all;
	if (port->info.change_mask) {
		struct spa_dict_item items[3];
		uint32_t n_items = 0;

		if (PORT_IS_DSP(this, port->direction, port->id)) {
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_FORMAT_DSP, "32 bit float mono audio");
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_AUDIO_CHANNEL, port->position);
			if (port->is_monitor)
				items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_PORT_MONITOR, "true");
		} else if (PORT_IS_CONTROL(this, port->direction, port->id)) {
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_PORT_NAME, "control");
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_FORMAT_DSP, "8 bit raw midi");
		}
		port->info.props = &SPA_DICT_INIT(items, n_items);

		if (port->info.change_mask & SPA_PORT_CHANGE_MASK_PARAMS) {
			for (i = 0; i < SPA_N_ELEMENTS(port->params); i++) {
				if (port->params[i].user > 0) {
					port->params[i].flags ^= SPA_PARAM_INFO_SERIAL;
					port->params[i].user = 0;
				}
			}
		}
		spa_node_emit_port_info(&this->hooks, port->direction, port->id, &port->info);
		port->info.change_mask = old;
	}
}

static int init_port(struct impl *this, enum spa_direction direction, uint32_t port_id,
		uint32_t position, bool is_dsp, bool is_monitor, bool is_control)
{
	struct port *port = GET_PORT(this, direction, port_id);
	const char *name;

	spa_assert(port_id < MAX_PORTS);

	if (port == NULL) {
		port = calloc(1, sizeof(struct port));
		if (port == NULL)
			return -errno;
		this->dir[direction].ports[port_id] = port;
	}
	port->direction = direction;
	port->id = port_id;

	name = spa_debug_type_find_short_name(spa_type_audio_channel, position);
	snprintf(port->position, sizeof(port->position), "%s", name ? name : "UNK");

	port->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PROPS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	port->info = SPA_PORT_INFO_INIT();
	port->info.flags = SPA_PORT_FLAG_NO_REF |
		SPA_PORT_FLAG_DYNAMIC_DATA;
	port->params[IDX_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[IDX_Meta] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	port->params[IDX_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	port->params[IDX_Latency] = SPA_PARAM_INFO(SPA_PARAM_Latency, SPA_PARAM_INFO_READWRITE);
	port->info.params = port->params;
	port->info.n_params = N_PORT_PARAMS;

	port->n_buffers = 0;
	port->have_format = false;
	port->is_monitor = is_monitor;
	port->is_dsp = is_dsp;
	if (port->is_dsp) {
		port->format.media_type = SPA_MEDIA_TYPE_audio;
		port->format.media_subtype = SPA_MEDIA_SUBTYPE_dsp;
		port->format.info.dsp.format = SPA_AUDIO_FORMAT_DSP_F32;
		port->blocks = 1;
		port->stride = 4;
	}
	port->is_control = is_control;
	if (port->is_control) {
		port->format.media_type = SPA_MEDIA_TYPE_application;
		port->format.media_subtype = SPA_MEDIA_SUBTYPE_control;
		port->blocks = 1;
		port->stride = 1;
	}
	spa_list_init(&port->queue);

	spa_log_info(this->log, "%p: add port %d:%d position:%s %d %d %d",
			this, direction, port_id, port->position, is_dsp, is_monitor, is_control);
	emit_port_info(this, port, true);

	return 0;
}

static int impl_node_enum_params(void *object, int seq,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[4096];
	struct spa_result_node_params result;
	uint32_t count = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumPortConfig:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamPortConfig, id,
				SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(SPA_DIRECTION_INPUT),
				SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamPortConfig, id,
				SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(SPA_DIRECTION_OUTPUT),
				SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp));
			break;
		case 2:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamPortConfig, id,
				SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(SPA_DIRECTION_INPUT),
				SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_convert));
			break;
		case 3:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamPortConfig, id,
				SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(SPA_DIRECTION_OUTPUT),
				SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_convert));
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_PortConfig:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamPortConfig, id,
				SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(SPA_DIRECTION_INPUT),
				SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_Id(this->dir[SPA_DIRECTION_INPUT].mode));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamPortConfig, id,
				SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(SPA_DIRECTION_OUTPUT),
				SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_Id(this->dir[SPA_DIRECTION_OUTPUT].mode));
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_PropInfo:
	{
		struct props *p = &this->props;
		struct spa_pod_frame f[2];
		uint32_t i;

		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_volume),
				SPA_PROP_INFO_description, SPA_POD_String("Volume"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(p->volume, 0.0, 10.0));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_mute),
				SPA_PROP_INFO_description, SPA_POD_String("Mute"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(p->channel.mute));
			break;
		case 2:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_channelVolumes),
				SPA_PROP_INFO_description, SPA_POD_String("Channel Volumes"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(p->volume, 0.0, 10.0),
				SPA_PROP_INFO_container, SPA_POD_Id(SPA_TYPE_Array));
			break;
		case 3:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_channelMap),
				SPA_PROP_INFO_description, SPA_POD_String("Channel Map"),
				SPA_PROP_INFO_type, SPA_POD_Id(SPA_AUDIO_CHANNEL_UNKNOWN),
				SPA_PROP_INFO_container, SPA_POD_Id(SPA_TYPE_Array));
			break;
		case 4:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_monitorMute),
				SPA_PROP_INFO_description, SPA_POD_String("Monitor Mute"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(p->monitor.mute));
			break;
		case 5:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_monitorVolumes),
				SPA_PROP_INFO_description, SPA_POD_String("Monitor Volumes"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(p->volume, 0.0, 10.0),
				SPA_PROP_INFO_container, SPA_POD_Id(SPA_TYPE_Array));
			break;
		case 6:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_softMute),
				SPA_PROP_INFO_description, SPA_POD_String("Soft Mute"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(p->soft.mute));
			break;
		case 7:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_softVolumes),
				SPA_PROP_INFO_description, SPA_POD_String("Soft Volumes"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(p->volume, 0.0, 10.0),
				SPA_PROP_INFO_container, SPA_POD_Id(SPA_TYPE_Array));
			break;
		case 8:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("monitor.channel-volumes"),
				SPA_PROP_INFO_description, SPA_POD_String("Monitor channel volume"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(
					this->monitor_channel_volumes),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 9:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.disable"),
				SPA_PROP_INFO_description, SPA_POD_String("Disable Channel mixing"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(p->mix_disabled),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 10:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.normalize"),
				SPA_PROP_INFO_description, SPA_POD_String("Normalize Volumes"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(
					SPA_FLAG_IS_SET(this->mix.options, CHANNELMIX_OPTION_NORMALIZE)),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 11:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.mix-lfe"),
				SPA_PROP_INFO_description, SPA_POD_String("Mix LFE into channels"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(
					SPA_FLAG_IS_SET(this->mix.options, CHANNELMIX_OPTION_MIX_LFE)),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 12:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.upmix"),
				SPA_PROP_INFO_description, SPA_POD_String("Enable upmixing"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(
					SPA_FLAG_IS_SET(this->mix.options, CHANNELMIX_OPTION_UPMIX)),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 13:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.lfe-cutoff"),
				SPA_PROP_INFO_description, SPA_POD_String("LFE cutoff frequency"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(
					this->mix.lfe_cutoff, 0.0, 1000.0),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 14:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.fc-cutoff"),
				SPA_PROP_INFO_description, SPA_POD_String("FC cutoff frequency (Hz)"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(
					this->mix.fc_cutoff, 0.0, 48000.0),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 15:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.rear-delay"),
				SPA_PROP_INFO_description, SPA_POD_String("Rear channels delay (ms)"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(
					this->mix.rear_delay, 0.0, 1000.0),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 16:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.stereo-widen"),
				SPA_PROP_INFO_description, SPA_POD_String("Stereo widen"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(
					this->mix.widen, 0.0, 1.0),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 17:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.hilbert-taps"),
				SPA_PROP_INFO_description, SPA_POD_String("Taps for phase shift of rear"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(
					this->mix.hilbert_taps, 0, MAX_TAPS),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 18:
			spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_PropInfo, id);
			spa_pod_builder_add(&b,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.upmix-method"),
				SPA_PROP_INFO_description, SPA_POD_String("Upmix method to use"),
				SPA_PROP_INFO_type, SPA_POD_String(
					channelmix_upmix_info[this->mix.upmix].label),
				SPA_PROP_INFO_params, SPA_POD_Bool(true),
				0);

			spa_pod_builder_prop(&b, SPA_PROP_INFO_labels, 0);
			spa_pod_builder_push_struct(&b, &f[1]);
			for (i = 0; i < SPA_N_ELEMENTS(channelmix_upmix_info); i++) {
				spa_pod_builder_string(&b, channelmix_upmix_info[i].label);
				spa_pod_builder_string(&b, channelmix_upmix_info[i].description);
			}
			spa_pod_builder_pop(&b, &f[1]);
			param = spa_pod_builder_pop(&b, &f[0]);
			break;
		case 19:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id, SPA_POD_Id(SPA_PROP_rate),
				SPA_PROP_INFO_description, SPA_POD_String("Rate scaler"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Double(p->rate, 0.0, 10.0));
			break;
		case 20:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id, SPA_POD_Id(SPA_PROP_quality),
				SPA_PROP_INFO_name, SPA_POD_String("resample.quality"),
				SPA_PROP_INFO_description, SPA_POD_String("Resample Quality"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(p->resample_quality, 0, 14),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 21:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("resample.disable"),
				SPA_PROP_INFO_description, SPA_POD_String("Disable Resampling"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(p->resample_disabled),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 22:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("dither.noise"),
				SPA_PROP_INFO_description, SPA_POD_String("Add noise bits"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(this->dir[1].conv.noise_bits, 0, 16),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 23:
			spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_PropInfo, id);
			spa_pod_builder_add(&b,
				SPA_PROP_INFO_name, SPA_POD_String("dither.method"),
				SPA_PROP_INFO_description, SPA_POD_String("The dithering method"),
				SPA_PROP_INFO_type, SPA_POD_String(
					dither_method_info[this->dir[1].conv.method].label),
				SPA_PROP_INFO_params, SPA_POD_Bool(true),
				0);
			spa_pod_builder_prop(&b, SPA_PROP_INFO_labels, 0);
			spa_pod_builder_push_struct(&b, &f[1]);
			for (i = 0; i < SPA_N_ELEMENTS(dither_method_info); i++) {
				spa_pod_builder_string(&b, dither_method_info[i].label);
				spa_pod_builder_string(&b, dither_method_info[i].description);
			}
			spa_pod_builder_pop(&b, &f[1]);
			param = spa_pod_builder_pop(&b, &f[0]);
			break;
		default:
			return 0;
		}
		break;
	}

	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;
		struct spa_pod_frame f[2];

		switch (result.index) {
		case 0:
			spa_pod_builder_push_object(&b, &f[0],
                                SPA_TYPE_OBJECT_Props, id);
			spa_pod_builder_add(&b,
				SPA_PROP_volume,		SPA_POD_Float(p->volume),
				SPA_PROP_mute,			SPA_POD_Bool(p->channel.mute),
				SPA_PROP_channelVolumes,	SPA_POD_Array(sizeof(float),
									SPA_TYPE_Float,
									p->channel.n_volumes,
									p->channel.volumes),
				SPA_PROP_channelMap,		SPA_POD_Array(sizeof(uint32_t),
									SPA_TYPE_Id,
									p->n_channels,
									p->channel_map),
				SPA_PROP_softMute,		SPA_POD_Bool(p->soft.mute),
				SPA_PROP_softVolumes,		SPA_POD_Array(sizeof(float),
									SPA_TYPE_Float,
									p->soft.n_volumes,
									p->soft.volumes),
				SPA_PROP_monitorMute,		SPA_POD_Bool(p->monitor.mute),
				SPA_PROP_monitorVolumes,	SPA_POD_Array(sizeof(float),
									SPA_TYPE_Float,
									p->monitor.n_volumes,
									p->monitor.volumes),
				0);
			spa_pod_builder_prop(&b, SPA_PROP_params, 0);
			spa_pod_builder_push_struct(&b, &f[1]);
			spa_pod_builder_string(&b, "monitor.channel-volumes");
			spa_pod_builder_bool(&b, this->monitor_channel_volumes);
			spa_pod_builder_string(&b, "channelmix.disable");
			spa_pod_builder_bool(&b, this->props.mix_disabled);
			spa_pod_builder_string(&b, "channelmix.normalize");
			spa_pod_builder_bool(&b, SPA_FLAG_IS_SET(this->mix.options,
						CHANNELMIX_OPTION_NORMALIZE));
			spa_pod_builder_string(&b, "channelmix.mix-lfe");
			spa_pod_builder_bool(&b, SPA_FLAG_IS_SET(this->mix.options,
						CHANNELMIX_OPTION_MIX_LFE));
			spa_pod_builder_string(&b, "channelmix.upmix");
			spa_pod_builder_bool(&b, SPA_FLAG_IS_SET(this->mix.options,
						CHANNELMIX_OPTION_UPMIX));
			spa_pod_builder_string(&b, "channelmix.lfe-cutoff");
			spa_pod_builder_float(&b, this->mix.lfe_cutoff);
			spa_pod_builder_string(&b, "channelmix.fc-cutoff");
			spa_pod_builder_float(&b, this->mix.fc_cutoff);
			spa_pod_builder_string(&b, "channelmix.rear-delay");
			spa_pod_builder_float(&b, this->mix.rear_delay);
			spa_pod_builder_string(&b, "channelmix.stereo-widen");
			spa_pod_builder_float(&b, this->mix.widen);
			spa_pod_builder_string(&b, "channelmix.hilbert-taps");
			spa_pod_builder_int(&b, this->mix.hilbert_taps);
			spa_pod_builder_string(&b, "channelmix.upmix-method");
			spa_pod_builder_string(&b, channelmix_upmix_info[this->mix.upmix].label);
			spa_pod_builder_string(&b, "resample.quality");
			spa_pod_builder_int(&b, p->resample_quality);
			spa_pod_builder_string(&b, "resample.disable");
			spa_pod_builder_bool(&b, p->resample_disabled);
			spa_pod_builder_string(&b, "dither.noise");
			spa_pod_builder_int(&b, this->dir[1].conv.noise_bits);
			spa_pod_builder_string(&b, "dither.method");
			spa_pod_builder_string(&b, dither_method_info[this->dir[1].conv.method].label);
			spa_pod_builder_pop(&b, &f[1]);
			param = spa_pod_builder_pop(&b, &f[0]);
			break;
		default:
			return 0;
		}
		break;
	}
	default:
		return 0;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, "%p: io %d %p/%zd", this, id, data, size);

	switch (id) {
	case SPA_IO_Position:
		this->io_position = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int audioconvert_set_param(struct impl *this, const char *k, const char *s)
{
	if (spa_streq(k, "monitor.channel-volumes"))
		this->monitor_channel_volumes = spa_atob(s);
	else if (spa_streq(k, "channelmix.disable"))
		this->props.mix_disabled = spa_atob(s);
	else if (spa_streq(k, "channelmix.normalize"))
		SPA_FLAG_UPDATE(this->mix.options, CHANNELMIX_OPTION_NORMALIZE, spa_atob(s));
	else if (spa_streq(k, "channelmix.mix-lfe"))
		SPA_FLAG_UPDATE(this->mix.options, CHANNELMIX_OPTION_MIX_LFE, spa_atob(s));
	else if (spa_streq(k, "channelmix.upmix"))
		SPA_FLAG_UPDATE(this->mix.options, CHANNELMIX_OPTION_UPMIX, spa_atob(s));
	else if (spa_streq(k, "channelmix.lfe-cutoff"))
		spa_atof(s, &this->mix.lfe_cutoff);
	else if (spa_streq(k, "channelmix.fc-cutoff"))
		spa_atof(s, &this->mix.fc_cutoff);
	else if (spa_streq(k, "channelmix.rear-delay"))
		spa_atof(s, &this->mix.rear_delay);
	else if (spa_streq(k, "channelmix.stereo-widen"))
		spa_atof(s, &this->mix.widen);
	else if (spa_streq(k, "channelmix.hilbert-taps"))
		spa_atou32(s, &this->mix.hilbert_taps, 0);
	else if (spa_streq(k, "channelmix.upmix-method"))
		this->mix.upmix = channelmix_upmix_from_label(s);
	else if (spa_streq(k, "resample.quality"))
		this->props.resample_quality = atoi(s);
	else if (spa_streq(k, "resample.disable"))
		this->props.resample_disabled = spa_atob(s);
	else if (spa_streq(k, "dither.noise"))
		spa_atou32(s, &this->dir[1].conv.noise_bits, 0);
	else if (spa_streq(k, "dither.method"))
		this->dir[1].conv.method = dither_method_from_label(s);
	else
		return 0;
	return 1;
}

static int parse_prop_params(struct impl *this, struct spa_pod *params)
{
	struct spa_pod_parser prs;
	struct spa_pod_frame f;
	int changed = 0;

	spa_pod_parser_pod(&prs, params);
	if (spa_pod_parser_push_struct(&prs, &f) < 0)
		return 0;

	while (true) {
		const char *name;
		struct spa_pod *pod;
		char value[512];

		if (spa_pod_parser_get_string(&prs, &name) < 0)
			break;

		if (spa_pod_parser_get_pod(&prs, &pod) < 0)
			break;

		if (spa_pod_is_string(pod)) {
			spa_pod_copy_string(pod, sizeof(value), value);
		} else if (spa_pod_is_float(pod)) {
			snprintf(value, sizeof(value), "%f",
					SPA_POD_VALUE(struct spa_pod_float, pod));
		} else if (spa_pod_is_int(pod)) {
			snprintf(value, sizeof(value), "%d",
					SPA_POD_VALUE(struct spa_pod_int, pod));
		} else if (spa_pod_is_bool(pod)) {
			snprintf(value, sizeof(value), "%s",
					SPA_POD_VALUE(struct spa_pod_bool, pod) ?
					"true" : "false");
		} else
			continue;

		spa_log_info(this->log, "key:'%s' val:'%s'", name, value);
		changed += audioconvert_set_param(this, name, value);
	}
	if (changed) {
		channelmix_init(&this->mix);
		set_volume(this);
	}
	return changed;
}

static int apply_props(struct impl *this, const struct spa_pod *param)
{
	struct spa_pod_prop *prop;
	struct spa_pod_object *obj = (struct spa_pod_object *) param;
	struct props *p = &this->props;
	bool have_channel_volume = false;
	bool have_soft_volume = false;
	int changed = 0;
	uint32_t n;

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_volume:
			if (spa_pod_get_float(&prop->value, &p->volume) == 0)
				changed++;
			break;
		case SPA_PROP_mute:
			if (spa_pod_get_bool(&prop->value, &p->channel.mute) == 0) {
				have_channel_volume = true;
				changed++;
			}
			break;
		case SPA_PROP_channelVolumes:
			if ((n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					p->channel.volumes, SPA_AUDIO_MAX_CHANNELS)) > 0) {
				have_channel_volume = true;
				p->channel.n_volumes = n;
				changed++;
			}
			break;
		case SPA_PROP_channelMap:
			if ((n = spa_pod_copy_array(&prop->value, SPA_TYPE_Id,
					p->channel_map, SPA_AUDIO_MAX_CHANNELS)) > 0) {
				p->n_channels = n;
				changed++;
			}
			break;
		case SPA_PROP_softMute:
			if (spa_pod_get_bool(&prop->value, &p->soft.mute) == 0) {
				have_soft_volume = true;
				changed++;
			}
			break;
		case SPA_PROP_softVolumes:
			if ((n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					p->soft.volumes, SPA_AUDIO_MAX_CHANNELS)) > 0) {
				have_soft_volume = true;
				p->soft.n_volumes = n;
				changed++;
			}
			break;
		case SPA_PROP_monitorMute:
			if (spa_pod_get_bool(&prop->value, &p->monitor.mute) == 0)
				changed++;
			break;
		case SPA_PROP_monitorVolumes:
			if ((n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					p->monitor.volumes, SPA_AUDIO_MAX_CHANNELS)) > 0) {
				p->monitor.n_volumes = n;
				changed++;
			}
			break;
		case SPA_PROP_rate:
			if (spa_pod_get_double(&prop->value, &p->rate) == 0)
				changed++;
			break;
		case SPA_PROP_params:
			changed += parse_prop_params(this, &prop->value);
			break;
		default:
			break;
		}
	}
	if (changed) {
		if (have_soft_volume)
			p->have_soft_volume = true;
		else if (have_channel_volume)
			p->have_soft_volume = false;

		set_volume(this);
	}
	return changed;
}

static int apply_midi(struct impl *this, const struct spa_pod *value)
{
	const uint8_t *val = SPA_POD_BODY(value);
	uint32_t size = SPA_POD_BODY_SIZE(value);
	struct props *p = &this->props;

	if (size < 3)
		return -EINVAL;

	if ((val[0] & 0xf0) != 0xb0 || val[1] != 7)
		return 0;

	p->volume = val[2] / 127.0f;
	set_volume(this);
	return 1;
}

static int reconfigure_mode(struct impl *this, enum spa_param_port_config_mode mode,
		enum spa_direction direction, bool monitor, bool control, struct spa_audio_info *info)
{
	struct dir *dir;
	uint32_t i;

	dir = &this->dir[direction];

	if (dir->have_profile && this->monitor == monitor && dir->mode == mode &&
	    dir->control == control &&
	    (info == NULL || memcmp(&dir->format, info, sizeof(*info)) == 0))
		return 0;

	spa_log_info(this->log, "%p: port config direction:%d monitor:%d control:%d mode:%d %d", this,
			direction, monitor, control, mode, dir->n_ports);

	for (i = 0; i < dir->n_ports; i++) {
		spa_node_emit_port_info(&this->hooks, direction, i, NULL);
		if (this->monitor && direction == SPA_DIRECTION_INPUT)
			spa_node_emit_port_info(&this->hooks, SPA_DIRECTION_OUTPUT, i+1, NULL);
	}

	this->monitor = monitor;
	dir->control = control;
	dir->have_profile = true;
	dir->mode = mode;

	switch (mode) {
	case SPA_PARAM_PORT_CONFIG_MODE_dsp:
	{
		if (info) {
			dir->n_ports = info->info.raw.channels;
			dir->format = *info;
			dir->format.info.raw.format = SPA_AUDIO_FORMAT_DSP_F32;
			dir->format.info.raw.rate = 0;
			dir->have_format = true;
		} else {
			dir->n_ports = 0;
		}

		if (this->monitor && direction == SPA_DIRECTION_INPUT)
			this->dir[SPA_DIRECTION_OUTPUT].n_ports = dir->n_ports + 1;

		for (i = 0; i < dir->n_ports; i++) {
			init_port(this, direction, i, info->info.raw.position[i], true, false, false);
			if (this->monitor && direction == SPA_DIRECTION_INPUT)
				init_port(this, SPA_DIRECTION_OUTPUT, i+1,
					info->info.raw.position[i], true, true, false);
		}
		break;
	}
	case SPA_PARAM_PORT_CONFIG_MODE_convert:
	{
		dir->n_ports = 1;
		dir->have_format = false;
		init_port(this, direction, 0, 0, false, false, false);
		break;
	}
	default:
		return -ENOTSUP;
	}
	if (direction == SPA_DIRECTION_INPUT && dir->control) {
		i = dir->n_ports++;
		init_port(this, direction, i, 0, false, false, true);
	}

	this->info.change_mask |= SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PARAMS;
	this->info.flags &= ~SPA_NODE_FLAG_NEED_CONFIGURE;
	this->params[IDX_Props].user++;
	this->params[IDX_PortConfig].user++;
	return 0;
}

static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if (param == NULL)
		return 0;

	switch (id) {
	case SPA_PARAM_PortConfig:
	{
		struct spa_audio_info info = { 0, }, *infop = NULL;
		struct spa_pod *format = NULL;
		enum spa_direction direction;
		enum spa_param_port_config_mode mode;
		bool monitor = false, control = false;
		int res;

		if (spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamPortConfig, NULL,
				SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(&direction),
				SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(&mode),
				SPA_PARAM_PORT_CONFIG_monitor,		SPA_POD_OPT_Bool(&monitor),
				SPA_PARAM_PORT_CONFIG_control,		SPA_POD_OPT_Bool(&control),
				SPA_PARAM_PORT_CONFIG_format,		SPA_POD_OPT_Pod(&format)) < 0)
			return -EINVAL;

		if (format) {
			if (!spa_pod_is_object_type(format, SPA_TYPE_OBJECT_Format))
				return -EINVAL;

			if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
				return res;

			if (info.media_type != SPA_MEDIA_TYPE_audio ||
			    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
				return -EINVAL;

			if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
				return -EINVAL;

			if (info.info.raw.channels > SPA_AUDIO_MAX_CHANNELS)
				return -EINVAL;

			infop = &info;
		}

		if ((res = reconfigure_mode(this, mode, direction, monitor, control, infop)) < 0)
			return res;

		emit_node_info(this, false);
		break;
	}
	case SPA_PARAM_Props:
		if (apply_props(this, param) > 0)
			emit_node_info(this, false);
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int int32_cmp(const void *v1, const void *v2)
{
	int32_t a1 = *(int32_t*)v1;
	int32_t a2 = *(int32_t*)v2;
	if (a1 == 0 && a2 != 0)
		return 1;
	if (a2 == 0 && a1 != 0)
		return -1;
	return a1 - a2;
}

static int setup_in_convert(struct impl *this)
{
	uint32_t i, j;
	struct dir *in = &this->dir[SPA_DIRECTION_INPUT];
	struct spa_audio_info src_info, dst_info;
	int res;
	bool remap = false;

	src_info = in->format;
	dst_info = src_info;
	dst_info.info.raw.format = SPA_AUDIO_FORMAT_DSP_F32;

	spa_log_info(this->log, "%p: %s/%d@%d->%s/%d@%d", this,
			spa_debug_type_find_name(spa_type_audio_format, src_info.info.raw.format),
			src_info.info.raw.channels,
			src_info.info.raw.rate,
			spa_debug_type_find_name(spa_type_audio_format, dst_info.info.raw.format),
			dst_info.info.raw.channels,
			dst_info.info.raw.rate);

	qsort(dst_info.info.raw.position, dst_info.info.raw.channels,
					sizeof(uint32_t), int32_cmp);

	for (i = 0; i < src_info.info.raw.channels; i++) {
		for (j = 0; j < dst_info.info.raw.channels; j++) {
			if (src_info.info.raw.position[i] !=
			    dst_info.info.raw.position[j])
				continue;
			in->remap[i] = j;
			if (i != j)
				remap = true;
			spa_log_debug(this->log, "%p: channel %d (%d) -> %d (%s -> %s)", this,
					i, in->remap[i], j,
					spa_debug_type_find_short_name(spa_type_audio_channel,
						src_info.info.raw.position[i]),
					spa_debug_type_find_short_name(spa_type_audio_channel,
						dst_info.info.raw.position[j]));
			dst_info.info.raw.position[j] = -1;
			break;
		}
	}
	if (in->conv.free)
		convert_free(&in->conv);

	in->conv.src_fmt = src_info.info.raw.format;
	in->conv.dst_fmt = dst_info.info.raw.format;
	in->conv.n_channels = dst_info.info.raw.channels;
	in->conv.cpu_flags = this->cpu_flags;
	in->need_remap = remap;

	if ((res = convert_init(&in->conv)) < 0)
		return res;

	spa_log_debug(this->log, "%p: got converter features %08x:%08x passthrough:%d remap:%d %s", this,
			this->cpu_flags, in->conv.cpu_flags, in->conv.is_passthrough,
			remap, in->conv.func_name);

	return 0;
}

#define _MASK(ch)	(1ULL << SPA_AUDIO_CHANNEL_ ## ch)
#define STEREO	(_MASK(FL)|_MASK(FR))

static uint64_t default_mask(uint32_t channels)
{
	uint64_t mask = 0;
	switch (channels) {
	case 7:
	case 8:
		mask |= _MASK(RL);
		mask |= _MASK(RR);
		SPA_FALLTHROUGH
	case 5:
	case 6:
		mask |= _MASK(SL);
		mask |= _MASK(SR);
		if ((channels & 1) == 0)
			mask |= _MASK(LFE);
		SPA_FALLTHROUGH
	case 3:
		mask |= _MASK(FC);
		SPA_FALLTHROUGH
	case 2:
		mask |= _MASK(FL);
		mask |= _MASK(FR);
		break;
	case 1:
		mask |= _MASK(MONO);
		break;
	case 4:
		mask |= _MASK(FL);
		mask |= _MASK(FR);
		mask |= _MASK(RL);
		mask |= _MASK(RR);
		break;
	}
	return mask;
}

static void fix_volumes(struct impl *this, struct volumes *vols, uint32_t channels)
{
	float s;
	uint32_t i;
	spa_log_debug(this->log, "%p %d -> %d", this, vols->n_volumes, channels);
	if (vols->n_volumes > 0) {
		s = 0.0f;
		for (i = 0; i < vols->n_volumes; i++)
			s += vols->volumes[i];
		s /= vols->n_volumes;
	} else {
		s = 1.0f;
	}
	vols->n_volumes = channels;
	for (i = 0; i < vols->n_volumes; i++)
		vols->volumes[i] = s;
}

static int remap_volumes(struct impl *this, const struct spa_audio_info *info)
{
	struct props *p = &this->props;
	uint32_t i, j, target = info->info.raw.channels;

	for (i = 0; i < p->n_channels; i++) {
		for (j = i; j < target; j++) {
			spa_log_debug(this->log, "%d %d: %d <-> %d", i, j,
					p->channel_map[i], info->info.raw.position[j]);
			if (p->channel_map[i] != info->info.raw.position[j])
				continue;
			if (i != j) {
				SPA_SWAP(p->channel_map[i], p->channel_map[j]);
				SPA_SWAP(p->channel.volumes[i], p->channel.volumes[j]);
				SPA_SWAP(p->soft.volumes[i], p->soft.volumes[j]);
				SPA_SWAP(p->monitor.volumes[i], p->monitor.volumes[j]);
			}
			break;
		}
	}
	p->n_channels = target;
	for (i = 0; i < p->n_channels; i++)
		p->channel_map[i] = info->info.raw.position[i];

	if (target == 0)
		return 0;
	if (p->channel.n_volumes != target)
		fix_volumes(this, &p->channel, target);
	if (p->soft.n_volumes != target)
		fix_volumes(this, &p->soft, target);
	if (p->monitor.n_volumes != target)
		fix_volumes(this, &p->monitor, target);

	return 1;
}

static void set_volume(struct impl *this)
{
	struct volumes *vol;
	uint32_t i;
	float volumes[SPA_AUDIO_MAX_CHANNELS];
	struct dir *dir = &this->dir[this->direction];

	spa_log_debug(this->log, "%p", this);

	if (dir->have_format)
		remap_volumes(this, &dir->format);

	if (this->mix.set_volume == NULL)
		return;

	if (this->props.have_soft_volume)
		vol = &this->props.soft;
	else
		vol = &this->props.channel;

	for (i = 0; i < vol->n_volumes; i++)
		volumes[i] = vol->volumes[dir->remap[i]];

	channelmix_set_volume(&this->mix, this->props.volume, vol->mute,
			vol->n_volumes, volumes);

	this->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
	this->params[IDX_Props].user++;
}

static int setup_channelmix(struct impl *this)
{
	struct dir *in = &this->dir[SPA_DIRECTION_INPUT];
	struct dir *out = &this->dir[SPA_DIRECTION_OUTPUT];
	uint32_t i, src_chan, dst_chan, p;
	uint64_t src_mask, dst_mask;
	int res;

	src_chan = in->format.info.raw.channels;
	dst_chan = out->format.info.raw.channels;

	for (i = 0, src_mask = 0; i < src_chan; i++) {
		p = in->format.info.raw.position[i];
		src_mask |= 1ULL << (p < 64 ? p : 0);
	}
	for (i = 0, dst_mask = 0; i < dst_chan; i++) {
		p = out->format.info.raw.position[i];
		dst_mask |= 1ULL << (p < 64 ? p : 0);
	}

	if (src_mask & 1)
		src_mask = default_mask(src_chan);
	if (dst_mask & 1)
		dst_mask = default_mask(dst_chan);

	spa_log_info(this->log, "%p: %s/%d@%d->%s/%d@%d %08"PRIx64":%08"PRIx64, this,
			spa_debug_type_find_name(spa_type_audio_format, SPA_AUDIO_FORMAT_DSP_F32),
			src_chan,
			in->format.info.raw.rate,
			spa_debug_type_find_name(spa_type_audio_format, SPA_AUDIO_FORMAT_DSP_F32),
			dst_chan,
			in->format.info.raw.rate,
			src_mask, dst_mask);

	this->mix.src_chan = src_chan;
	this->mix.src_mask = src_mask;
	this->mix.dst_chan = dst_chan;
	this->mix.dst_mask = dst_mask;
	this->mix.cpu_flags = this->cpu_flags;
	this->mix.log = this->log;
	this->mix.freq = in->format.info.raw.rate;

	if ((res = channelmix_init(&this->mix)) < 0)
		return res;

	set_volume(this);

	spa_log_debug(this->log, "%p: got channelmix features %08x:%08x flags:%08x %s",
			this, this->cpu_flags, this->mix.cpu_flags,
			this->mix.flags, this->mix.func_name);
	return 0;
}

static int setup_resample(struct impl *this)
{
	struct dir *in = &this->dir[SPA_DIRECTION_INPUT];
	struct dir *out = &this->dir[SPA_DIRECTION_OUTPUT];
	int res;

	spa_log_info(this->log, "%p: %s/%d@%d->%s/%d@%d", this,
			spa_debug_type_find_name(spa_type_audio_format, SPA_AUDIO_FORMAT_DSP_F32),
			out->format.info.raw.channels,
			in->format.info.raw.rate,
			spa_debug_type_find_name(spa_type_audio_format, SPA_AUDIO_FORMAT_DSP_F32),
			out->format.info.raw.channels,
			out->format.info.raw.rate);

	if (this->resample.free)
		resample_free(&this->resample);

	this->resample.channels = out->format.info.raw.channels;
	this->resample.i_rate = in->format.info.raw.rate;
	this->resample.o_rate = out->format.info.raw.rate;
	this->resample.log = this->log;
	this->resample.quality = this->props.resample_quality;
	this->resample.cpu_flags = this->cpu_flags;

	if (this->peaks)
		res = resample_peaks_init(&this->resample);
	else
		res = resample_native_init(&this->resample);

	spa_log_debug(this->log, "%p: got resample features %08x:%08x %s",
			this, this->cpu_flags, this->resample.cpu_flags,
			this->resample.func_name);
	return res;
}

static int calc_width(struct spa_audio_info *info)
{
	switch (info->info.raw.format) {
	case SPA_AUDIO_FORMAT_U8:
	case SPA_AUDIO_FORMAT_U8P:
	case SPA_AUDIO_FORMAT_S8:
	case SPA_AUDIO_FORMAT_S8P:
	case SPA_AUDIO_FORMAT_ULAW:
	case SPA_AUDIO_FORMAT_ALAW:
		return 1;
	case SPA_AUDIO_FORMAT_S16P:
	case SPA_AUDIO_FORMAT_S16:
	case SPA_AUDIO_FORMAT_S16_OE:
		return 2;
	case SPA_AUDIO_FORMAT_S24P:
	case SPA_AUDIO_FORMAT_S24:
	case SPA_AUDIO_FORMAT_S24_OE:
		return 3;
	case SPA_AUDIO_FORMAT_F64P:
	case SPA_AUDIO_FORMAT_F64:
	case SPA_AUDIO_FORMAT_F64_OE:
		return 8;
	default:
		return 4;
	}
}

static int setup_out_convert(struct impl *this)
{
	uint32_t i, j;
	struct dir *out = &this->dir[SPA_DIRECTION_OUTPUT];
	struct spa_audio_info src_info, dst_info;
	int res;
	bool remap = false;

	dst_info = out->format;
	src_info = dst_info;
	src_info.info.raw.format = SPA_AUDIO_FORMAT_DSP_F32;

	spa_log_info(this->log, "%p: %s/%d@%d->%s/%d@%d", this,
			spa_debug_type_find_name(spa_type_audio_format, src_info.info.raw.format),
			src_info.info.raw.channels,
			src_info.info.raw.rate,
			spa_debug_type_find_name(spa_type_audio_format, dst_info.info.raw.format),
			dst_info.info.raw.channels,
			dst_info.info.raw.rate);

	qsort(src_info.info.raw.position, src_info.info.raw.channels,
					sizeof(uint32_t), int32_cmp);

	for (i = 0; i < src_info.info.raw.channels; i++) {
		for (j = 0; j < dst_info.info.raw.channels; j++) {
			if (src_info.info.raw.position[i] !=
			    dst_info.info.raw.position[j])
				continue;
			out->remap[i] = j;
			if (i != j)
				remap = true;

			spa_log_debug(this->log, "%p: channel %d (%d) -> %d (%s -> %s)", this,
					i, out->remap[i], j,
					spa_debug_type_find_short_name(spa_type_audio_channel,
						src_info.info.raw.position[i]),
					spa_debug_type_find_short_name(spa_type_audio_channel,
						dst_info.info.raw.position[j]));
			dst_info.info.raw.position[j] = -1;
			break;
		}
	}
	if (out->conv.free)
		convert_free(&out->conv);

	out->conv.src_fmt = src_info.info.raw.format;
	out->conv.dst_fmt = dst_info.info.raw.format;
	out->conv.rate = dst_info.info.raw.rate;
	out->conv.n_channels = dst_info.info.raw.channels;
	out->conv.cpu_flags = this->cpu_flags;
	out->need_remap = remap;

	if ((res = convert_init(&out->conv)) < 0)
		return res;

	spa_log_debug(this->log, "%p: got converter features %08x:%08x quant:%d:%d"
			" passthrough:%d remap:%d %s", this,
			this->cpu_flags, out->conv.cpu_flags, out->conv.method,
			out->conv.noise_bits, out->conv.is_passthrough, remap, out->conv.func_name);

	return 0;
}

static int setup_convert(struct impl *this)
{
	struct dir *in, *out;
	uint32_t i, rate;
	int res;

	in = &this->dir[SPA_DIRECTION_INPUT];
	out = &this->dir[SPA_DIRECTION_OUTPUT];

	if (!in->have_format || !out->have_format)
		return -EINVAL;

	rate = this->io_position ?  this->io_position->clock.rate.denom : DEFAULT_RATE;

	/* in DSP mode we always convert to the DSP rate */
	if (in->mode == SPA_PARAM_PORT_CONFIG_MODE_dsp)
		in->format.info.raw.rate = rate;
	if (out->mode == SPA_PARAM_PORT_CONFIG_MODE_dsp)
		out->format.info.raw.rate = rate;

	/* try to passthrough the rates */
	if (in->format.info.raw.rate == 0)
		in->format.info.raw.rate = out->format.info.raw.rate;
	else if (out->format.info.raw.rate == 0)
		out->format.info.raw.rate = in->format.info.raw.rate;

	/* try to passthrough the channels */
	if (in->format.info.raw.channels == 0)
		in->format.info.raw.channels = out->format.info.raw.channels;
	else if (out->format.info.raw.channels == 0)
		out->format.info.raw.channels = in->format.info.raw.channels;

	if (in->format.info.raw.rate == 0 || out->format.info.raw.rate == 0)
		return -EINVAL;
	if (in->format.info.raw.channels == 0 || out->format.info.raw.channels == 0)
		return -EINVAL;

	if ((res = setup_in_convert(this)) < 0)
		return res;
	if ((res = setup_channelmix(this)) < 0)
		return res;
	if ((res = setup_resample(this)) < 0)
		return res;
	if ((res = setup_out_convert(this)) < 0)
		return res;

	for (i = 0; i < MAX_PORTS; i++) {
		this->tmp_datas[0][i] = SPA_PTROFF(this->tmp[0], this->empty_size * i, void);
		this->tmp_datas[0][i] = SPA_PTR_ALIGN(this->tmp_datas[0][i], MAX_ALIGN, void);
		this->tmp_datas[1][i] = SPA_PTROFF(this->tmp[1], this->empty_size * i, void);
		this->tmp_datas[1][i] = SPA_PTR_ALIGN(this->tmp_datas[1][i], MAX_ALIGN, void);
	}

	emit_node_info(this, false);

	return 0;
}

static void reset_node(struct impl *this)
{
	if (this->resample.reset)
		resample_reset(&this->resample);
	this->in_offset = 0;
	this->out_offset = 0;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (this->started)
			return 0;
		if ((res = setup_convert(this)) < 0)
			return res;
		this->started = true;
		break;
	case SPA_NODE_COMMAND_Suspend:
		SPA_FALLTHROUGH;
	case SPA_NODE_COMMAND_Flush:
		reset_node(this);
		SPA_FALLTHROUGH;
	case SPA_NODE_COMMAND_Pause:
		this->started = false;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static int
impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *this = object;
	uint32_t i;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_trace(this->log, "%p: add listener %p", this, listener);
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);
	for (i = 0; i < this->dir[SPA_DIRECTION_INPUT].n_ports; i++) {
		emit_port_info(this, GET_IN_PORT(this, i), true);
	}
	for (i = 0; i < this->dir[SPA_DIRECTION_OUTPUT].n_ports; i++) {
		emit_port_info(this, GET_OUT_PORT(this, i), true);
	}
	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *user_data)
{
	return 0;
}

static int impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	return -ENOTSUP;
}

static int
impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int port_enum_formats(void *object,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t index,
			     struct spa_pod **param,
			     struct spa_pod_builder *builder)
{
	struct impl *this = object;
	struct port *port = GET_PORT(this, direction, port_id);

	switch (index) {
	case 0:
		if (PORT_IS_DSP(this, direction, port_id)) {
			struct spa_audio_info_dsp info;
			info.format = SPA_AUDIO_FORMAT_DSP_F32;
			*param = spa_format_audio_dsp_build(builder,
				SPA_PARAM_EnumFormat, &info);
		} else if (PORT_IS_CONTROL(this, direction, port_id)) {
			*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
				SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_application),
				SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));
		} else if (port->have_format) {
			*param = spa_format_audio_raw_build(builder,
				SPA_PARAM_EnumFormat, &this->dir[direction].format.info.raw);
		}
		else {
			uint32_t rate = this->io_position ?
				this->io_position->clock.rate.denom : DEFAULT_RATE;

			*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
				SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
				SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_AUDIO_format,   SPA_POD_CHOICE_ENUM_Id(25,
							SPA_AUDIO_FORMAT_F32P,
							SPA_AUDIO_FORMAT_F32P,
							SPA_AUDIO_FORMAT_F32,
							SPA_AUDIO_FORMAT_F32_OE,
							SPA_AUDIO_FORMAT_F64P,
							SPA_AUDIO_FORMAT_F64,
							SPA_AUDIO_FORMAT_F64_OE,
							SPA_AUDIO_FORMAT_S32P,
							SPA_AUDIO_FORMAT_S32,
							SPA_AUDIO_FORMAT_S32_OE,
							SPA_AUDIO_FORMAT_S24_32P,
							SPA_AUDIO_FORMAT_S24_32,
							SPA_AUDIO_FORMAT_S24_32_OE,
							SPA_AUDIO_FORMAT_S24P,
							SPA_AUDIO_FORMAT_S24,
							SPA_AUDIO_FORMAT_S24_OE,
							SPA_AUDIO_FORMAT_S16P,
							SPA_AUDIO_FORMAT_S16,
							SPA_AUDIO_FORMAT_S16_OE,
							SPA_AUDIO_FORMAT_S8P,
							SPA_AUDIO_FORMAT_S8,
							SPA_AUDIO_FORMAT_U8P,
							SPA_AUDIO_FORMAT_U8,
							SPA_AUDIO_FORMAT_ULAW,
							SPA_AUDIO_FORMAT_ALAW),
				SPA_FORMAT_AUDIO_rate,     SPA_POD_CHOICE_RANGE_Int(
					rate, 1, INT32_MAX),
				SPA_FORMAT_AUDIO_channels, SPA_POD_CHOICE_RANGE_Int(
					DEFAULT_CHANNELS, 1, SPA_AUDIO_MAX_CHANNELS));
		}
		break;
	default:
		return 0;
	}
	return 1;
}

static int
impl_node_port_enum_params(void *object, int seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t start, uint32_t num,
			   const struct spa_pod *filter)
{
	struct impl *this = object;
	struct port *port;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[2048];
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	spa_log_debug(this->log, "%p: enum params port %d.%d %d %u",
			this, direction, port_id, seq, id);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if ((res = port_enum_formats(object, direction, port_id, result.index, &param, &b)) <= 0)
			return res;
		break;
	case SPA_PARAM_Format:
		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		if (PORT_IS_DSP(this, direction, port_id))
			param = spa_format_audio_dsp_build(&b, id, &port->format.info.dsp);
		else if (PORT_IS_CONTROL(this, direction, port_id))
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Format,  id,
				SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_application),
				SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));
		else
			param = spa_format_audio_raw_build(&b, id, &port->format.info.raw);
		break;
	case SPA_PARAM_Buffers:
	{
		uint32_t size;
		struct dir *dir;

		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		dir = &this->dir[direction];
		if (dir->mode == SPA_PARAM_PORT_CONFIG_MODE_dsp) {
			/* DSP ports always use the quantum_limit as the buffer
			 * size. */
			size = this->quantum_limit;
		} else {
			uint32_t irate, orate;
			/* Convert ports are scaled so that they can always
			 * provide one quantum of data */
			irate = dir->format.info.raw.rate;

			/* collect the other port rate */
			dir = &this->dir[SPA_DIRECTION_REVERSE(direction)];
			if (dir->mode == SPA_PARAM_PORT_CONFIG_MODE_dsp)
				orate = this->io_position ?  this->io_position->clock.rate.denom : DEFAULT_RATE;
			else
				orate = dir->format.info.raw.rate;

			/* always keep some extra room for adaptive resampling */
			size = this->quantum_limit * 2;
			/*  scale the buffer size when we can. */
			if (irate != 0 && orate != 0)
				size = size * (irate + orate - 1) / orate;
		}

		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(port->blocks),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
								size * port->stride,
								16 * port->stride,
								INT32_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(port->stride));
		break;
	}
	case SPA_PARAM_Meta:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
			break;
		default:
			return 0;
		}
		break;
	case SPA_PARAM_IO:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
			break;
		default:
			return 0;
		}
		break;
	case SPA_PARAM_Latency:
		switch (result.index) {
		case 0: case 1:
			param = spa_latency_build(&b, id, &this->dir[result.index].latency);
			break;
		default:
			return 0;
		}
		break;
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int clear_buffers(struct impl *this, struct port *port)
{
	if (port->n_buffers > 0) {
		spa_log_debug(this->log, "%p: clear buffers %p", this, port);
		port->n_buffers = 0;
		spa_list_init(&port->queue);
	}
	return 0;
}

static int port_set_latency(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *latency)
{
	struct impl *this = object;
	struct port *port;
	enum spa_direction other = SPA_DIRECTION_REVERSE(direction);
	uint32_t i;

	spa_log_debug(this->log, "%p: set latency direction:%d id:%d",
			this, direction, port_id);

	port = GET_PORT(this, direction, port_id);
	if (port->is_monitor)
		return 0;

	if (latency == NULL) {
		this->dir[other].latency = SPA_LATENCY_INFO(other);
	} else {
		struct spa_latency_info info;
		if (spa_latency_parse(latency, &info) < 0 ||
		    info.direction != other)
			return -EINVAL;
		this->dir[other].latency = info;
	}

	for (i = 0; i < this->dir[other].n_ports; i++) {
		port = GET_PORT(this, other, i);
		port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
		port->params[IDX_Latency].user++;
		emit_port_info(this, port, false);
	}
	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	port->params[IDX_Latency].user++;
	emit_port_info(this, port, false);
	return 0;
}

static int port_set_format(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	struct impl *this = object;
	struct port *port;
	int res;

	port = GET_PORT(this, direction, port_id);

	spa_log_debug(this->log, "%p: set format", this);

	if (format == NULL) {
		port->have_format = false;
		clear_buffers(this, port);
	} else {
		struct spa_audio_info info = { 0 };

		if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0) {
			spa_log_error(this->log, "can't parse format %s", spa_strerror(res));
			return res;
		}
		if (PORT_IS_DSP(this, direction, port_id)) {
			if (info.media_type != SPA_MEDIA_TYPE_audio ||
			    info.media_subtype != SPA_MEDIA_SUBTYPE_dsp) {
				spa_log_error(this->log, "unexpected types %d/%d",
						info.media_type, info.media_subtype);
				return -EINVAL;
			}
			if ((res = spa_format_audio_dsp_parse(format, &info.info.dsp)) < 0) {
				spa_log_error(this->log, "can't parse format %s", spa_strerror(res));
				return res;
			}
			if (info.info.dsp.format != SPA_AUDIO_FORMAT_DSP_F32) {
				spa_log_error(this->log, "unexpected format %d<->%d",
					info.info.dsp.format, SPA_AUDIO_FORMAT_DSP_F32);
				return -EINVAL;
			}
			port->blocks = 1;
			port->stride = 4;
		}
		else if (PORT_IS_CONTROL(this, direction, port_id)) {
			if (info.media_type != SPA_MEDIA_TYPE_application ||
			    info.media_subtype != SPA_MEDIA_SUBTYPE_control) {
				spa_log_error(this->log, "unexpected types %d/%d",
						info.media_type, info.media_subtype);
				return -EINVAL;
			}
			port->blocks = 1;
			port->stride = 1;
		}
		else {
			if (info.media_type != SPA_MEDIA_TYPE_audio ||
			    info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
				spa_log_error(this->log, "unexpected types %d/%d",
						info.media_type, info.media_subtype);
				return -EINVAL;
			}
			if ((res = spa_format_audio_raw_parse(format, &info.info.raw)) < 0) {
				spa_log_error(this->log, "can't parse format %s", spa_strerror(res));
				return res;
			}
			if (info.info.raw.channels > SPA_AUDIO_MAX_CHANNELS) {
				spa_log_error(this->log, "too many channels %d > %d",
						info.info.raw.channels, SPA_AUDIO_MAX_CHANNELS);
				return -EINVAL;
			}
			port->stride = calc_width(&info);
			if (SPA_AUDIO_FORMAT_IS_PLANAR(info.info.raw.format)) {
				port->blocks = info.info.raw.channels;
			} else {
				port->stride *= info.info.raw.channels;
				port->blocks = 1;
			}
			this->dir[direction].format = info;
			this->dir[direction].have_format = true;
		}
		port->format = info;
		port->have_format = true;

		spa_log_debug(this->log, "%p: %d %d %d", this,
				port_id, port->stride, port->blocks);
	}

	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	if (port->have_format) {
		port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
	} else {
		port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	}
	emit_port_info(this, port, false);

	return 0;
}


static int
impl_node_port_set_param(void *object,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, "%p: set param port %d.%d %u",
			this, direction, port_id, id);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	switch (id) {
	case SPA_PARAM_Latency:
		return port_set_latency(this, direction, port_id, flags, param);
	case SPA_PARAM_Format:
		return port_set_format(this, direction, port_id, flags, param);
	default:
		return -ENOENT;
	}
}

static void queue_buffer(struct impl *this, struct port *port, uint32_t id)
{
	struct buffer *b = &port->buffers[id];

	spa_log_trace_fp(this->log, "%p: queue buffer %d on port %d %d",
			this, id, port->id, b->flags);
	if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_QUEUED))
		return;

	spa_list_append(&port->queue, &b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_QUEUED);
}

static struct buffer *peek_buffer(struct impl *this, struct port *port)
{
	struct buffer *b;

	if (spa_list_is_empty(&port->queue))
		return NULL;

	b = spa_list_first(&port->queue, struct buffer, link);
	spa_log_trace_fp(this->log, "%p: peek buffer %d on port %d %u",
			this, b->id, port->id, b->flags);
	return b;
}

static void dequeue_buffer(struct impl *this, struct port *port, struct buffer *b)
{
	spa_list_remove(&b->link);
	SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_QUEUED);
	spa_log_trace_fp(this->log, "%p: dequeue buffer %d on port %d %u",
			this, b->id, port->id, b->flags);
}

static int
impl_node_port_use_buffers(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this = object;
	struct port *port;
	uint32_t i, j, maxsize;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	spa_return_val_if_fail(port->have_format, -EIO);

	spa_log_debug(this->log, "%p: use buffers %d on port %d:%d",
			this, n_buffers, direction, port_id);

	clear_buffers(this, port);

	maxsize = this->quantum_limit * sizeof(float);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		uint32_t n_datas = buffers[i]->n_datas;
		struct spa_data *d = buffers[i]->datas;

		b = &port->buffers[i];
		b->id = i;
		b->flags = 0;
		b->buf = buffers[i];

		if (n_datas != port->blocks) {
			spa_log_error(this->log, "%p: invalid blocks %d on buffer %d",
					this, n_datas, i);
			return -EINVAL;
		}

		for (j = 0; j < n_datas; j++) {
			if (d[j].data == NULL) {
				spa_log_error(this->log, "%p: invalid memory %d on buffer %d %d %p",
						this, j, i, d[j].type, d[j].data);
				return -EINVAL;
			}
			if (!SPA_IS_ALIGNED(d[j].data, this->max_align)) {
				spa_log_warn(this->log, "%p: memory %d on buffer %d not aligned",
						this, j, i);
			}
			if (direction == SPA_DIRECTION_OUTPUT &&
			    !SPA_FLAG_IS_SET(d[j].flags, SPA_DATA_FLAG_DYNAMIC))
				this->is_passthrough = false;

			b->datas[j] = d[j].data;

			maxsize = SPA_MAX(maxsize, d[j].maxsize);
		}
		if (direction == SPA_DIRECTION_OUTPUT)
			queue_buffer(this, port, i);
	}
	if (maxsize > this->empty_size) {
		this->empty = realloc(this->empty, maxsize + MAX_ALIGN);
		this->scratch = realloc(this->scratch, maxsize + MAX_ALIGN);
		this->tmp[0] = realloc(this->tmp[0], (maxsize + MAX_ALIGN) * MAX_PORTS);
		this->tmp[1] = realloc(this->tmp[1], (maxsize + MAX_ALIGN) * MAX_PORTS);
		if (this->empty == NULL || this->scratch == NULL ||
		    this->tmp[0] == NULL || this->tmp[1] == NULL)
			return -errno;
		memset(this->empty, 0, maxsize + MAX_ALIGN);
		this->empty_size = maxsize;
	}
	port->n_buffers = n_buffers;

	return 0;
}

static int
impl_node_port_set_io(void *object,
		      enum spa_direction direction, uint32_t port_id,
		      uint32_t id, void *data, size_t size)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, "%p: set io %d on port %d:%d %p",
			this, id, direction, port_id, data);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	switch (id) {
	case SPA_IO_Buffers:
		port->io = data;
		break;
	case SPA_IO_RateMatch:
		this->io_rate_match = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);

	port = GET_OUT_PORT(this, port_id);
	queue_buffer(this, port, buffer_id);

	return 0;
}

static int channelmix_process_control(struct impl *this, struct port *ctrlport,
				      void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
				      uint32_t n_samples)
{
	struct spa_pod_control *c, *prev = NULL;
	uint32_t avail_samples = n_samples;
	uint32_t i;
	const float *s[MAX_PORTS], **ss = (const float**) src;
	float *d[MAX_PORTS], **sd = (float **) dst;
	const struct spa_pod_sequence_body *body = &(ctrlport->ctrl)->body;
	uint32_t size = SPA_POD_BODY_SIZE(ctrlport->ctrl);
	bool end = false;

	c = spa_pod_control_first(body);
	while (true) {
		uint32_t chunk;

		if (c == NULL || !spa_pod_control_is_inside(body, size, c)) {
			c = NULL;
			end = true;
		}
		if (avail_samples == 0)
			break;

		/* ignore old control offsets */
		if (c != NULL) {
			if (c->offset <= ctrlport->ctrl_offset) {
				prev = c;
				if (c != NULL)
					c = spa_pod_control_next(c);
				continue;
			}
			chunk = SPA_MIN(avail_samples, c->offset - ctrlport->ctrl_offset);
			spa_log_trace_fp(this->log, "%p: process %d-%d %d/%d", this,
					ctrlport->ctrl_offset, c->offset, chunk, avail_samples);
		} else {
			chunk = avail_samples;
			spa_log_trace_fp(this->log, "%p: process remain %d", this, chunk);
		}

		if (prev) {
			switch (prev->type) {
			case SPA_CONTROL_Midi:
				apply_midi(this, &prev->value);
				break;
			case SPA_CONTROL_Properties:
				apply_props(this, &prev->value);
				break;
			default:
				continue;
			}
		}
		if (ss == (const float**)src && chunk != avail_samples) {
			for (i = 0; i < this->mix.src_chan; i++)
				s[i] = ss[i];
			for (i = 0; i < this->mix.dst_chan; i++)
				d[i] = sd[i];
			ss = s;
			sd = d;
		}

		channelmix_process(&this->mix, (void**)sd, (const void**)ss, chunk);

		if (chunk != avail_samples) {
			for (i = 0; i < this->mix.src_chan; i++)
				ss[i] += chunk;
			for (i = 0; i < this->mix.dst_chan; i++)
				sd[i] += chunk;
		}
		avail_samples -= chunk;
		ctrlport->ctrl_offset += chunk;
	}
	return end ? 1 : 0;
}

static uint32_t resample_update_rate_match(struct impl *this, bool passthrough, uint32_t out_size, uint32_t in_queued)
{
	double rate = this->rate_scale / this->props.rate;
	uint32_t delay, match_size;

	if (passthrough) {
		delay = 0;
		match_size = out_size;
	} else {
		if (this->io_rate_match &&
		    SPA_FLAG_IS_SET(this->io_rate_match->flags, SPA_IO_RATE_MATCH_FLAG_ACTIVE))
			rate *= this->io_rate_match->rate;
		resample_update_rate(&this->resample, rate);
		delay = resample_delay(&this->resample);
		match_size = resample_in_len(&this->resample, out_size);
	}
	match_size -= SPA_MIN(match_size, in_queued);

	spa_log_trace_fp(this->log, "%p: next match %u", this, match_size);

	if (this->io_rate_match) {
		this->io_rate_match->delay = delay;
		this->io_rate_match->size = match_size;
	}
	return match_size;
}

static inline bool resample_is_passthrough(struct impl *this)
{
	return this->resample.i_rate == this->resample.o_rate && this->rate_scale == 1.0 &&
		this->props.rate == 1.0 &&
		(this->io_rate_match == NULL ||
		 !SPA_FLAG_IS_SET(this->io_rate_match->flags, SPA_IO_RATE_MATCH_FLAG_ACTIVE));
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	const void *src_datas[MAX_PORTS], **in_datas;
	void *dst_datas[MAX_PORTS], *remap_src_datas[MAX_PORTS], *remap_dst_datas[MAX_PORTS];
	void **out_datas, **dst_remap;
	uint32_t i, j, n_src_datas = 0, n_dst_datas = 0, n_mon_datas = 0, remap;
	uint32_t n_samples, max_in, n_out, max_out, quant_samples;
	struct port *port, *ctrlport = NULL;
	struct buffer *buf, *out_bufs[MAX_PORTS];
	struct spa_data *bd;
	struct dir *dir;
	int tmp = 0, res = 0;
	bool in_passthrough, mix_passthrough, resample_passthrough, out_passthrough;
	bool in_avail = false, flush_in = false, flush_out = false, draining = false, in_empty = true;
	struct spa_io_buffers *io, *ctrlio = NULL;
	const struct spa_pod_sequence *ctrl = NULL;

	/* calculate quantum scale, this is how many samples we need to produce or
	 * consume. Also update the rate scale, this is sent to the resampler to adjust
	 * the rate, either when the graph clock changed or when the user adjusted the
	 * rate.  */
	if (SPA_LIKELY(this->io_position)) {
		double r =  this->rate_scale;

		quant_samples = this->io_position->clock.duration;
		if (this->direction == SPA_DIRECTION_INPUT) {
			if (this->io_position->clock.rate.denom != this->resample.o_rate)
				r = (double) this->io_position->clock.rate.denom / this->resample.o_rate;
			else
				r = 1.0;
		} else {
			if (this->io_position->clock.rate.denom != this->resample.i_rate)
				r = (double) this->resample.i_rate / this->io_position->clock.rate.denom;
			else
				r = 1.0;
		}
		if (this->rate_scale != r) {
			spa_log_info(this->log, "scale %f->%f", this->rate_scale, r);
			this->rate_scale = r;
		}
	}
	else
		quant_samples = this->quantum_limit;

	dir = &this->dir[SPA_DIRECTION_INPUT];
	in_passthrough = dir->conv.is_passthrough;
	max_in = UINT32_MAX;

	/* collect input port data */
	for (i = 0; i < dir->n_ports; i++) {
		port = GET_IN_PORT(this, i);

		if (SPA_UNLIKELY((io = port->io) == NULL)) {
			spa_log_trace_fp(this->log, "%p: no io on input port %d",
					this, port->id);
			buf = NULL;
		} else if (SPA_UNLIKELY(io->status != SPA_STATUS_HAVE_DATA)) {
			if (io->status & SPA_STATUS_DRAINED) {
				spa_log_debug(this->log, "%p: port %d drained", this, port->id);
				in_avail = flush_in = draining = true;
			} else {
				spa_log_trace_fp(this->log, "%p: empty input port %d %p %d %d %d",
						this, port->id, io, io->status, io->buffer_id,
						port->n_buffers);
				this->drained = false;
			}
			buf = NULL;
		} else if (SPA_UNLIKELY(io->buffer_id >= port->n_buffers)) {
			spa_log_trace_fp(this->log, "%p: invalid input buffer port %d %p %d %d %d",
					this, port->id, io, io->status, io->buffer_id,
					port->n_buffers);
			io->status = -EINVAL;
			buf = NULL;
		} else {
			spa_log_trace_fp(this->log, "%p: input buffer port %d io:%p status:%d id:%d n:%d",
					this, port->id, io, io->status, io->buffer_id,
					port->n_buffers);
			buf = &port->buffers[io->buffer_id];
		}

		if (SPA_UNLIKELY(buf == NULL)) {
			for (j = 0; j < port->blocks; j++) {
				if (port->is_control) {
					spa_log_trace_fp(this->log, "%p: empty control %d", this,
							i * port->blocks + j);
				} else {
					remap = n_src_datas++;
					src_datas[remap] = SPA_PTR_ALIGN(this->empty, MAX_ALIGN, void);
					spa_log_trace_fp(this->log, "%p: empty input %d->%d", this,
							i * port->blocks + j, remap);
					max_in = SPA_MIN(max_in, this->empty_size / port->stride);
				}
			}
		} else {
			in_avail = true;
			for (j = 0; j < port->blocks; j++) {
				uint32_t offs, size;

				bd = &buf->buf->datas[j];

				offs = SPA_MIN(bd->chunk->offset, bd->maxsize);
				size = SPA_MIN(bd->maxsize - offs, bd->chunk->size);
				if (!SPA_FLAG_IS_SET(bd->chunk->flags, SPA_CHUNK_FLAG_EMPTY))
					in_empty = false;

				if (SPA_UNLIKELY(port->is_control)) {
					spa_log_trace_fp(this->log, "%p: control %d", this,
							i * port->blocks + j);
					ctrlport = port;
					ctrlio = io;
					ctrl = spa_pod_from_data(bd->data, bd->maxsize,
							bd->chunk->offset, bd->chunk->size);
					if (ctrl && !spa_pod_is_sequence(&ctrl->pod))
						ctrl = NULL;
					if (ctrl != ctrlport->ctrl) {
						ctrlport->ctrl = ctrl;
						ctrlport->ctrl_offset = 0;
					}
				} else  {
					max_in = SPA_MIN(max_in, size / port->stride);

					remap = n_src_datas++;
					offs += this->in_offset * port->stride;
					src_datas[remap] = SPA_PTROFF(bd->data, offs, void);

					spa_log_trace_fp(this->log, "%p: input %d:%d:%d %d %d %d->%d", this,
							offs, size, port->stride, this->in_offset, max_in,
							i * port->blocks + j, remap);
				}
			}
		}
	}

	/* calculate how many samples we are going to produce. */
	if (this->direction == SPA_DIRECTION_INPUT) {
		/* in split mode we need to output exactly the size of the
		 * duration so we don't try to flush early */
		max_out = quant_samples;
		flush_out = false;
	} else {
		/* in merge mode we consume one duration of samples and
		 * always output the resulting data */
		max_out = this->quantum_limit;
		flush_out = true;
	}

	dir = &this->dir[SPA_DIRECTION_OUTPUT];
	/* collect output ports and monitor ports data */
	for (i = 0; i < dir->n_ports; i++) {
		port = GET_OUT_PORT(this, i);

		if (SPA_UNLIKELY((io = port->io) == NULL ||
		    io->status == SPA_STATUS_HAVE_DATA)) {
			buf = NULL;
		}
		else {
			if (SPA_LIKELY(io->buffer_id < port->n_buffers))
				queue_buffer(this, port, io->buffer_id);

			buf = peek_buffer(this, port);
		}
		out_bufs[i] = buf;

		if (SPA_UNLIKELY(buf == NULL)) {
			for (j = 0; j < port->blocks; j++) {
				if (port->is_monitor) {
					remap = n_mon_datas++;
					spa_log_trace_fp(this->log, "%p: empty monitor %d", this,
						remap);
				} else if (port->is_control) {
					spa_log_trace_fp(this->log, "%p: empty control %d", this, j);
				} else {
					remap = n_dst_datas++;
					dst_datas[remap] = SPA_PTR_ALIGN(this->scratch, MAX_ALIGN, void);
					spa_log_trace_fp(this->log, "%p: empty output %d->%d", this,
						i * port->blocks + j, remap);
					max_out = SPA_MIN(max_out, this->empty_size / port->stride);
				}
			}
		} else {
			for (j = 0; j < port->blocks; j++) {
				bd = &buf->buf->datas[j];

				bd->chunk->offset = 0;
				bd->chunk->size = 0;
				if (port->is_monitor) {
					float volume;
					uint32_t mon_max;

					remap = n_mon_datas++;
					volume = this->props.monitor.mute ? 0.0f : this->props.monitor.volumes[remap];
					if (this->monitor_channel_volumes)
						volume *= this->props.channel.mute ? 0.0f :
							this->props.channel.volumes[remap];

					mon_max = SPA_MIN(bd->maxsize / port->stride, max_in);

					volume_process(&this->volume, bd->data, src_datas[remap],
							volume, mon_max);

					bd->chunk->size = mon_max * port->stride;

					spa_log_trace_fp(this->log, "%p: monitor %d %d", this,
							remap, mon_max);

					dequeue_buffer(this, port, buf);
					io->status = SPA_STATUS_HAVE_DATA;
					io->buffer_id = buf->id;
					res |= SPA_STATUS_HAVE_DATA;
				} else if (SPA_UNLIKELY(port->is_control)) {
					spa_log_trace_fp(this->log, "%p: control %d", this, j);
				} else {
					remap = n_dst_datas++;
					dst_datas[remap] = SPA_PTROFF(bd->data,
							this->out_offset * port->stride, void);
					max_out = SPA_MIN(max_out, bd->maxsize / port->stride);

					spa_log_trace_fp(this->log, "%p: output %d offs:%d %d->%d", this,
							max_out, this->out_offset,
							i * port->blocks + j, remap);
				}
			}
		}
	}


	/* calculate how many samples at most we are going to consume. If we're
	 * draining, we consume as much as we can. Otherwise we consume what is
	 * left. */
	if (SPA_UNLIKELY(draining))
		n_samples = SPA_MIN(max_in, this->quantum_limit);
	else {
		n_samples = max_in - SPA_MIN(max_in, this->in_offset);
	}
	/* we only need to output the remaining samples */
	n_out = max_out - SPA_MIN(max_out, this->out_offset);

	resample_passthrough = resample_is_passthrough(this);

	/* calculate how many samples we are going to consume. */
	if (this->direction == SPA_DIRECTION_INPUT) {
		uint32_t n_in;
		/* then figure out how much input samples we need to consume */
		n_in = resample_update_rate_match(this, resample_passthrough, n_out, 0);
		if (!in_avail || this->drained) {
			spa_log_trace_fp(this->log, "%p: no input drained:%d", this, this->drained);
			/* no input, ask for more */
			res |= this->drained ? SPA_STATUS_DRAINED : SPA_STATUS_NEED_DATA;
			return res;
		}
		n_samples = SPA_MIN(n_samples, n_in);
	} else {
		/* in merge mode we consume one duration of samples */
		n_samples = SPA_MIN(n_samples, quant_samples);
		flush_in = true;
	}

	mix_passthrough = SPA_FLAG_IS_SET(this->mix.flags, CHANNELMIX_FLAG_IDENTITY) &&
		(ctrlport == NULL || ctrlport->ctrl == NULL);

	out_passthrough = dir->conv.is_passthrough;
	if (in_passthrough && mix_passthrough && resample_passthrough)
		out_passthrough = false;

	if (out_passthrough && dir->need_remap) {
		for (i = 0; i < dir->conv.n_channels; i++) {
			remap_dst_datas[i] = dst_datas[dir->remap[i]];
			spa_log_trace_fp(this->log, "%p: output remap %d -> %d", this, i, dir->remap[i]);
		}
		dst_remap = (void **)remap_dst_datas;
	} else {
		dst_remap = (void **)dst_datas;
	}

	dir = &this->dir[SPA_DIRECTION_INPUT];
	if (!in_passthrough) {
		if (mix_passthrough && resample_passthrough && out_passthrough)
			out_datas = (void **)dst_remap;
		else
			out_datas = (void **)this->tmp_datas[(tmp++) & 1];

		if (dir->need_remap) {
			for (i = 0; i < dir->conv.n_channels; i++) {
				remap_src_datas[i] = out_datas[dir->remap[i]];
				spa_log_trace_fp(this->log, "%p: input remap %d -> %d", this, dir->remap[i], i);
			}
		} else {
			for (i = 0; i < dir->conv.n_channels; i++)
				remap_src_datas[i] = out_datas[i];
		}

		spa_log_trace_fp(this->log, "%p: input convert %d", this, n_samples);
		convert_process(&dir->conv, remap_src_datas, src_datas, n_samples);
	} else {
		if (dir->need_remap) {
			for (i = 0; i < dir->conv.n_channels; i++) {
				remap_src_datas[dir->remap[i]] = (void *)src_datas[i];
				spa_log_trace_fp(this->log, "%p: input remap %d -> %d", this, dir->remap[i], i);
			}
			out_datas = (void **)remap_src_datas;
		} else {
			out_datas = (void **)src_datas;
		}
	}

	if (!mix_passthrough) {
		in_datas = (const void**)out_datas;
		if (resample_passthrough && out_passthrough) {
			out_datas = (void **)dst_remap;
			n_samples = SPA_MIN(n_samples, n_out);
		} else {
			out_datas = (void **)this->tmp_datas[(tmp++) & 1];
		}
		spa_log_trace_fp(this->log, "%p: channelmix %d %d %d", this, n_samples,
				resample_passthrough, out_passthrough);
		if (ctrlport != NULL && ctrlport->ctrl != NULL) {
			if (channelmix_process_control(this, ctrlport, out_datas,
						in_datas, n_samples) == 1) {
				ctrlio->status = SPA_STATUS_OK;
				ctrlport->ctrl = NULL;
			}
		} else {
			channelmix_process(&this->mix, out_datas, in_datas, n_samples);
		}
	}
	if (!resample_passthrough) {
		uint32_t in_len, out_len;

		in_datas = (const void**)out_datas;
		if (out_passthrough)
			out_datas = (void **)dst_remap;
		else
			out_datas = (void **)this->tmp_datas[(tmp++) & 1];

		in_len = n_samples;
		out_len = n_out;
		resample_process(&this->resample, in_datas, &in_len, out_datas, &out_len);
		spa_log_trace_fp(this->log, "%p: resample %d/%d -> %d/%d %d", this,
				n_samples, in_len, n_out, out_len, out_passthrough);
		this->in_offset += in_len;
		n_samples = out_len;
	} else {
		n_samples = SPA_MIN(n_samples, n_out);
		this->in_offset += n_samples;
	}
	this->out_offset += n_samples;

	if (!out_passthrough) {
		dir = &this->dir[SPA_DIRECTION_OUTPUT];
		if (dir->need_remap) {
			for (i = 0; i < dir->conv.n_channels; i++) {
				remap_dst_datas[dir->remap[i]] = out_datas[i];
				spa_log_trace_fp(this->log, "%p: output remap %d -> %d", this, i, dir->remap[i]);
			}
			in_datas = (const void**)remap_dst_datas;
		} else {
			in_datas = (const void**)out_datas;
		}
		spa_log_trace_fp(this->log, "%p: output convert %d", this, n_samples);
		convert_process(&dir->conv, dst_datas, in_datas, n_samples);
	}

	spa_log_trace_fp(this->log, "%d/%d  %d/%d %d->%d", this->in_offset, max_in,
			this->out_offset, max_out, n_samples, n_out);

	dir = &this->dir[SPA_DIRECTION_INPUT];
	if (SPA_LIKELY(this->in_offset >= max_in || flush_in)) {
		/* return input buffers */
		for (i = 0; i < dir->n_ports; i++) {
			port = GET_IN_PORT(this, i);
			if (port->is_control)
				continue;
			if (SPA_UNLIKELY((io = port->io) == NULL))
				continue;
			spa_log_trace_fp(this->log, "return: input %d %d", port->id, io->buffer_id);
			if (!draining)
				io->status = SPA_STATUS_NEED_DATA;
		}
		this->in_offset = 0;
		max_in = 0;
		res |= SPA_STATUS_NEED_DATA;
	}

	dir = &this->dir[SPA_DIRECTION_OUTPUT];
	if (SPA_LIKELY(n_samples > 0 && (this->out_offset >= max_out || flush_out))) {
		/* queue output buffers */
		for (i = 0; i < dir->n_ports; i++) {
			port = GET_OUT_PORT(this, i);
			if (SPA_UNLIKELY(port->is_monitor || port->is_control))
				continue;
			if (SPA_UNLIKELY((io = port->io) == NULL))
				continue;

			if (SPA_UNLIKELY((buf = out_bufs[i]) == NULL))
				continue;

			dequeue_buffer(this, port, buf);

			for (j = 0; j < port->blocks; j++) {
				bd = &buf->buf->datas[j];
				bd->chunk->size = this->out_offset * port->stride;
				SPA_FLAG_UPDATE(bd->chunk->flags, SPA_CHUNK_FLAG_EMPTY, in_empty);
				spa_log_trace_fp(this->log, "out: %d %d %d", this->out_offset,
						port->stride, bd->chunk->size);
			}
			io->status = SPA_STATUS_HAVE_DATA;
			io->buffer_id = buf->id;
		}
		res |= SPA_STATUS_HAVE_DATA;
		this->drained = draining;
		this->out_offset = 0;
	}
	else if (n_samples == 0 && this->peaks) {
		for (i = 0; i < dir->n_ports; i++) {
			port = GET_OUT_PORT(this, i);
			if (port->is_monitor || port->is_control)
				continue;
			if (SPA_UNLIKELY((io = port->io) == NULL))
				continue;

			io->status = SPA_STATUS_HAVE_DATA;
			io->buffer_id = SPA_ID_INVALID;
			res |= SPA_STATUS_HAVE_DATA;
			spa_log_trace_fp(this->log, "%p: no output buffer", this);
		}
	}
	if (resample_update_rate_match(this, resample_passthrough,
			max_out - this->out_offset,
			max_in - this->in_offset) > 0)
		res |= SPA_STATUS_NEED_DATA;

	return res;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.enum_params = impl_node_enum_params,
	.set_param = impl_node_set_param,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.add_port = impl_node_add_port,
	.remove_port = impl_node_remove_port,
	.port_enum_params = impl_node_port_enum_params,
	.port_set_param = impl_node_port_set_param,
	.port_use_buffers = impl_node_port_use_buffers,
	.port_set_io = impl_node_port_set_io,
	.port_reuse_buffer = impl_node_port_reuse_buffer,
	.process = impl_node_process,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Node))
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this;
	uint32_t i;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	this = (struct impl *) handle;

	for (i = 0; i < MAX_PORTS; i++)
		free(this->dir[SPA_DIRECTION_INPUT].ports[i]);
	for (i = 0; i < MAX_PORTS; i++)
		free(this->dir[SPA_DIRECTION_OUTPUT].ports[i]);
	free(this->empty);
	free(this->scratch);
	free(this->tmp[0]);
	free(this->tmp[1]);

	if (this->resample.free)
		resample_free(&this->resample);
	if (this->dir[0].conv.free)
		convert_free(&this->dir[0].conv);
	if (this->dir[1].conv.free)
		convert_free(&this->dir[1].conv);

	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_log_topic_init(this->log, log_topic);

	this->cpu = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_CPU);
	if (this->cpu) {
		this->cpu_flags = spa_cpu_get_flags(this->cpu);
		this->max_align = SPA_MIN(MAX_ALIGN, spa_cpu_get_max_align(this->cpu));
	}

	props_reset(&this->props);

	this->mix.options = CHANNELMIX_OPTION_UPMIX;
	this->mix.upmix = CHANNELMIX_UPMIX_PSD;
	this->mix.log = this->log;
	this->mix.lfe_cutoff = 150.0f;
	this->mix.fc_cutoff = 12000.0f;
	this->mix.rear_delay = 12.0f;
	this->mix.widen = 0.0f;

	for (i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, "clock.quantum-limit"))
			spa_atou32(s, &this->quantum_limit, 0);
		else if (spa_streq(k, "resample.peaks"))
			this->peaks = spa_atob(s);
		else if (spa_streq(k, "factory.mode")) {
			if (spa_streq(s, "merge"))
				this->direction = SPA_DIRECTION_OUTPUT;
			else
				this->direction = SPA_DIRECTION_INPUT;
		}
		else
			audioconvert_set_param(this, k, s);
	}

	this->dir[SPA_DIRECTION_INPUT].latency = SPA_LATENCY_INFO(SPA_DIRECTION_INPUT);
	this->dir[SPA_DIRECTION_OUTPUT].latency = SPA_LATENCY_INFO(SPA_DIRECTION_OUTPUT);

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);
	spa_hook_list_init(&this->hooks);

	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_input_ports = MAX_PORTS;
	this->info.max_output_ports = MAX_PORTS;
	this->info.flags = SPA_NODE_FLAG_RT |
		SPA_NODE_FLAG_IN_PORT_CONFIG |
		SPA_NODE_FLAG_OUT_PORT_CONFIG |
		SPA_NODE_FLAG_NEED_CONFIGURE;
	this->params[IDX_EnumPortConfig] = SPA_PARAM_INFO(SPA_PARAM_EnumPortConfig, SPA_PARAM_INFO_READ);
	this->params[IDX_PortConfig] = SPA_PARAM_INFO(SPA_PARAM_PortConfig, SPA_PARAM_INFO_READWRITE);
	this->params[IDX_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->params[IDX_Props] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = N_NODE_PARAMS;

	this->volume.cpu_flags = this->cpu_flags;
	volume_init(&this->volume);

	this->rate_scale = 1.0;

	reconfigure_mode(this, SPA_PARAM_PORT_CONFIG_MODE_convert, SPA_DIRECTION_INPUT, false, false, NULL);
	reconfigure_mode(this, SPA_PARAM_PORT_CONFIG_MODE_convert, SPA_DIRECTION_OUTPUT, false, false, NULL);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

const struct spa_handle_factory spa_audioconvert_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_AUDIO_CONVERT,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
