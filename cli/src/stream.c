// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki-cli.h>

#include <chiaki/session.h>
#include <chiaki/discovery.h>
#include <chiaki/base64.h>
#include <chiaki/bitstream.h>
#include <chiaki/time.h>
#include <chiaki/thread.h>
#include <chiaki/stoppipe.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>

#include <argp.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static char doc[] = "Start a headless PS4 or PS5 Remote Play stream (RTSP/file/pipe output).";

#define ARG_KEY_HOST           'h'
#define ARG_KEY_REGISTKEY      'r'
#define ARG_KEY_MORNING        'm'
#define ARG_KEY_OUTPUT         'o'
#define ARG_KEY_PS4            '4'
#define ARG_KEY_PS5            '5'
#define ARG_KEY_WAKEUP         'w'
#define ARG_KEY_RESOLUTION     256
#define ARG_KEY_FPS            257
#define ARG_KEY_BITRATE        258
#define ARG_KEY_CODEC          259
#define ARG_KEY_IDR_INTERVAL   260
#define ARG_KEY_WAKEUP_TIMEOUT 261
#define ARG_KEY_LISTEN_TRIGGER 262
#define ARG_KEY_NO_AUDIO       263

static struct argp_option options[] = {
	{ "host",           ARG_KEY_HOST,           "Host",     0, "Console IP or hostname (or env CHIAKI_HOST)", 0 },
	{ "registkey",      ARG_KEY_REGISTKEY,      "Key",      0, "8-char ASCII or 32-char hex registration key (or env CHIAKI_REGIST_KEY)", 0 },
	{ "morning",        ARG_KEY_MORNING,        "Key",      0, "Base64 or 32-char hex morning/rp_key (or env CHIAKI_MORNING_KEY)", 0 },
	{ "output",         ARG_KEY_OUTPUT,         "URL",      0, "Output RTSP URL or file path (or env CHIAKI_OUTPUT_URL)", 0 },
	{ "ps4",            ARG_KEY_PS4,            NULL,       0, "Target PlayStation 4", 0 },
	{ "ps5",            ARG_KEY_PS5,            NULL,       0, "Target PlayStation 5 (default)", 0 },
	{ "wakeup",         ARG_KEY_WAKEUP,         NULL,       0, "Wake console from standby if needed before connecting", 0 },
	{ "wakeup-timeout", ARG_KEY_WAKEUP_TIMEOUT, "Seconds",  0, "Seconds to wait for console wakeup (default: 40)", 0 },
	{ "resolution",     ARG_KEY_RESOLUTION,     "Preset",   0, "Resolution: 360p, 540p, 720p, 1080p (default: 1080p)", 0 },
	{ "fps",            ARG_KEY_FPS,            "FPS",      0, "Framerate: 30 or 60 (default: 60)", 0 },
	{ "bitrate",        ARG_KEY_BITRATE,        "Kbps",     0, "Video bitrate in kbps (default: 10000)", 0 },
	{ "codec",          ARG_KEY_CODEC,          "Codec",    0, "Video codec: h264 or h265 (default: h264)", 0 },
	{ "idr-interval",   ARG_KEY_IDR_INTERVAL,   "Ms",       0, "Periodic IDR keyframe request interval in ms (default: 2000, 0=off)", 0 },
	{ "listen-trigger", ARG_KEY_LISTEN_TRIGGER, "Port",     0, "On-demand daemon mode: listen on TCP port and stream while client is connected", 0 },
	{ "no-audio",       ARG_KEY_NO_AUDIO,       NULL,       0, "Disable audio stream muxing", 0 },
	{ 0 }
};

typedef struct stream_arguments_t
{
	const char *host;
	const char *registkey;
	const char *morning;
	const char *output;
	bool ps5;
	bool wakeup;
	unsigned int wakeup_timeout_sec;
	ChiakiVideoResolutionPreset resolution;
	ChiakiVideoFPSPreset fps;
	unsigned int bitrate;
	ChiakiCodec codec;
	uint64_t idr_interval_ms;
	int listen_trigger_port;
	bool no_audio;
} StreamArguments;

typedef struct stream_context_t
{
	ChiakiLog *log;
	const StreamArguments *args;
	ChiakiSession session;
	ChiakiBitstream bitstream;

	ChiakiMutex mux_mutex;
	AVFormatContext *fmt_ctx;
	AVStream *video_st;
	AVStream *audio_st;
	bool mux_initialized;
	bool mux_header_written;
	bool mux_failed;

	uint8_t *sps_pps_buf;
	size_t sps_pps_size;

	uint64_t start_time_us;
	bool start_time_set;
	int64_t last_video_dts;
	int64_t last_audio_dts;

	ChiakiStopPipe session_stop_pipe;
	volatile bool session_quit;
	ChiakiQuitReason quit_reason;
} StreamContext;

