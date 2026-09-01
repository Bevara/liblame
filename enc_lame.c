/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / libmp3lame MP3 encoder filter - talks
 *  directly to LAME's native API (lame_init/lame_encode_buffer_interleaved/
 *  lame_close). Used as a progressive/MSE-compatible fallback audio path:
 *  MSE's ChunkDemuxer does not accept the source's original audio codec
 *  (e.g. Vorbis) remuxed as-is into mp4, and this project has no working
 *  native AAC encoder, so MP3 (an MSE-supported mp4 audio codec) is used
 *  instead. LAME's core encoder has no pthread dependency, unlike x265.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>

#include <lame.h>

typedef struct
{
	/* opts */
	u32 bitrate;

	GF_FilterPid *ipid, *opid;
	u32 sample_rate, num_channels;
	u64 samples_done;

	lame_global_flags *gfp;
} GF_LameEncCtx;

static GF_Err lameenc_setup(GF_LameEncCtx *ctx)
{
	if (ctx->gfp)
	{
		lame_close(ctx->gfp);
		ctx->gfp = NULL;
	}

	ctx->gfp = lame_init();
	if (!ctx->gfp)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[LameEnc] Failed to init encoder\n"));
		return GF_IO_ERR;
	}

	lame_set_in_samplerate(ctx->gfp, ctx->sample_rate);
	lame_set_num_channels(ctx->gfp, ctx->num_channels);
	lame_set_brate(ctx->gfp, ctx->bitrate);
	lame_set_quality(ctx->gfp, 5);

	if (lame_init_params(ctx->gfp) < 0)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[LameEnc] Failed to init encoder params\n"));
		lame_close(ctx->gfp);
		ctx->gfp = NULL;
		return GF_IO_ERR;
	}

	ctx->samples_done = 0;
	return GF_OK;
}

static GF_Err lameenc_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *prop;
	GF_LameEncCtx *ctx = (GF_LameEncCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		if (ctx->gfp)
		{
			lame_close(ctx->gfp);
			ctx->gfp = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;

	if (!ctx->opid)
	{
		ctx->opid = gf_filter_pid_new(filter);
	}
	gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_MPEG_AUDIO));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, NULL);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_UNFRAMED, NULL);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_DECODER_CONFIG, NULL);

	gf_filter_set_name(filter, "enclame:libmp3lame");

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_SAMPLE_RATE);
	if (!prop)
		return GF_OK;
	ctx->sample_rate = prop->value.uint;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_NUM_CHANNELS);
	if (!prop)
		return GF_OK;
	ctx->num_channels = prop->value.uint;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_AUDIO_FORMAT);
	if (prop && (prop->value.uint != GF_AUDIO_FMT_S16))
	{
		gf_filter_pid_negotiate_property(pid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));
		return GF_OK;
	}

	return lameenc_setup(ctx);
}

static GF_Err lameenc_process(GF_Filter *filter)
{
	GF_LameEncCtx *ctx = (GF_LameEncCtx *)gf_filter_get_udta(filter);
	GF_FilterPacket *pck, *dst_pck;
	const u8 *in_data;
	u32 size, num_samples;
	int mp3buf_size, ret;
	u8 *mp3buf, *output;

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			if (ctx->gfp)
			{
				mp3buf_size = 7200;
				mp3buf = gf_malloc(mp3buf_size);
				ret = lame_encode_flush(ctx->gfp, mp3buf, mp3buf_size);
				if (ret > 0)
				{
					dst_pck = gf_filter_pck_new_alloc(ctx->opid, ret, &output);
					if (dst_pck)
					{
						memcpy(output, mp3buf, ret);
						gf_filter_pck_set_cts(dst_pck, ctx->samples_done);
						gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
						gf_filter_pck_send(dst_pck);
					}
				}
				gf_free(mp3buf);
			}
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}

	if (!ctx->gfp)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_SERVICE_ERROR;
	}

	in_data = (const u8 *)gf_filter_pck_get_data(pck, &size);
	if (!in_data || !ctx->num_channels)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OK;
	}

	num_samples = size / (2 * ctx->num_channels);
	/* LAME docs: worst-case output size is 1.25 * num_samples + 7200 bytes */
	mp3buf_size = (int)(1.25 * num_samples) + 7200;
	mp3buf = gf_malloc(mp3buf_size);

	ret = lame_encode_buffer_interleaved(ctx->gfp, (short *)in_data, num_samples, mp3buf, mp3buf_size);

	if (ret < 0)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[LameEnc] Encoding failed (%d)\n", ret));
		gf_free(mp3buf);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	if (ret > 0)
	{
		dst_pck = gf_filter_pck_new_alloc(ctx->opid, ret, &output);
		if (dst_pck)
		{
			memcpy(output, mp3buf, ret);
			gf_filter_pck_set_cts(dst_pck, ctx->samples_done);
			gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
			gf_filter_pck_send(dst_pck);
		}
	}
	gf_free(mp3buf);

	ctx->samples_done += num_samples;
	gf_filter_pid_drop_packet(ctx->ipid);
	return GF_OK;
}

static void lameenc_finalize(GF_Filter *filter)
{
	GF_LameEncCtx *ctx = (GF_LameEncCtx *)gf_filter_get_udta(filter);
	if (ctx->gfp)
		lame_close(ctx->gfp);
}

static const GF_FilterCapability LameEncCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_AUDIO_FORMAT, GF_AUDIO_FMT_S16),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_MPEG_AUDIO),
};

#define OFFS(_n) #_n, offsetof(GF_LameEncCtx, _n)
static GF_FilterArgs LameEncArgs[] =
	{
		{OFFS(bitrate), "target bitrate in kbps", GF_PROP_UINT, "128", NULL, GF_FS_ARG_HINT_ADVANCED},
		{0}};

GF_FilterRegister LameEncRegister = {
	.name = "enclame",
	GF_FS_SET_DESCRIPTION("MP3 audio encoder (native libmp3lame)")
		GF_FS_SET_HELP("This filter encodes a raw S16 PCM audio PID to MPEG-1 Layer III (MP3) by calling "
					   "libmp3lame's native API directly.")
			.private_size = sizeof(GF_LameEncCtx),
	.args = LameEncArgs,
	SETCAPS(LameEncCaps),
	.configure_pid = lameenc_configure_pid,
	.process = lameenc_process,
	.finalize = lameenc_finalize,
};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE enclame_register(GF_FilterSession *session)
{
	return &LameEncRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_enclame(void) {
    gf_filter_auto_register("enclame", enclame_register);
}