static ChiakiStopPipe g_sig_stop_pipe;
static volatile sig_atomic_t g_sig_interrupted = 0;

static void sig_handler(int sig)
{
	(void)sig;
	g_sig_interrupted = 1;
	chiaki_stop_pipe_stop(&g_sig_stop_pipe);
}

static int hex_val(char c)
{
	if(c >= '0' && c <= '9') return c - '0';
	if(c >= 'a' && c <= 'f') return c - 'a' + 10;
	if(c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static bool decode_hex(const char *hex, uint8_t *out, size_t out_len)
{
	if(strlen(hex) != out_len * 2)
		return false;
	for(size_t i = 0; i < out_len; i++)
	{
		int hi = hex_val(hex[i * 2]);
		int lo = hex_val(hex[i * 2 + 1]);
		if(hi < 0 || lo < 0)
			return false;
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return true;
}

static bool parse_regist_key(const char *input, char out_regist[CHIAKI_SESSION_AUTH_SIZE], uint64_t *out_wakeup_cred)
{
	memset(out_regist, 0, CHIAKI_SESSION_AUTH_SIZE);
	size_t len = strlen(input);
	if(len > 0 && len <= 8)
	{
		memcpy(out_regist, input, len);
		if(out_wakeup_cred)
			*out_wakeup_cred = (uint64_t)strtoull(input, NULL, 16);
		return true;
	}
	if(len == 32)
	{
		if(!decode_hex(input, (uint8_t *)out_regist, CHIAKI_SESSION_AUTH_SIZE))
			return false;
		char ascii_cred[9] = { 0 };
		memcpy(ascii_cred, out_regist, 8);
		if(out_wakeup_cred)
			*out_wakeup_cred = (uint64_t)strtoull(ascii_cred, NULL, 16);
		return true;
	}
	return false;
}

static bool parse_morning_key(const char *input, uint8_t out_morning[0x10])
{
	size_t len = strlen(input);
	if(len == 32 && decode_hex(input, out_morning, 0x10))
		return true;

	uint8_t tmp[32];
	size_t decoded_len = sizeof(tmp);
	if(chiaki_base64_decode(input, len, tmp, &decoded_len) == CHIAKI_ERR_SUCCESS && decoded_len == 0x10)
	{
		memcpy(out_morning, tmp, 0x10);
		return true;
	}
	return false;
}

static int parse_opt(int key, char *arg, struct argp_state *state)
{
	StreamArguments *args = state->input;
	switch(key)
	{
		case ARG_KEY_HOST:
			args->host = arg;
			break;
		case ARG_KEY_REGISTKEY:
			args->registkey = arg;
			break;
		case ARG_KEY_MORNING:
			args->morning = arg;
			break;
		case ARG_KEY_OUTPUT:
			args->output = arg;
			break;
		case ARG_KEY_PS4:
			args->ps5 = false;
			break;
		case ARG_KEY_PS5:
			args->ps5 = true;
			break;
		case ARG_KEY_WAKEUP:
			args->wakeup = true;
			break;
		case ARG_KEY_WAKEUP_TIMEOUT:
			args->wakeup_timeout_sec = (unsigned int)strtoul(arg, NULL, 10);
			break;
		case ARG_KEY_RESOLUTION:
			if(strcmp(arg, "360p") == 0)
				args->resolution = CHIAKI_VIDEO_RESOLUTION_PRESET_360p;
			else if(strcmp(arg, "540p") == 0)
				args->resolution = CHIAKI_VIDEO_RESOLUTION_PRESET_540p;
			else if(strcmp(arg, "720p") == 0)
				args->resolution = CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
			else if(strcmp(arg, "1080p") == 0)
				args->resolution = CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
			else
			{
				fprintf(stderr, "Invalid resolution '%s' (expected 360p, 540p, 720p, or 1080p)\n", arg);
				return ARGP_ERR_UNKNOWN;
			}
			break;
		case ARG_KEY_FPS:
			if(strcmp(arg, "30") == 0)
				args->fps = CHIAKI_VIDEO_FPS_PRESET_30;
			else if(strcmp(arg, "60") == 0)
				args->fps = CHIAKI_VIDEO_FPS_PRESET_60;
			else
			{
				fprintf(stderr, "Invalid fps '%s' (expected 30 or 60)\n", arg);
				return ARGP_ERR_UNKNOWN;
			}
			break;
		case ARG_KEY_BITRATE:
			args->bitrate = (unsigned int)strtoul(arg, NULL, 10);
			break;
		case ARG_KEY_CODEC:
			if(strcmp(arg, "h264") == 0)
				args->codec = CHIAKI_CODEC_H264;
			else if(strcmp(arg, "h265") == 0 || strcmp(arg, "hevc") == 0)
				args->codec = CHIAKI_CODEC_H265;
			else
			{
				fprintf(stderr, "Invalid codec '%s' (expected h264 or h265)\n", arg);
				return ARGP_ERR_UNKNOWN;
			}
			break;
		case ARG_KEY_IDR_INTERVAL:
			args->idr_interval_ms = (uint64_t)strtoull(arg, NULL, 10);
			break;
		case ARG_KEY_LISTEN_TRIGGER:
			args->listen_trigger_port = atoi(arg);
			break;
		case ARG_KEY_NO_AUDIO:
			args->no_audio = true;
			break;
		case ARGP_KEY_ARG:
			argp_usage(state);
			break;
		default:
			return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = { options, parse_opt, 0, doc, 0, 0, 0 };

typedef struct discovery_probe_state_t
{
	ChiakiDiscoveryHostState host_state;
	bool received;
} DiscoveryProbeState;

static void discovery_probe_cb(ChiakiDiscoveryHost *host, void *user)
{
	DiscoveryProbeState *st = user;
	if(!st || !host)
		return;
	st->host_state = host->state;
	st->received = true;
}

static ChiakiDiscoveryHostState probe_console_state(ChiakiLog *log, const char *host_str, bool ps5, uint64_t timeout_ms)
{
	struct addrinfo hints = { 0 };
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_family = strchr(host_str, ':') ? AF_INET6 : AF_INET;

	struct addrinfo *addrinfos = NULL;
	if(getaddrinfo(host_str, NULL, &hints, &addrinfos) != 0 || !addrinfos)
		return CHIAKI_DISCOVERY_HOST_STATE_UNKNOWN;

	struct sockaddr_storage host_addr = { 0 };
	socklen_t host_addr_len = 0;
	for(struct addrinfo *ai = addrinfos; ai; ai = ai->ai_next)
	{
		if(ai->ai_protocol != IPPROTO_UDP)
			continue;
		if(ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
			continue;
		memcpy(&host_addr, ai->ai_addr, ai->ai_addrlen);
		host_addr_len = ai->ai_addrlen;
		break;
	}
	freeaddrinfo(addrinfos);

	if(!host_addr_len)
		return CHIAKI_DISCOVERY_HOST_STATE_UNKNOWN;

	uint16_t port = ps5 ? CHIAKI_DISCOVERY_PORT_PS5 : CHIAKI_DISCOVERY_PORT_PS4;
	if(host_addr.ss_family == AF_INET)
		((struct sockaddr_in *)&host_addr)->sin_port = htons(port);
	else
		((struct sockaddr_in6 *)&host_addr)->sin6_port = htons(port);

	ChiakiDiscovery discovery;
	if(chiaki_discovery_init(&discovery, log, host_addr.ss_family) != CHIAKI_ERR_SUCCESS)
		return CHIAKI_DISCOVERY_HOST_STATE_UNKNOWN;

	DiscoveryProbeState probe = { CHIAKI_DISCOVERY_HOST_STATE_UNKNOWN, false };
	ChiakiDiscoveryThread thread;
	if(chiaki_discovery_thread_start_oneshot(&thread, &discovery, discovery_probe_cb, &probe) != CHIAKI_ERR_SUCCESS)
	{
		chiaki_discovery_fini(&discovery);
		return CHIAKI_DISCOVERY_HOST_STATE_UNKNOWN;
	}

	ChiakiDiscoveryPacket packet = { 0 };
	packet.cmd = CHIAKI_DISCOVERY_CMD_SRCH;
	packet.protocol_version = ps5 ? CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS5 : CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS4;
	chiaki_discovery_send(&discovery, &packet, (struct sockaddr *)&host_addr, host_addr_len);

	if(chiaki_thread_timedjoin(&thread.thread, NULL, timeout_ms) != CHIAKI_ERR_SUCCESS)
		chiaki_discovery_thread_stop(&thread);

	chiaki_discovery_fini(&discovery);
	return probe.received ? probe.host_state : CHIAKI_DISCOVERY_HOST_STATE_UNKNOWN;
}

static bool ensure_console_ready(ChiakiLog *log, const StreamArguments *args, uint64_t wakeup_cred)
{
	ChiakiDiscoveryHostState st = probe_console_state(log, args->host, args->ps5, 1500);
	CHIAKI_LOGI(log, "Console discovery state for %s: %s", args->host, chiaki_discovery_host_state_string(st));
	if(st == CHIAKI_DISCOVERY_HOST_STATE_READY)
		return true;

	if(!args->wakeup)
	{
		CHIAKI_LOGE(log, "Console is not ready (%s) and --wakeup was not specified", chiaki_discovery_host_state_string(st));
		return false;
	}

	CHIAKI_LOGI(log, "Sending wakeup packet to %s...", args->host);
	chiaki_discovery_wakeup(log, NULL, args->host, wakeup_cred, args->ps5);

	uint64_t start_ms = chiaki_time_now_monotonic_ms();
	uint64_t timeout_ms = (uint64_t)args->wakeup_timeout_sec * 1000;
	while(!g_sig_interrupted && (chiaki_time_now_monotonic_ms() - start_ms) < timeout_ms)
	{
		if(chiaki_stop_pipe_sleep(&g_sig_stop_pipe, 2000) == CHIAKI_ERR_CANCELED)
			return false;

		st = probe_console_state(log, args->host, args->ps5, 1500);
		CHIAKI_LOGI(log, "Polling console state: %s", chiaki_discovery_host_state_string(st));
		if(st == CHIAKI_DISCOVERY_HOST_STATE_READY)
		{
			CHIAKI_LOGI(log, "Console is READY, waiting 2s for Remote Play service initialization...");
			chiaki_stop_pipe_sleep(&g_sig_stop_pipe, 2000);
			return !g_sig_interrupted;
		}
		if(st == CHIAKI_DISCOVERY_HOST_STATE_STANDBY)
			chiaki_discovery_wakeup(log, NULL, args->host, wakeup_cred, args->ps5);
	}

	CHIAKI_LOGE(log, "Timed out waiting for console %s to wake up", args->host);
	return false;
}

static bool init_muxer_locked(StreamContext *ctx, const uint8_t *sps_pps, size_t sps_pps_size)
{
	if(ctx->mux_header_written)
		return true;
	if(ctx->mux_failed)
		return false;

	const char *out_url = ctx->args->output;
	const char *fmt_name = NULL;
	if(strncmp(out_url, "rtsp://", 7) == 0 || strncmp(out_url, "rtsps://", 8) == 0)
		fmt_name = "rtsp";
	else if(strncmp(out_url, "rtp://", 6) == 0)
		fmt_name = "rtp";

	int ret = avformat_alloc_output_context2(&ctx->fmt_ctx, NULL, fmt_name, out_url);
	if(ret < 0 || !ctx->fmt_ctx)
	{
		CHIAKI_LOGE(ctx->log, "Failed to allocate AVFormatContext for '%s': %s", out_url, av_err2str(ret));
		ctx->mux_failed = true;
		return false;
	}

	ctx->video_st = avformat_new_stream(ctx->fmt_ctx, NULL);
	if(!ctx->video_st)
	{
		CHIAKI_LOGE(ctx->log, "Failed to create video AVStream");
		ctx->mux_failed = true;
		return false;
	}

	ChiakiConnectVideoProfile vprof;
	chiaki_connect_video_profile_preset(&vprof, ctx->args->resolution, ctx->args->fps);
	ctx->video_st->id = 0;
	ctx->video_st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
	ctx->video_st->codecpar->codec_id = (ctx->args->codec == CHIAKI_CODEC_H265) ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
	ctx->video_st->codecpar->width = (int)vprof.width;
	ctx->video_st->codecpar->height = (int)vprof.height;
	ctx->video_st->time_base = (AVRational){ 1, 90000 };

	if(sps_pps && sps_pps_size > 0)
	{
		ctx->video_st->codecpar->extradata = av_mallocz(sps_pps_size + AV_INPUT_BUFFER_PADDING_SIZE);
		if(ctx->video_st->codecpar->extradata)
		{
			memcpy(ctx->video_st->codecpar->extradata, sps_pps, sps_pps_size);
			ctx->video_st->codecpar->extradata_size = (int)sps_pps_size;
		}
	}

	if(!ctx->args->no_audio)
	{
		ctx->audio_st = avformat_new_stream(ctx->fmt_ctx, NULL);
		if(!ctx->audio_st)
		{
			CHIAKI_LOGE(ctx->log, "Failed to create audio AVStream");
			ctx->mux_failed = true;
			return false;
		}
		ctx->audio_st->id = 1;
		ctx->audio_st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
		ctx->audio_st->codecpar->codec_id = AV_CODEC_ID_OPUS;
		ctx->audio_st->codecpar->sample_rate = 48000;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
		av_channel_layout_default(&ctx->audio_st->codecpar->ch_layout, 2);
#else
		ctx->audio_st->codecpar->channels = 2;
		ctx->audio_st->codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
#endif
		ctx->audio_st->time_base = (AVRational){ 1, 48000 };

		static const uint8_t opus_head[19] = {
			'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
			1, 2, 0x00, 0x00, 0x80, 0xbb, 0x00, 0x00, 0x00, 0x00, 0
		};
		ctx->audio_st->codecpar->extradata = av_mallocz(sizeof(opus_head) + AV_INPUT_BUFFER_PADDING_SIZE);
		if(ctx->audio_st->codecpar->extradata)
		{
			memcpy(ctx->audio_st->codecpar->extradata, opus_head, sizeof(opus_head));
			ctx->audio_st->codecpar->extradata_size = sizeof(opus_head);
		}
	}

	AVDictionary *opts = NULL;
	if(fmt_name && strcmp(fmt_name, "rtsp") == 0)
	{
		av_dict_set(&opts, "rtsp_transport", "tcp", 0);
		av_dict_set(&opts, "muxdelay", "0.0", 0);
		av_dict_set(&opts, "muxpreload", "0.0", 0);
	}

	if(!(ctx->fmt_ctx->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_open2(&ctx->fmt_ctx->pb, out_url, AVIO_FLAG_WRITE, NULL, &opts);
		if(ret < 0)
		{
			CHIAKI_LOGE(ctx->log, "Failed to open output '%s': %s", out_url, av_err2str(ret));
			av_dict_free(&opts);
			ctx->mux_failed = true;
			return false;
		}
	}

	ret = avformat_write_header(ctx->fmt_ctx, &opts);
	av_dict_free(&opts);
	if(ret < 0)
	{
		CHIAKI_LOGE(ctx->log, "Failed to write output header to '%s': %s", out_url, av_err2str(ret));
		ctx->mux_failed = true;
		return false;
	}

	ctx->mux_initialized = true;
	ctx->mux_header_written = true;
	ctx->start_time_us = chiaki_time_now_monotonic_us();
	ctx->start_time_set = true;
	ctx->last_video_dts = -1;
	ctx->last_audio_dts = -1;
	CHIAKI_LOGI(ctx->log, "Muxer connected and header written to '%s'", out_url);
	return true;
}

static void close_muxer(StreamContext *ctx)
{
	chiaki_mutex_lock(&ctx->mux_mutex);
	if(ctx->fmt_ctx)
	{
		if(ctx->mux_header_written)
			av_write_trailer(ctx->fmt_ctx);
		if(!(ctx->fmt_ctx->oformat->flags & AVFMT_NOFILE) && ctx->fmt_ctx->pb)
			avio_closep(&ctx->fmt_ctx->pb);
		avformat_free_context(ctx->fmt_ctx);
		ctx->fmt_ctx = NULL;
	}
	free(ctx->sps_pps_buf);
	ctx->sps_pps_buf = NULL;
	ctx->sps_pps_size = 0;
	ctx->mux_initialized = false;
	ctx->mux_header_written = false;
	ctx->mux_failed = false;
	chiaki_mutex_unlock(&ctx->mux_mutex);
}

static void inspect_annexb_nal(const uint8_t *buf, size_t buf_size, ChiakiCodec codec, bool *out_is_header, bool *out_is_keyframe)
{
	*out_is_header = false;
	*out_is_keyframe = false;

	size_t i = 0;
	while(i + 4 <= buf_size)
	{
		size_t sc_len = 0;
		if(buf[i] == 0x00 && buf[i + 1] == 0x00 && buf[i + 2] == 0x01)
			sc_len = 3;
		else if(i + 4 < buf_size && buf[i] == 0x00 && buf[i + 1] == 0x00 && buf[i + 2] == 0x00 && buf[i + 3] == 0x01)
			sc_len = 4;

		if(sc_len > 0 && (i + sc_len) < buf_size)
		{
			uint8_t nal_byte = buf[i + sc_len];
			if(chiaki_codec_is_h265(codec))
			{
				uint8_t nal_type = (nal_byte >> 1) & 0x3f;
				if(nal_type == 32 || nal_type == 33 || nal_type == 34)
					*out_is_header = true;
				else if(nal_type >= 16 && nal_type <= 21)
					*out_is_keyframe = true;
			}
			else
			{
				uint8_t nal_type = nal_byte & 0x1f;
				if(nal_type == 7 || nal_type == 8)
					*out_is_header = true;
				else if(nal_type == 5)
					*out_is_keyframe = true;
			}
			if(*out_is_keyframe)
				break;
			i += sc_len + 1;
		}
		else
		{
			i++;
		}
	}
}

static bool video_sample_cb(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user)
{
	(void)frames_lost;
	(void)frame_recovered;
	StreamContext *ctx = user;
	if(!ctx || !buf || buf_size == 0)
		return true;

	bool is_header = false;
	bool is_keyframe = false;
	inspect_annexb_nal(buf, buf_size, ctx->args->codec, &is_header, &is_keyframe);

	chiaki_mutex_lock(&ctx->mux_mutex);

	if(is_header && !is_keyframe)
	{
		uint8_t *new_hdr = malloc(buf_size);
		if(new_hdr)
		{
			memcpy(new_hdr, buf, buf_size);
			free(ctx->sps_pps_buf);
			ctx->sps_pps_buf = new_hdr;
			ctx->sps_pps_size = buf_size;
		}
		if(!ctx->mux_header_written)
			init_muxer_locked(ctx, ctx->sps_pps_buf, ctx->sps_pps_size);
		chiaki_mutex_unlock(&ctx->mux_mutex);
		return true;
	}

	if(!ctx->mux_header_written && !init_muxer_locked(ctx, ctx->sps_pps_buf, ctx->sps_pps_size))
	{
		chiaki_mutex_unlock(&ctx->mux_mutex);
		return false;
	}

	uint8_t *combined_buf = NULL;
	const uint8_t *pkt_data = buf;
	size_t pkt_size = buf_size;

	if(is_keyframe && ctx->sps_pps_buf && ctx->sps_pps_size > 0)
	{
		pkt_size = ctx->sps_pps_size + buf_size;
		combined_buf = malloc(pkt_size);
		if(combined_buf)
		{
			memcpy(combined_buf, ctx->sps_pps_buf, ctx->sps_pps_size);
			memcpy(combined_buf + ctx->sps_pps_size, buf, buf_size);
			pkt_data = combined_buf;
		}
		else
		{
			pkt_size = buf_size;
		}
	}

	uint64_t now_us = chiaki_time_now_monotonic_us();
	uint64_t rel_us = (now_us > ctx->start_time_us) ? (now_us - ctx->start_time_us) : 0;
	int64_t dts = av_rescale_q((int64_t)rel_us, (AVRational){ 1, 1000000 }, ctx->video_st->time_base);
	if(dts <= ctx->last_video_dts)
		dts = ctx->last_video_dts + 1;
	ctx->last_video_dts = dts;

	AVPacket *pkt = av_packet_alloc();
	if(pkt)
	{
		pkt->data = (uint8_t *)pkt_data;
		pkt->size = (int)pkt_size;
		pkt->stream_index = ctx->video_st->index;
		pkt->pts = dts;
		pkt->dts = dts;
		if(is_keyframe)
			pkt->flags |= AV_PKT_FLAG_KEY;

		int ret = av_interleaved_write_frame(ctx->fmt_ctx, pkt);
		if(ret < 0)
		{
			CHIAKI_LOGW(ctx->log, "Video mux write failed: %s", av_err2str(ret));
			ctx->mux_failed = true;
			ctx->session_quit = true;
			chiaki_stop_pipe_stop(&ctx->session_stop_pipe);
		}
		av_packet_free(&pkt);
	}

	free(combined_buf);
	chiaki_mutex_unlock(&ctx->mux_mutex);
	return true;
}

static void audio_header_cb(ChiakiAudioHeader *header, void *user)
{
	StreamContext *ctx = user;
	if(!ctx || !header)
		return;
	CHIAKI_LOGI(ctx->log, "Audio stream initialized: %u Hz, %u ch, frame_size=%u",
		header->rate, header->channels, header->frame_size);
}

static void audio_frame_cb(uint8_t *buf, size_t buf_size, void *user)
{
	StreamContext *ctx = user;
	if(!ctx || ctx->args->no_audio || !buf || buf_size == 0)
		return;

	chiaki_mutex_lock(&ctx->mux_mutex);
	if(!ctx->mux_header_written || ctx->mux_failed || !ctx->audio_st)
	{
		chiaki_mutex_unlock(&ctx->mux_mutex);
		return;
	}

	uint64_t now_us = chiaki_time_now_monotonic_us();
	uint64_t rel_us = (now_us > ctx->start_time_us) ? (now_us - ctx->start_time_us) : 0;
	int64_t dts = av_rescale_q((int64_t)rel_us, (AVRational){ 1, 1000000 }, ctx->audio_st->time_base);
	if(dts <= ctx->last_audio_dts)
		dts = ctx->last_audio_dts + 1;
	ctx->last_audio_dts = dts;

	AVPacket *pkt = av_packet_alloc();
	if(pkt)
	{
		pkt->data = buf;
		pkt->size = (int)buf_size;
		pkt->stream_index = ctx->audio_st->index;
		pkt->pts = dts;
		pkt->dts = dts;
		pkt->duration = av_rescale_q(480, (AVRational){ 1, 48000 }, ctx->audio_st->time_base);

		int ret = av_interleaved_write_frame(ctx->fmt_ctx, pkt);
		if(ret < 0)
			CHIAKI_LOGW(ctx->log, "Audio mux write failed: %s", av_err2str(ret));
		av_packet_free(&pkt);
	}

	chiaki_mutex_unlock(&ctx->mux_mutex);
}

static void session_event_cb(ChiakiEvent *event, void *user)
{
	StreamContext *ctx = user;
	if(!ctx || !event)
		return;

	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			CHIAKI_LOGI(ctx->log, "Remote Play session CONNECTED");
			break;
		case CHIAKI_EVENT_QUIT:
			CHIAKI_LOGI(ctx->log, "Remote Play session QUIT: %s (%s)",
				chiaki_quit_reason_string(event->quit.reason),
				event->quit.reason_str ? event->quit.reason_str : "none");
			ctx->quit_reason = event->quit.reason;
			ctx->session_quit = true;
			chiaki_stop_pipe_stop(&ctx->session_stop_pipe);
			break;
		default:
			break;
	}
}

static int run_single_stream_session(ChiakiLog *log, const StreamArguments *args,
	const char regist_key[CHIAKI_SESSION_AUTH_SIZE], const uint8_t morning[0x10],
	uint64_t wakeup_cred, int trigger_client_fd)
{
	if(!ensure_console_ready(log, args, wakeup_cred))
		return 1;

	StreamContext ctx = { 0 };
	ctx.log = log;
	ctx.args = args;
	ctx.quit_reason = CHIAKI_QUIT_REASON_NONE;
	chiaki_mutex_init(&ctx.mux_mutex, false);
	chiaki_stop_pipe_init(&ctx.session_stop_pipe);
	chiaki_bitstream_init(&ctx.bitstream, log, args->codec);

	ChiakiConnectInfo connect_info = { 0 };
	connect_info.ps5 = args->ps5;
	connect_info.host = args->host;
	memcpy(connect_info.regist_key, regist_key, CHIAKI_SESSION_AUTH_SIZE);
	memcpy(connect_info.morning, morning, 0x10);
	chiaki_connect_video_profile_preset(&connect_info.video_profile, args->resolution, args->fps);
	connect_info.video_profile.bitrate = args->bitrate;
	connect_info.video_profile.codec = args->codec;
	connect_info.video_profile_auto_downgrade = false;
	connect_info.enable_keyboard = false;
	connect_info.enable_dualsense = false;
	connect_info.enable_idr_on_fec_failure = true;
	connect_info.packet_loss_max = 0.0;
	connect_info.audio_video_disabled = args->no_audio ? CHIAKI_AUDIO_DISABLED : CHIAKI_NONE_DISABLED;

	ChiakiErrorCode err = chiaki_session_init(&ctx.session, &connect_info, log);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(log, "chiaki_session_init failed: %s", chiaki_error_string(err));
		chiaki_stop_pipe_fini(&ctx.session_stop_pipe);
		chiaki_mutex_fini(&ctx.mux_mutex);
		return 1;
	}

	chiaki_session_set_event_cb(&ctx.session, session_event_cb, &ctx);
	chiaki_session_set_video_sample_cb(&ctx.session, video_sample_cb, &ctx);

	if(!args->no_audio)
	{
		ChiakiAudioSink audio_sink = { 0 };
		audio_sink.user = &ctx;
		audio_sink.header_cb = audio_header_cb;
		audio_sink.frame_cb = audio_frame_cb;
		chiaki_session_set_audio_sink(&ctx.session, &audio_sink);
	}

	err = chiaki_session_start(&ctx.session);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(log, "chiaki_session_start failed: %s", chiaki_error_string(err));
		chiaki_session_fini(&ctx.session);
		chiaki_stop_pipe_fini(&ctx.session_stop_pipe);
		chiaki_mutex_fini(&ctx.mux_mutex);
		return 1;
	}

	uint64_t last_idr_ms = chiaki_time_now_monotonic_ms();
	while(!g_sig_interrupted && !ctx.session_quit)
	{
		uint64_t wait_ms = 250;
		chiaki_stop_pipe_select_single(&ctx.session_stop_pipe,
			trigger_client_fd >= 0 ? trigger_client_fd : CHIAKI_INVALID_SOCKET,
			false, wait_ms);

		if(trigger_client_fd >= 0)
		{
			char peek_byte;
			ssize_t r = recv(trigger_client_fd, &peek_byte, 1, MSG_PEEK | MSG_DONTWAIT);
			if(r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
			{
				CHIAKI_LOGI(log, "Trigger client disconnected, stopping Remote Play session...");
				break;
			}
		}

		if(args->idr_interval_ms > 0 && ctx.mux_header_written)
		{
			uint64_t now_ms = chiaki_time_now_monotonic_ms();
			if(now_ms - last_idr_ms >= args->idr_interval_ms)
			{
				chiaki_session_request_idr(&ctx.session);
				last_idr_ms = now_ms;
			}
		}
	}

	CHIAKI_LOGI(log, "Stopping Chiaki session gracefully...");
	chiaki_session_stop(&ctx.session);
	chiaki_session_join(&ctx.session);
	chiaki_session_fini(&ctx.session);

	close_muxer(&ctx);
	chiaki_stop_pipe_fini(&ctx.session_stop_pipe);
	chiaki_mutex_fini(&ctx.mux_mutex);

	return (ctx.quit_reason == CHIAKI_QUIT_REASON_NONE || ctx.quit_reason == CHIAKI_QUIT_REASON_STOPPED) ? 0 : 1;
}

CHIAKI_EXPORT int chiaki_cli_cmd_stream(ChiakiLog *log, int argc, char *argv[])
{
	StreamArguments args = { 0 };
	args.ps5 = true;
	args.wakeup = false;
	args.wakeup_timeout_sec = 40;
	args.resolution = CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
	args.fps = CHIAKI_VIDEO_FPS_PRESET_60;
	args.bitrate = 10000;
	args.codec = CHIAKI_CODEC_H264;
	args.idr_interval_ms = 2000;
	args.listen_trigger_port = 0;
	args.no_audio = false;

	args.host = getenv("CHIAKI_HOST");
	args.registkey = getenv("CHIAKI_REGIST_KEY");
	args.morning = getenv("CHIAKI_MORNING_KEY");
	args.output = getenv("CHIAKI_OUTPUT_URL");

	if(argp_parse(&argp, argc, argv, ARGP_IN_ORDER, NULL, &args) != 0)
		return 1;

	if(!args.host || !*args.host)
	{
		fprintf(stderr, "Error: --host (or CHIAKI_HOST) is required.\n");
		return 1;
	}
	if(!args.registkey || !*args.registkey)
	{
		fprintf(stderr, "Error: --registkey (or CHIAKI_REGIST_KEY) is required.\n");
		return 1;
	}
	if(!args.morning || !*args.morning)
	{
		fprintf(stderr, "Error: --morning (or CHIAKI_MORNING_KEY) is required.\n");
		return 1;
	}
	if(!args.output || !*args.output)
	{
		fprintf(stderr, "Error: --output (or CHIAKI_OUTPUT_URL) is required.\n");
		return 1;
	}

	char regist_key[CHIAKI_SESSION_AUTH_SIZE];
	uint64_t wakeup_cred = 0;
	if(!parse_regist_key(args.registkey, regist_key, &wakeup_cred))
	{
		fprintf(stderr, "Error: Invalid --registkey format (expected 1-8 char hex/ASCII or 32-char hex).\n");
		return 1;
	}

	uint8_t morning[0x10];
	if(!parse_morning_key(args.morning, morning))
	{
		fprintf(stderr, "Error: Invalid --morning format (expected base64 or 32-char hex representing 16 bytes).\n");
		return 1;
	}

	signal(SIGPIPE, SIG_IGN);
	chiaki_stop_pipe_init(&g_sig_stop_pipe);
	struct sigaction sa = { 0 };
	sa.sa_handler = sig_handler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	avformat_network_init();

	int ret = 0;
	if(args.listen_trigger_port <= 0)
	{
		ret = run_single_stream_session(log, &args, regist_key, morning, wakeup_cred, -1);
	}
	else
	{
		int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
		if(listen_fd < 0)
		{
			CHIAKI_LOGE(log, "Failed to create trigger socket: %s", strerror(errno));
			chiaki_stop_pipe_fini(&g_sig_stop_pipe);
			return 1;
		}
		int opt = 1;
		setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

		struct sockaddr_in addr = { 0 };
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = htons((uint16_t)args.listen_trigger_port);

		if(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(listen_fd, 4) < 0)
		{
			CHIAKI_LOGE(log, "Failed to bind/listen on trigger port %d: %s", args.listen_trigger_port, strerror(errno));
			close(listen_fd);
			chiaki_stop_pipe_fini(&g_sig_stop_pipe);
			return 1;
		}

		CHIAKI_LOGI(log, "On-demand trigger listener ready on TCP port %d", args.listen_trigger_port);
		while(!g_sig_interrupted)
		{
			if(chiaki_stop_pipe_select_single(&g_sig_stop_pipe, listen_fd, false, 60000) == CHIAKI_ERR_CANCELED)
				break;

			struct sockaddr_in client_addr;
			socklen_t client_len = sizeof(client_addr);
			int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
			if(client_fd < 0)
				continue;

			CHIAKI_LOGI(log, "Accepted on-demand trigger connection from %s:%d",
				inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

			run_single_stream_session(log, &args, regist_key, morning, wakeup_cred, client_fd);
			close(client_fd);
			CHIAKI_LOGI(log, "Session ended, returning to idle trigger listener on TCP port %d", args.listen_trigger_port);
		}
		close(listen_fd);
	}

	avformat_network_deinit();
	chiaki_stop_pipe_fini(&g_sig_stop_pipe);
	return ret;
}
