/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2018
 *					All rights reserved
 *
 *  This file is part of GPAC / MPEG-DASH/HLS segmenter
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <gpac/iso639.h>
#include <gpac/internal/mpd.h>
#include <gpac/internal/media_dev.h>


typedef struct
{
	GF_List *streams;

	//period element we will fill
	GF_MPD_Period *period;
} GF_DasherPeriod;

enum
{
	DASHER_BS_SWITCH_DEF=0,
	DASHER_BS_SWITCH_OFF,
	DASHER_BS_SWITCH_ON,
	DASHER_BS_SWITCH_INBAND,
	DASHER_BS_SWITCH_FORCE,
	DASHER_BS_SWITCH_MULTI,
};

enum
{
	DASHER_NTP_REM=0,
	DASHER_NTP_YES,
	DASHER_NTP_KEEP,
};

typedef struct
{
	u32 bs_switch, profile, cp, subs_per_sidx, ntp;
	s32 buf, timescale;
	Bool forcep, dynamic, single_file, single_segment, no_sar, mix_codecs, stl, tpl, align, sap, no_frag_def, sidx, split;
	Double dur;
	char *avcp;
	char *hvcp;
	char *aacp;
	char *template;
	char *ext;
	char *profX;
	s32 asto;
	char *ast;
	char *state;
	char *title, *source, *info, *cprt, *lang;
	GF_List *location, *base;
	Bool for_test, check_dur, skip_seg;
	Double refresh, tsb, subdur;
	
	//TODO & not yet exposed
	Bool mpeg2;

	//internal

	//MPD output pid
	GF_FilterPid *opid;

	GF_MPD *mpd;

	Double period_start;
	GF_DasherPeriod *current_period, *next_period;
	GF_List *pids;
	Bool template_use_source;

	Bool use_xlink, use_cenc, check_main_role;

	//options for muxers, constrained by profile
	Bool no_fragments_defaults;

	Bool is_eos;
	u32 nb_seg_url_pending;
	Bool on_demand_done;
	Bool subdur_done;
	char *out_path;

	GF_Err setup_failure;

	u64 generation_start_utc;
	u64 nb_secs_to_discard;
	Bool first_context_load, store_init_params;
} GF_DasherCtx;


typedef struct _dash_stream
{
	GF_FilterPid *ipid, *opid;

	//stream properties
	u32 codec_id, timescale, stream_type, dsi_crc, dsi_enh_crc, id, dep_id;
	GF_Fraction sar, fps;
	u32 width, height;
	u32 sr, nb_ch;
	const char *lang;
	Bool interlaced;
	const GF_PropertyValue *p_role;
	const GF_PropertyValue *p_period_desc;
	const GF_PropertyValue *p_as_desc;
	const GF_PropertyValue *p_as_any_desc;
	const GF_PropertyValue *p_rep_desc;
	const GF_PropertyValue *p_base_url;
	const char *template;
	const char *xlink;

	//TODO: get the values for all below
	u32 ch_layout, nb_surround, nb_lfe;
	GF_PropVec4i srd;
	u32 view_id;
	//end of TODO

	u32 bitrate;
	GF_DasherPeriod *period;

	Double dash_dur;

	char *period_id;
	Double period_start;
	Double period_dur;
	//0: not done, 1: eos/abort, 2: subdur exceeded
	u32 done;
	Bool seg_done;

	u32 nb_comp, nb_comp_done;

	u32 nb_rep, nb_rep_done;
	Double set_seg_duration;

	//repID for this stream
	char *rep_id;
	struct _dash_stream *muxed_base;
	GF_List *complementary_reps;

	//the one and only representation element
	GF_MPD_Representation *rep;
	//the parent adaptation set
	GF_MPD_AdaptationSet *set;
	Bool owns_set;
	//set to true to use inband params
	Bool inband_params;
	GF_List *multi_pids;
	//in case we share the same init segment, we MUST use the same timescale
	u32 force_timescale;


	u32 startNumber, seg_number;
	Bool rep_init;
	u64 first_cts;

	//target MPD timescale
	u32 mpd_timescale;
	//segment start time in target MPD timescale
	u64 seg_start_time;
	Bool split_set_names;
	u64 max_period_dur;

	GF_Filter *dst_filter;

	const char *src_url;

	char *init_seg, *seg_template;
	u32 nb_sap_3, nb_sap_4;
	u32 pid_id;

	//seg urls not yet handled (waiting for size/index callbacks)
	GF_List *seg_urls;
	//next segment start time in this stream timescale (NOT MPD timescale)
	u64 next_seg_start;
	//adjusted next segment start time in this stream timescale (NOT MPD timescale)
	//the value is the same as next_seg_start until the end of segment is found (SAP)
	//in which case it is adjusted to the SAP time
	u64 adjusted_next_seg_start;

	//force representation time end in this stream timescale (NOT MPD timescale)
	u64 force_rep_end;

	Bool segment_started;
	u64 first_cts_in_seg;
	u64 first_cts_in_next_seg;
	//used for last segment computation of segmentTimeline
	u64 est_first_cts_in_next_seg;
	u64 last_cts;
	u64 cumulated_dur;
	u32 nb_pck;
	u32 seek_to_pck;

	Bool splitable;
	u32 split_dur_next;
} GF_DashStream;


static GF_DasherPeriod *dasher_new_period()
{
	GF_DasherPeriod *period;
	GF_SAFEALLOC(period, GF_DasherPeriod);
	period->streams = gf_list_new();
	return period;
}

static GF_Err dasher_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	Bool period_switch = GF_FALSE;
	const GF_PropertyValue *p;
	u32 dc_crc, dc_enh_crc;
	GF_DashStream *ds;
	GF_DasherCtx *ctx = gf_filter_get_udta(filter);

	if (is_remove) {
		return GF_OK;
	}

	if (!ctx->opid) {
		ctx->opid = gf_filter_pid_new(filter);

		//copy properties at init or reconfig
		gf_filter_pid_copy_properties(ctx->opid, pid);
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_DECODER_CONFIG, NULL);
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_DECODER_CONFIG_ENHANCEMENT, NULL);
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, NULL);
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_UNFRAMED, NULL);
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_FILE) );
		p = gf_filter_pid_caps_query(pid, GF_PROP_PID_FILE_EXT);
		if (p)
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_FILE_EXT, p );
		else
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_FILE_EXT, &PROP_STRING("mpd") );
		gf_filter_pid_set_name(ctx->opid, "manifest" );
	}

	ds = gf_filter_pid_get_udta(pid);
	if (!ds) {
		GF_SAFEALLOC(ds, GF_DashStream);
		ds->ipid = pid;
		gf_list_add(ctx->pids, ds);
		ds->complementary_reps = gf_list_new();
		period_switch = GF_TRUE;
		//don't create pid at this time
	}

#define CHECK_PROP(_type, _mem, _e) \
	p = gf_filter_pid_get_property(pid, _type); \
	if (!p && (_e<=0) ) return _e; \
	if (p && (p->value.uint != _mem) && _mem) period_switch = GF_TRUE; \
	if (p) _mem = p->value.uint; \

#define CHECK_PROP_BOOL(_type, _mem, _e) \
	p = gf_filter_pid_get_property(pid, _type); \
	if (!p && (_e<=0) ) return _e; \
	if (p && (p->value.boolean != _mem) && _mem) period_switch = GF_TRUE; \
	if (p) _mem = p->value.uint; \

#define CHECK_PROP_FRAC(_type, _mem, _e) \
	p = gf_filter_pid_get_property(pid, _type); \
	if (!p && (_e<=0) ) return _e; \
	if (p && (p->value.frac.num * _mem.den != p->value.frac.den * _mem.num) && _mem.den && _mem.num) period_switch = GF_TRUE; \
	if (p) _mem = p->value.frac; \

#define CHECK_PROP_STR(_type, _mem, _e) \
	p = gf_filter_pid_get_property(pid, _type); \
	if (!p && (_e<=0) ) return _e; \
	if (p && _mem && strcmp(_mem, p->value.string)) period_switch = GF_TRUE; \
	if (p) _mem = p->value.string; \

#define CHECK_PROP_PROP(_type, _mem, _e) \
	p = gf_filter_pid_get_property(pid, _type); \
	if (!p && (_e<=0) ) return _e; \
	if (p != _mem) period_switch = GF_TRUE; \
	_mem = p; \

	CHECK_PROP(GF_PROP_PID_STREAM_TYPE, ds->stream_type, GF_NOT_SUPPORTED)

	if (ds->stream_type != GF_STREAM_FILE) {

		CHECK_PROP(GF_PROP_PID_CODECID, ds->codec_id, GF_NOT_SUPPORTED)
		CHECK_PROP(GF_PROP_PID_TIMESCALE, ds->timescale, GF_NOT_SUPPORTED)
		CHECK_PROP(GF_PROP_PID_BITRATE, ds->bitrate, GF_EOS)

		if (ds->stream_type==GF_STREAM_VISUAL) {
			CHECK_PROP(GF_PROP_PID_WIDTH, ds->width, GF_OK)
			CHECK_PROP(GF_PROP_PID_HEIGHT, ds->height, GF_OK)
			//don't return if not defined
			CHECK_PROP_FRAC(GF_PROP_PID_SAR, ds->sar, GF_EOS)
			if (!ds->sar.num) ds->sar.num = ds->sar.den = 1;
			CHECK_PROP_FRAC(GF_PROP_PID_FPS, ds->fps, GF_EOS)
		} else if (ds->stream_type==GF_STREAM_AUDIO) {
			CHECK_PROP(GF_PROP_PID_SAMPLE_RATE, ds->sr, GF_OK)
			CHECK_PROP(GF_PROP_PID_NUM_CHANNELS, ds->nb_ch, GF_OK)
			CHECK_PROP(GF_PROP_PID_CHANNEL_LAYOUT, ds->ch_layout, GF_EOS)
		}

		CHECK_PROP(GF_PROP_PID_ID, ds->id, GF_EOS)
		CHECK_PROP(GF_PROP_PID_DEPENDENCY_ID, ds->dep_id, GF_EOS)

		dc_crc = 0;
		p = gf_filter_pid_get_property(pid, GF_PROP_PID_DECODER_CONFIG);
		if (p) dc_crc = gf_crc_32(p->value.data.ptr, p->value.data.size);
		dc_enh_crc = 0;
		p = gf_filter_pid_get_property(pid, GF_PROP_PID_DECODER_CONFIG_ENHANCEMENT);
		if (p) dc_enh_crc = gf_crc_32(p->value.data.ptr, p->value.data.size);

		if (((dc_crc != ds->dsi_crc) && ds->dsi_crc)
			|| ((dc_enh_crc != ds->dsi_enh_crc) && ds->dsi_enh_crc)
		) {
			//check which codecs can support inband param sets
			switch (ds->codec_id) {
			case GF_CODECID_AVC:
			case GF_CODECID_SVC:
			case GF_CODECID_MVC:
			case GF_CODECID_HEVC:
			case GF_CODECID_LHVC:
				if (!ctx->bs_switch)
					period_switch = GF_TRUE;
				break;
			default:
				period_switch = GF_TRUE;
				break;
			}
		}
		ds->dsi_crc = dc_crc;

		CHECK_PROP_STR(GF_PROP_PID_URL, ds->src_url, GF_EOS)
		CHECK_PROP_STR(GF_PROP_PID_TEMPLATE, ds->template, GF_EOS)
		CHECK_PROP_STR(GF_PROP_PID_LANGUAGE, ds->lang, GF_EOS)
		CHECK_PROP_BOOL(GF_PROP_PID_INTERLACED, ds->interlaced, GF_EOS)
		CHECK_PROP_PROP(GF_PROP_PID_AS_COND_DESC, ds->p_as_desc, GF_EOS)
		CHECK_PROP_PROP(GF_PROP_PID_AS_ANY_DESC, ds->p_as_any_desc, GF_EOS)
		CHECK_PROP_PROP(GF_PROP_PID_REP_DESC, ds->p_rep_desc, GF_EOS)
		CHECK_PROP_PROP(GF_PROP_PID_BASE_URL, ds->p_base_url, GF_EOS)
		CHECK_PROP_PROP(GF_PROP_PID_ROLE, ds->p_role, GF_EOS)

		ds->startNumber = 1;
		CHECK_PROP(GF_PROP_PID_START_NUMBER, ds->startNumber, GF_EOS)
		ds->dash_dur = ctx->dur;
		p = gf_filter_pid_get_property(pid, GF_PROP_PID_DASH_DUR);
		if (p) ds->dash_dur = p->value.number;

		ds->splitable = GF_FALSE;
		switch (ds->stream_type) {
		case GF_STREAM_TEXT:
		case GF_STREAM_METADATA:
		case GF_STREAM_OD:
		case GF_STREAM_SCENE:
			ds->splitable = ctx->split;
			break;
		}
	} else {

		p = gf_filter_pid_get_property(pid, GF_PROP_PID_URL);
		if (!p) p = gf_filter_pid_get_property(pid, GF_PROP_PID_FILEPATH);
		if (p) return GF_NOT_SUPPORTED;

		CHECK_PROP_STR(GF_PROP_PID_XLINK, ds->xlink, GF_EOS)
	}
	CHECK_PROP_STR(GF_PROP_PID_PERIOD_ID, ds->period_id, GF_EOS)
	CHECK_PROP_PROP(GF_PROP_PID_PERIOD_DESC, ds->p_period_desc, GF_EOS)

	ds->period_start = 0;
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_PERIOD_START);
	if (p) ds->period_start = p->value.number;

	ds->period_dur = 0;
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_PERIOD_DUR);
	if (p) ds->period_dur = p->value.number;

	if (ds->stream_type==GF_STREAM_FILE) {
		if (!ds->xlink && !ds->period_start && !ds->period_dur) {
			ds->done = 1;
			GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] null PID specified without any XLINK/start/duration, ignoring\n"));
		} else if (ds->xlink) {
			ctx->use_xlink = GF_TRUE;
		}
	}

	//our stream is already scheduled for next period, don't do anything
	if (gf_list_find(ctx->next_period->streams, ds)>=0)
		period_switch = GF_FALSE;


	//assign default ID
	if (!ds->period_id)
		ds->period_id = "_gpac_dasher_default_period_id";

	if (!period_switch) return GF_OK;
	if (period_switch) {
		gf_list_del_item(ctx->current_period->streams, ds);
		gf_list_add(ctx->next_period->streams, ds);
		ds->period = ctx->next_period;
	}
	return GF_OK;
}

static GF_Err dasher_update_mpd(GF_DasherCtx *ctx)
{
	char profiles_string[GF_MAX_PATH];
	GF_XMLAttribute *cenc_att = NULL;
	GF_XMLAttribute *xlink_att = NULL;
	GF_XMLAttribute *prof_att = NULL;

	u32 i, count=gf_list_count(ctx->mpd->attributes);
	for (i=0; i<count; i++) {
		GF_XMLAttribute * att = gf_list_get(ctx->mpd->attributes, i);
		if (!strcmp(att->name, "profiles")) prof_att = att;
		if (!strcmp(att->name, "xmlns:cenc")) cenc_att = att;
		if (!strcmp(att->name, "xmlns:xlink")) xlink_att = att;

	}
	if (ctx->dynamic) {
		ctx->mpd->type = GF_MPD_TYPE_DYNAMIC;
		ctx->mpd->availabilityStartTime = ctx->generation_start_utc;
	} else {
		ctx->mpd->type = GF_MPD_TYPE_STATIC;
	}

	if (ctx->profile==GF_DASH_PROFILE_LIVE) {
		if (ctx->use_xlink && !ctx->mpeg2) {
			strcpy(profiles_string, "urn:mpeg:dash:profile:isoff-ext-live:2014");
		} else {
			sprintf(profiles_string, "urn:mpeg:dash:profile:%s:2011", ctx->mpeg2 ? "mp2t-simple" : "isoff-live");
		}
	} else if (ctx->profile==GF_DASH_PROFILE_ONDEMAND) {
		if (ctx->use_xlink) {
			strcpy(profiles_string, "urn:mpeg:dash:profile:isoff-ext-on-demand:2014");
		} else {
			strcpy(profiles_string, "urn:mpeg:dash:profile:isoff-on-demand:2011");
		}
	} else if (ctx->profile==GF_DASH_PROFILE_MAIN) {
		sprintf(profiles_string, "urn:mpeg:dash:profile:%s:2011", ctx->mpeg2 ? "mp2t-main" : "isoff-main");
	} else if (ctx->profile==GF_DASH_PROFILE_HBBTV_1_5_ISOBMF_LIVE) {
		strcpy(profiles_string, "urn:hbbtv:dash:profile:isoff-live:2012");
	} else if (ctx->profile==GF_DASH_PROFILE_AVC264_LIVE) {
		strcpy(profiles_string, "urn:mpeg:dash:profile:isoff-live:2011,http://dashif.org/guidelines/dash264");
	} else if (ctx->profile==GF_DASH_PROFILE_AVC264_ONDEMAND) {
		strcpy(profiles_string, "urn:mpeg:dash:profile:isoff-on-demand:2011,http://dashif.org/guidelines/dash264");
	} else {
		strcpy(profiles_string, "urn:mpeg:dash:profile:full:2011");
	}

	if (ctx->profX) {
		char profiles_w_ext[256];
		sprintf(profiles_w_ext, "%s,%s", profiles_string, ctx->profX);
		if (!prof_att) {
			prof_att = gf_xml_dom_create_attribute("profiles", profiles_w_ext);
			gf_list_add(ctx->mpd->attributes, prof_att);
		} else {
			if (prof_att->value) gf_free(prof_att->value);
			prof_att->value = gf_strdup(profiles_w_ext);
		}
	} else {
		if (!prof_att) {
			prof_att = gf_xml_dom_create_attribute("profiles", profiles_string);
			gf_list_add(ctx->mpd->attributes, prof_att);
		} else {
			if (prof_att->value) gf_free(prof_att->value);
			prof_att->value = gf_strdup(profiles_string);
		}
	}

	if (ctx->use_cenc && !cenc_att) {
		cenc_att = gf_xml_dom_create_attribute("xmlns:cenc", "urn:mpeg:cenc:2013");
		gf_list_add(ctx->mpd->attributes, cenc_att);
	}
	if (ctx->use_xlink && !xlink_att) {
		xlink_att = gf_xml_dom_create_attribute("xmlns:xlink", "http://www.w3.org/1999/xlink");
		gf_list_add(ctx->mpd->attributes, xlink_att);
	}

	if (ctx->tsb>=0) ctx->mpd->time_shift_buffer_depth = 1000*ctx->tsb;
	else ctx->mpd->time_shift_buffer_depth = (u32) -1;

	if (ctx->refresh>=0) {
		ctx->mpd->minimum_update_period = 1000*(ctx->refresh ? ctx->refresh : ctx->dur);
		ctx->mpd->media_presentation_duration = 0;
	} else {
		ctx->mpd->minimum_update_period = 0;
		ctx->mpd->media_presentation_duration = (-ctx->refresh) * 1000;
	}
	return GF_OK;
}
static GF_Err dasher_setup_mpd(GF_DasherCtx *ctx)
{
	u32 i, count;
	ctx->mpd = gf_mpd_new();
	ctx->mpd->xml_namespace = "urn:mpeg:dash:schema:mpd:2011";
	ctx->mpd->base_URLs = gf_list_new();
	ctx->mpd->locations = gf_list_new();
	ctx->mpd->program_infos = gf_list_new();
	ctx->mpd->periods = gf_list_new();
	ctx->mpd->attributes = gf_list_new();
	if (ctx->buf<0) {
		s32 buf = -ctx->buf;
		ctx->mpd->min_buffer_time = ctx->dur*10 * buf; //*1000 (ms) / 100 (percent)
	}
	else ctx->mpd->min_buffer_time = ctx->buf;

	if (ctx->for_test)
		ctx->mpd->force_test_mode = GF_TRUE;

	if (ctx->title || ctx->cprt || ctx->info || ctx->source) {
		GF_MPD_ProgramInfo *info;
		GF_SAFEALLOC(info, GF_MPD_ProgramInfo);
		gf_list_add(ctx->mpd->program_infos, info);
		if (ctx->title)
			info->title = gf_strdup(ctx->title);
		else {
			char tmp[256];
			sprintf(tmp,"MPD file generated by GPAC");
			info->title = gf_strdup(tmp);
		}
		if (ctx->cprt) info->copyright = gf_strdup(ctx->cprt);
		if (ctx->info) info->more_info_url = gf_strdup(ctx->info);
		else info->more_info_url = gf_strdup("http://gpac.io");
		if (ctx->source) info->source = gf_strdup(ctx->source);
		if (ctx->lang) info->lang = gf_strdup(ctx->lang);
	}
	count = ctx->location ? gf_list_count(ctx->location) : 0;
	for (i=0; i<count; i++) {
		char *l = gf_list_get(ctx->location, i);
		gf_list_add(ctx->mpd->locations, gf_strdup(l));
	}
	count = ctx->base ? gf_list_count(ctx->base) : 0;
	for (i=0; i<count; i++) {
		GF_MPD_BaseURL *base;
		char *b = gf_list_get(ctx->base, i);
		GF_SAFEALLOC(base, GF_MPD_BaseURL);
		base->URL = gf_strdup(b);
		gf_list_add(ctx->mpd->base_URLs, base);
	}
	return dasher_update_mpd(ctx);
}


static u32 dasher_cicp_get_channel_config(u32 nb_chan,u32 nb_surr, u32 nb_lfe)
{
	if ( !nb_chan && !nb_surr && !nb_lfe) return 0;
	else if ((nb_chan==1) && !nb_surr && !nb_lfe) return 1;
	else if ((nb_chan==2) && !nb_surr && !nb_lfe) return 2;
	else if ((nb_chan==3) && !nb_surr && !nb_lfe) return 3;
	else if ((nb_chan==3) && (nb_surr==1) && !nb_lfe) return 4;
	else if ((nb_chan==3) && (nb_surr==2) && !nb_lfe) return 5;
	else if ((nb_chan==3) && (nb_surr==2) && (nb_lfe==1)) return 6;
	else if ((nb_chan==5) && (nb_surr==0) && (nb_lfe==1)) return 6;

	else if ((nb_chan==5) && (nb_surr==2) && (nb_lfe==1)) return 7;
	else if ((nb_chan==2) && (nb_surr==1) && !nb_lfe) return 9;
	else if ((nb_chan==2) && (nb_surr==2) && !nb_lfe) return 10;
	else if ((nb_chan==3) && (nb_surr==3) && (nb_lfe==1)) return 11;
	else if ((nb_chan==3) && (nb_surr==4) && (nb_lfe==1)) return 12;
	else if ((nb_chan==11) && (nb_surr==11) && (nb_lfe==2)) return 13;
	else if ((nb_chan==5) && (nb_surr==2) && (nb_lfe==1)) return 14;
	else if ((nb_chan==5) && (nb_surr==5) && (nb_lfe==2)) return 15;
	else if ((nb_chan==5) && (nb_surr==4) && (nb_lfe==1)) return 16;
	else if ((nb_surr==5) && (nb_lfe==1) && (nb_chan==6)) return 17;
	else if ((nb_surr==7) && (nb_lfe==1) && (nb_chan==6)) return 18;
	else if ((nb_chan==5) && (nb_surr==6) && (nb_lfe==1)) return 19;
	else if ((nb_chan==7) && (nb_surr==6) && (nb_lfe==1)) return 20;

	GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("Unkown CICP mapping for channel config %d/%d.%d\n", nb_chan, nb_surr, nb_lfe));
	return 0;
}

static GF_Err dasher_get_rfc_6381_codec_name(GF_DasherCtx *ctx, GF_DashStream *ds, char *szCodec, Bool force_inband, Bool force_sbr)
{
	u32 subtype=0;
	const GF_PropertyValue *dcd, *dcd_enh;

	dcd = gf_filter_pid_get_property(ds->ipid, GF_PROP_PID_DECODER_CONFIG);
	dcd_enh = gf_filter_pid_get_property(ds->ipid, GF_PROP_PID_DECODER_CONFIG_ENHANCEMENT);

	switch (ds->codec_id) {
	case GF_CODECID_AAC_MPEG4:
	case GF_CODECID_AAC_MPEG2_MP:
	case GF_CODECID_AAC_MPEG2_LCP:
	case GF_CODECID_AAC_MPEG2_SSRP:
		if (dcd && (!ctx->forcep || !ctx->aacp) ) {
			/*5 first bits of AAC config*/
			u8 audio_object_type = (dcd->value.data.ptr[0] & 0xF8) >> 3;
#ifndef GPAC_DISABLE_AV_PARSERS
			if (force_sbr && (audio_object_type==2) ) {
				GF_M4ADecSpecInfo a_cfg;
				GF_Err e = gf_m4a_get_config(dcd->value.data.ptr, dcd->value.data.size, &a_cfg);
				if (e==GF_OK) {
					if (a_cfg.sbr_sr)
						audio_object_type = a_cfg.sbr_object_type;
					if (a_cfg.has_ps)
						audio_object_type = 29;
				}
			}
#endif
			snprintf(szCodec, RFC6381_CODEC_NAME_SIZE_MAX, "mp4a.%02X.%01d", ds->codec_id, audio_object_type);
			return GF_OK;
		}

		if (ctx->aacp)
			snprintf(szCodec, RFC6381_CODEC_NAME_SIZE_MAX, "mp4a.%s", ctx->aacp);
		else
			snprintf(szCodec, RFC6381_CODEC_NAME_SIZE_MAX, "mp4a.%02X", ds->codec_id);

		if (!ctx->forcep) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_CONTAINER, ("[Dasher] Cannot find AVC config, using default %s\n", szCodec));
		}
		return GF_OK;

		break;
	case GF_CODECID_MPEG4_PART2:
#ifndef GPAC_DISABLE_AV_PARSERS
		if (dcd) {
			GF_M4VDecSpecInfo dsi;
			gf_m4v_get_config(dcd->value.data.ptr, dcd->value.data.size, &dsi);
			snprintf(szCodec, RFC6381_CODEC_NAME_SIZE_MAX, "mp4v.%02X.%01x", ds->codec_id, dsi.VideoPL);
		} else
#endif
		{
			snprintf(szCodec, RFC6381_CODEC_NAME_SIZE_MAX, "mp4v.%02X", ds->codec_id);
		}
		break;
	case GF_CODECID_SVC:
	case GF_CODECID_MVC:
		if (dcd_enh) dcd = dcd_enh;
		subtype = (ds->codec_id==GF_CODECID_SVC) ? GF_ISOM_SUBTYPE_SVC_H264 : GF_ISOM_SUBTYPE_MVC_H264;
	case GF_CODECID_AVC:
		if (!subtype) {
			if (force_inband) {
				subtype = dcd_enh ? GF_ISOM_SUBTYPE_AVC4_H264 : GF_ISOM_SUBTYPE_AVC3_H264;
			} else {
				subtype = dcd_enh ? GF_ISOM_SUBTYPE_AVC2_H264 : GF_ISOM_SUBTYPE_AVC_H264;
			}
		}

		if (dcd && (!ctx->forcep || !ctx->avcp) ) {
			GF_AVCConfig *avcc = gf_odf_avc_cfg_read(dcd->value.data.ptr, dcd->value.data.size);
			if (avcc) {
				snprintf(szCodec, RFC6381_CODEC_NAME_SIZE_MAX, "%s.%02X%02X%02X", gf_4cc_to_str(subtype), avcc->AVCProfileIndication, avcc->profile_compatibility, avcc->AVCLevelIndication);
				gf_odf_avc_cfg_del(avcc);
				return GF_OK;
			}
		}
		if (ctx->avcp)
			snprintf(szCodec, RFC6381_CODEC_NAME_SIZE_MAX, "%s.%s", gf_4cc_to_str(subtype), ctx->avcp);
		else
			snprintf(szCodec, RFC6381_CODEC_NAME_SIZE_MAX, "%s", gf_4cc_to_str(subtype));
		if (!ctx->forcep) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_CONTAINER, ("[Dasher] Cannot find AVC config, using default %s\n", szCodec));
		}
		return GF_OK;
#ifndef GPAC_DISABLE_HEVC
	case GF_CODECID_LHVC:
		if (dcd_enh) dcd = dcd_enh;
		subtype = force_inband ? GF_ISOM_SUBTYPE_LHE1 : GF_ISOM_SUBTYPE_LHV1;
	case GF_CODECID_HEVC:
		if (!subtype) {
			if (dcd_enh) {
				subtype = force_inband ? GF_ISOM_SUBTYPE_HEV2 : GF_ISOM_SUBTYPE_HVC2;
			} else {
				subtype = force_inband ? GF_ISOM_SUBTYPE_HEV1 : GF_ISOM_SUBTYPE_HVC1;
			}
		}
		if (dcd && (!ctx->forcep || !ctx->hvcp)) {
			u8 c;
			char szTemp[RFC6381_CODEC_NAME_SIZE_MAX];
			GF_HEVCConfig *hvcc = gf_odf_hevc_cfg_read(dcd->value.data.ptr, dcd->value.data.size, (dcd==dcd_enh) ? GF_TRUE : GF_FALSE);
			//TODO - check we do expose hvcC for tiled tracks !

			snprintf(szCodec, RFC6381_CODEC_NAME_SIZE_MAX, "%s.", gf_4cc_to_str(subtype));
			if (hvcc->profile_space==1) strcat(szCodec, "A");
			else if (hvcc->profile_space==2) strcat(szCodec, "B");
			else if (hvcc->profile_space==3) strcat(szCodec, "C");
			//profile idc encoded as a decimal number
			sprintf(szTemp, "%d", hvcc->profile_idc);
			strcat(szCodec, szTemp);
			//general profile compatibility flags: hexa, bit-reversed
			{
				u32 val = hvcc->general_profile_compatibility_flags;
				u32 i, res = 0;
				for (i=0; i<32; i++) {
					res |= val & 1;
					if (i==31) break;
					res <<= 1;
					val >>=1;
				}
				sprintf(szTemp, ".%X", res);
				strcat(szCodec, szTemp);
			}

			if (hvcc->tier_flag) strcat(szCodec, ".H");
			else strcat(szCodec, ".L");
			sprintf(szTemp, "%d", hvcc->level_idc);
			strcat(szCodec, szTemp);

			c = hvcc->progressive_source_flag << 7;
			c |= hvcc->interlaced_source_flag << 6;
			c |= hvcc->non_packed_constraint_flag << 5;
			c |= hvcc->frame_only_constraint_flag << 4;
			c |= (hvcc->constraint_indicator_flags >> 40);
			sprintf(szTemp, ".%X", c);
			strcat(szCodec, szTemp);
			if (hvcc->constraint_indicator_flags & 0xFFFFFFFF) {
				c = (hvcc->constraint_indicator_flags >> 32) & 0xFF;
				sprintf(szTemp, ".%X", c);
				strcat(szCodec, szTemp);
				if (hvcc->constraint_indicator_flags & 0x00FFFFFF) {
					c = (hvcc->constraint_indicator_flags >> 24) & 0xFF;
					sprintf(szTemp, ".%X", c);
					strcat(szCodec, szTemp);
					if (hvcc->constraint_indicator_flags & 0x0000FFFF) {
						c = (hvcc->constraint_indicator_flags >> 16) & 0xFF;
						sprintf(szTemp, ".%X", c);
						strcat(szCodec, szTemp);
						if (hvcc->constraint_indicator_flags & 0x000000FF) {
							c = (hvcc->constraint_indicator_flags >> 8) & 0xFF;
							sprintf(szTemp, ".%X", c);
							strcat(szCodec, szTemp);
							c = (hvcc->constraint_indicator_flags ) & 0xFF;
							sprintf(szTemp, ".%X", c);
							strcat(szCodec, szTemp);
						}
					}
				}
			}
			gf_odf_hevc_cfg_del(hvcc);
			return GF_OK;
		}

		if (ctx->hvcp)
			snprintf(szCodec, RFC6381_CODEC_NAME_SIZE_MAX, "%s.%s", gf_4cc_to_str(subtype), ctx->hvcp);
		else
			snprintf(szCodec, RFC6381_CODEC_NAME_SIZE_MAX, "%s", gf_4cc_to_str(subtype));
		if (!ctx->forcep) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_CONTAINER, ("[Dasher] Cannot find HEVC config, using default %s\n", szCodec));
		}
		return GF_OK;
#endif

	default:
		subtype = gf_codecid_4cc_type(ds->codec_id);
		if (!subtype) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_AUTHOR, ("[Dasher] codec parameters not known, cannot set codec string\n" ));
			strcpy(szCodec, "unkn");
			return GF_OK;
		}
		if (ds->codec_id<GF_CODECID_LAST_MPEG4_MAPPING) {
			if (ds->stream_type==GF_STREAM_VISUAL) {
				snprintf(szCodec, RFC6381_CODEC_NAME_SIZE_MAX, "mp4v.%02X", ds->codec_id);
			} else if (ds->stream_type==GF_STREAM_AUDIO) {
				snprintf(szCodec, RFC6381_CODEC_NAME_SIZE_MAX, "mp4a.%02X", ds->codec_id);
			} else {
				snprintf(szCodec, RFC6381_CODEC_NAME_SIZE_MAX, "mp4s.%02X", ds->codec_id);
			}
		} else {
			GF_LOG(GF_LOG_WARNING, GF_LOG_AUTHOR, ("[Dasher] codec parameters not known - setting codecs string to default value \"%s\"\n", gf_4cc_to_str(subtype) ));
			snprintf(szCodec, RFC6381_CODEC_NAME_SIZE_MAX, "%s", gf_4cc_to_str(subtype));
		}
	}
	return GF_OK;
}

static void dasher_setup_rep(GF_DasherCtx *ctx, GF_DashStream *ds)
{
	char szCodec[RFC6381_CODEC_NAME_SIZE_MAX];
	const GF_PropertyValue *p;
	assert(ds->rep==NULL);
	ds->rep = gf_mpd_representation_new();
	ds->rep->playback.udta = ds;

	ds->rep->bandwidth = ds->bitrate;
	if (ds->stream_type==GF_STREAM_VISUAL) {
		ds->rep->width = ds->width;
		ds->rep->height = ds->height;
		ds->rep->mime_type = gf_strdup("video/mp4");
	}
	else if (ds->stream_type==GF_STREAM_AUDIO) {
		GF_MPD_Descriptor *desc;
		char value[256];
		ds->rep->samplerate = ds->sr;
		if (!ds->nb_surround && !ds->nb_lfe) {
			sprintf(value, "%d", ds->nb_ch);
			desc = gf_mpd_descriptor_new(NULL, "urn:mpeg:dash:23003:3:audio_channel_configuration:2011", value);
		} else {
			sprintf(value, "%d", dasher_cicp_get_channel_config(ds->nb_ch, ds->nb_surround, ds->nb_lfe));
			desc = gf_mpd_descriptor_new(NULL, "urn:mpeg:mpegB:cicp:ChannelConfiguration", value);
		}
		gf_list_add(ds->rep->audio_channels, desc);
		ds->rep->mime_type = gf_strdup("audio/mp4");
	} else {
		ds->rep->mime_type = gf_strdup("application/mp4");
	}
	dasher_get_rfc_6381_codec_name(ctx, ds, szCodec, (ctx->bs_switch==DASHER_BS_SWITCH_INBAND) ? GF_TRUE : GF_FALSE, GF_TRUE);
	ds->rep->codecs = gf_strdup(szCodec);

	p = gf_filter_pid_get_property(ds->ipid, GF_PROP_PID_REP_ID);
	if (p) {
		if (ds->rep_id) gf_free(ds->rep_id);
		ds->rep_id = gf_strdup(p->value.string);
	} else if (!ds->rep_id) {
		char szRepID[20];
		sprintf(szRepID, "%d", 1 + gf_list_find(ctx->pids, ds));
		ds->rep_id = gf_strdup(szRepID);
	}
	ds->rep->id = gf_strdup(ds->rep_id);

	if (ds->interlaced) ds->rep->scan_type = GF_MPD_SCANTYPE_INTERLACED;
}


static Bool dasher_same_roles(GF_DashStream *ds1, GF_DashStream *ds2)
{
	GF_List *list;
	if (ds1->p_role && ds2->p_role) {
		if (gf_props_equal(ds1->p_role, ds2->p_role)) return GF_TRUE;
	}
	if (!ds1->p_role && !ds2->p_role)
		return GF_TRUE;

	//special case, if one is set and the other is not, compare with "main" role
	list = ds2->p_role ?  ds2->p_role->value.string_list : ds1->p_role->value.string_list;
	if (gf_list_count(list)==1) {
		char *s = gf_list_get(list, 0);
		if (!strcmp(s, "main")) return GF_TRUE;
	}
	return GF_FALSE;
}

static Bool dasher_same_adaptation_set(GF_DasherCtx *ctx, GF_DashStream *ds, GF_DashStream *ds_test)
{
	//muxed representations
	if (ds_test->muxed_base == ds) return GF_TRUE;
	//otherwise we have to be of same type
	if (ds->stream_type != ds_test->stream_type) return GF_FALSE;

	//not the same roles
	if (!dasher_same_roles(ds, ds_test)) return GF_FALSE;

	/* if two inputs don't have the same (number and value) as_desc they don't belong to the same AdaptationSet
	   (use c_as_desc for AdaptationSet descriptors common to all inputs in an AS) */
	if (!ds->p_as_desc && ds_test->p_as_desc)
		return GF_FALSE;
	if (ds->p_as_desc && !ds_test->p_as_desc)
		return GF_FALSE;
	if (ds->p_as_desc && ! gf_props_equal(ds->p_as_desc, ds_test->p_as_desc))
		return GF_FALSE;

	if (ctx->align) {
		if (ds->dash_dur != ds_test->dash_dur) return GF_FALSE;
	}

	if (ds->srd.x != ds_test->srd.x) return GF_FALSE;
	if (ds->srd.y != ds_test->srd.y) return GF_FALSE;
	if (ds->srd.z != ds_test->srd.z) return GF_FALSE;
	if (ds->srd.w != ds_test->srd.w) return GF_FALSE;

	if (ds->view_id != ds_test->view_id) return GF_FALSE;
	//according to DASH spec mixing interlaced and progressive is OK
	//if (ds->interlaced != ds_test->interlaced) return GF_FALSE;
	if (ds->nb_ch != ds_test->nb_ch) return GF_FALSE;
	if (ds->lang != ds_test->lang) return GF_FALSE;

	if (ds->stream_type==GF_STREAM_VISUAL) {
		u32 w, h, tw, th;
		if (ctx->no_sar) {
			w = ds->width;
			h = ds->height;
			tw = ds_test->width;
			th = ds_test->height;
		} else {
			w = ds->width * ds->sar.num;
			h = ds->height * ds->sar.den;
			tw = ds_test->width * ds_test->sar.num;
			th = ds_test->height * ds_test->sar.den;
		}

		//not the same aspect ratio
		if (w * th != h * tw)
			return GF_FALSE;
	} else if (ds->stream_type==GF_STREAM_AUDIO) {
		if (!ctx->mix_codecs && (ds->codec_id != ds_test->codec_id) )
			return GF_FALSE;
		//we allow mix of channels config
	} else {
		if (!ctx->mix_codecs && strcmp(ds->rep->codecs, ds_test->rep->codecs)) return GF_FALSE;
		return GF_TRUE;
	}
	//ok, we are video or audio with mixed codecs
	if (ctx->mix_codecs) return GF_TRUE;
	//we need dependencies
	if (ds_test->dep_id && gf_list_find(ds->complementary_reps, ds_test->rep) < 0)
		return GF_FALSE;
	//we should be good
	return GF_TRUE;
}

static void dasher_add_descriptors(GF_List **p_dst_list, const GF_PropertyValue *desc_val)
{
	u32 j, count;
	GF_List *dst_list;
	if (!desc_val) return;
	if (desc_val->type != GF_PROP_STRING_LIST) return;
	count = gf_list_count(desc_val->value.string_list);
	if (!count) return;
	if ( ! (*p_dst_list)) *p_dst_list = gf_list_new();
	dst_list = *p_dst_list;
	for (j=0; j<count; j++) {
		char *desc = gf_list_get(desc_val->value.string_list, j);
		if (desc[0] == '<') {
			GF_MPD_other_descriptors *d;
			GF_SAFEALLOC(d, GF_MPD_other_descriptors);
			d->xml_desc = gf_strdup(desc);
			gf_list_add(dst_list, d);
		} else {
			GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] Invalid descriptor %s, expecting '<' as first character\n", desc));
		}
	}
}

static void dasher_setup_set_defaults(GF_DasherCtx *ctx, GF_MPD_AdaptationSet *set)
{
	u32 i, count;
	Bool main_role_set = GF_FALSE;
	//by default setup alignment
	if (ctx->single_segment) set->subsegment_alignment = ctx->align;
	else set->segment_alignment = ctx->align;

	//startWithSAP is set when the first packet comes in

	//the rest depends on the various profiles/iop, to check
	count = gf_list_count(set->representations);
	for (i=0; i<count; i++) {
		GF_MPD_Representation *rep = gf_list_get(set->representations, i);
		GF_DashStream *ds = rep->playback.udta;

		if (set->max_width < ds->width) set->max_width = ds->width;
		if (set->max_height < ds->height) set->max_height = ds->height;
/*		if (set->max_bandwidth < ds->rep->bandwidth) set->max_bandwidth = ds->rep->bandwidth;
		if (set->max_framerate * ds->fps.den < ds->fps.num) set->max_framerate = (u32) (ds->fps.num / ds->fps.den);
*/

		/*set role*/
		if (ds->p_role) {
			u32 j, count;
			count = gf_list_count(ds->p_role->value.string_list);
			for (j=0; j<count; j++) {
				char *role = gf_list_get(ds->p_role->value.string_list, j);
				GF_MPD_Descriptor *desc;
				char *uri;
				if (!strcmp(role, "caption") || !strcmp(role, "subtitle") || !strcmp(role, "main")
			        || !strcmp(role, "alternate") || !strcmp(role, "supplementary") || !strcmp(role, "commentary")
			        || !strcmp(role, "dub") || !strcmp(role, "description") || !strcmp(role, "sign")
					 || !strcmp(role, "metadata") || !strcmp(role, "enhanced-audio- intelligibility")
				) {
					uri = "urn:mpeg:dash:role:2011";
					if (!strcmp(role, "main")) main_role_set = GF_TRUE;
				} else {
					GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] Unrecognized role %s - using GPAC urn for schemaID\n", role));
					uri = "urn:gpac:dash:role:2013";
				}
				desc = gf_mpd_descriptor_new(NULL, uri, role);
				gf_list_add(set->role, desc);
			}
		}
	}
	if (ctx->check_main_role && !main_role_set) {
		GF_MPD_Descriptor *desc;
		desc = gf_mpd_descriptor_new(NULL, "urn:mpeg:dash:role:2011", "main");
		gf_list_add(set->role, desc);
	}
}

static void dasher_check_bitstream_swicthing(GF_DasherCtx *ctx, GF_MPD_AdaptationSet *set)
{
	u32 i, j, count;
	Bool use_inband = (ctx->bs_switch==DASHER_BS_SWITCH_INBAND) ? GF_TRUE : GF_FALSE;
	Bool use_multi = (ctx->bs_switch==DASHER_BS_SWITCH_MULTI) ? GF_TRUE : GF_FALSE;
	GF_MPD_Representation *base_rep = gf_list_get(set->representations, 0);
	GF_DashStream *base_ds;

	if (ctx->bs_switch==DASHER_BS_SWITCH_OFF) return;
	if (!base_rep) return;
	base_ds = base_rep->playback.udta;

	count = gf_list_count(set->representations);
	if (count==1) {
		if (ctx->bs_switch==DASHER_BS_SWITCH_FORCE) set->bitstream_switching=GF_TRUE;
		else if (ctx->bs_switch==DASHER_BS_SWITCH_INBAND) {
			GF_DashStream *ds = base_rep->playback.udta;
			ds->inband_params = GF_TRUE;
		}
		return;
	}

	for (i=1; i<count; i++) {
		GF_MPD_Representation *rep = gf_list_get(set->representations, i);
		GF_DashStream *ds = rep->playback.udta;
		//same codec ID
		if (ds->codec_id == base_ds->codec_id) {
			//we will use inband params, so bs switching is OK
			if (use_inband || use_multi) continue;
			//we consider we can switch in non-inband only if we have same CRC for the decoder config
			if (base_ds->dsi_crc == ds->dsi_crc) continue;
			//not the same config, no BS switching
			return;
		}
		//dependencies / different codec IDs, cannot use bitstream switching
		return;
	}
	//ok we can use BS switching, ensure we use the same timescale for every stream
	set->bitstream_switching = GF_TRUE;

	for (i=0; i<count; i++) {
		GF_MPD_Representation *rep = gf_list_get(set->representations, i);
		GF_DashStream *ds = rep->playback.udta;
		for (j=i+1; j<count; j++) {
			GF_DashStream *a_ds;
			rep = gf_list_get(set->representations, j);
			a_ds = rep->playback.udta;
			if (a_ds->stream_type != ds->stream_type) continue;
			if (a_ds->timescale != ds->timescale)
				a_ds->force_timescale = ds->timescale;
		}
	}
}

static void dasher_open_destination(GF_Filter *filter, GF_DasherCtx *ctx, GF_MPD_Representation *rep, const char *szInitURL, Bool trash_init)
{
	GF_Err e;
	Bool has_frag=GF_FALSE;
	Bool has_subs=GF_FALSE;
	const char *dst_args;
	char szDST[GF_MAX_PATH];
	char szSRC[100];

	GF_DashStream *ds = rep->playback.udta;
	if (ds->muxed_base) return;

	strcpy(szDST, szInitURL);
	if (ctx->out_path) {
		char *rel = gf_url_concatenate(ctx->out_path, szInitURL);
		if (rel) {
			strcpy(szDST, rel);
			gf_free(rel);
		}
	}

	dst_args = gf_filter_get_dst_args(filter);
	if (dst_args) {
		char szKey[20];
		sprintf(szSRC, "%c", gf_filter_get_sep(filter, GF_FS_SEP_ARGS));
		strcat(szDST, szSRC);
		strcat(szDST, dst_args);
		//look for frag arg
		sprintf(szKey, "%cfrag", gf_filter_get_sep(filter, GF_FS_SEP_ARGS));
		if (strstr(dst_args, szKey)) has_frag = GF_TRUE;
		else {
			sprintf(szKey, "%csfrag", gf_filter_get_sep(filter, GF_FS_SEP_ARGS));
			if (strstr(dst_args, szKey)) has_frag = GF_TRUE;
		}
		//look for subs_sidx arg
		sprintf(szKey, "%csubs_sidx", gf_filter_get_sep(filter, GF_FS_SEP_ARGS));
		if (strstr(dst_args, szKey)) has_subs = GF_TRUE;
	}
	if (trash_init) {
		sprintf(szSRC, "%cnoinit", gf_filter_get_sep(filter, GF_FS_SEP_ARGS));
		strcat(szDST, szSRC);
	}
	if (!has_frag) {
		sprintf(szSRC, "%cfrag", gf_filter_get_sep(filter, GF_FS_SEP_ARGS));
		strcat(szDST, szSRC);
	}
	if (!has_subs && ctx->single_segment) {
		sprintf(szSRC, "%csubs_sidx%c0", gf_filter_get_sep(filter, GF_FS_SEP_ARGS), gf_filter_get_sep(filter, GF_FS_SEP_NAME));
		strcat(szDST, szSRC);
	}
	//override xps inband declaration in args
	sprintf(szSRC, "%cxps_inband%c%s", gf_filter_get_sep(filter, GF_FS_SEP_ARGS), gf_filter_get_sep(filter, GF_FS_SEP_NAME), ds->inband_params ? "all" : "no");
	strcat(szDST, szSRC);

	if (ctx->no_fragments_defaults) {
		sprintf(szSRC, "%cno_frags_def", gf_filter_get_sep(filter, GF_FS_SEP_ARGS) );
		strcat(szDST, szSRC);
	}

	ds->dst_filter = gf_filter_connect_destination(filter, szDST, &e);
	if (e) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_DASH, ("[Dasher] Couldn't create output file %s: %s\n", szInitURL, gf_error_to_string(e) ));
		return;
	}
	sprintf(szSRC, "MuxSrc%cdasher_%p", gf_filter_get_sep(filter, GF_FS_SEP_NAME), ds->dst_filter);
	//assigne sourceID to be this
	gf_filter_set_source(ds->dst_filter, filter, szSRC);
}

static void dasher_open_pid(GF_Filter *filter, GF_DasherCtx *ctx, GF_DashStream *ds, GF_List *multi_pids)
{
	GF_DashStream *base_ds = ds->muxed_base ? ds->muxed_base : ds;
	char szSRC[1024];
	assert(!ds->opid);
	assert(base_ds->dst_filter);

	sprintf(szSRC, "dasher_%p", base_ds->dst_filter);
	ds->opid = gf_filter_pid_new(filter);
	gf_filter_pid_copy_properties(ds->opid, ds->ipid);

	//set init filename
	if (ds->init_seg)
		gf_filter_pid_set_property(ds->opid, GF_PROP_PID_OUTPATH, &PROP_STRING(ds->init_seg));

	//force PID ID
	gf_filter_pid_set_property(ds->opid, GF_PROP_PID_ID, &PROP_UINT(ds->pid_id) );
	gf_filter_pid_set_info(ds->opid, GF_PROP_PID_MUX_SRC, &PROP_STRING(szSRC) );
	gf_filter_pid_set_info(ds->opid, GF_PROP_PID_DASH_MODE, &PROP_UINT(ctx->single_segment ? 2 : 1) );
	gf_filter_pid_set_info(ds->opid, GF_PROP_PID_DASH_DUR, &PROP_DOUBLE(ds->dash_dur) );

	gf_filter_pid_force_cap(ds->opid, GF_PROP_PID_DASH_MODE);

	/*timescale forced (bitstream switching) */
	if (ds->force_timescale)
		gf_filter_pid_set_property(ds->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(ds->force_timescale) );

	if (multi_pids) {
		s32 idx = 1+gf_list_find(multi_pids, ds->ipid);
		assert(idx>0);
		gf_filter_pid_set_property(ds->opid, GF_PROP_PID_DASH_MULTI_PID, &PROP_POINTER(multi_pids) );
		gf_filter_pid_set_property(ds->opid, GF_PROP_PID_DASH_MULTI_PID_IDX, &PROP_UINT(idx) );
	}
}

static Bool dasher_template_use_source_url(const char *template)
{
	if (strstr(template, "$File$") != NULL) return GF_TRUE;
	else if (strstr(template, "$FSRC$") != NULL) return GF_TRUE;
	else if (strstr(template, "$SourcePath$") != NULL) return GF_TRUE;
	else if (strstr(template, "$FURL$") != NULL) return GF_TRUE;
	else if (strstr(template, "$URL$") != NULL) return GF_TRUE;
	return GF_FALSE;
}

static void dasher_set_content_components(GF_DashStream *ds)
{
	GF_MPD_ContentComponent *component;
	GF_DashStream *base_ds = ds->muxed_base ? ds->muxed_base : ds;

	GF_SAFEALLOC(component, GF_MPD_ContentComponent);
	component->id = ds->id;
	switch (ds->stream_type) {
	case GF_STREAM_TEXT:
		component->type = gf_strdup("text");
		break;
	case GF_STREAM_VISUAL:
		component->type = gf_strdup("video");
		break;
	case GF_STREAM_AUDIO:
		component->type = gf_strdup("audio");
		break;
	case GF_STREAM_SCENE:
	case GF_STREAM_OD:
	default:
		component->type = gf_strdup("application");
		break;
	}
	/*if lang not specified at adaptationSet level, put it here*/
	if (!base_ds->set->lang && ds->lang && strcmp(ds->lang, "und")) {
		component->lang = gf_strdup(ds->lang);
	}
	gf_list_add(base_ds->set->content_component, component);
}

static void dasher_setup_sources(GF_Filter *filter, GF_DasherCtx *ctx, GF_MPD_AdaptationSet *set)
{
	char szDASHTemplate[GF_MAX_PATH];
	char szTemplate[GF_MAX_PATH];
	char szSegmentName[GF_MAX_PATH];
	char szInitSegmentName[GF_MAX_PATH];
	const char *template;
	Bool single_template = GF_TRUE;
	GF_MPD_Representation *rep = gf_list_get(set->representations, 0);
	GF_DashStream *ds = rep->playback.udta;
	u32 i, j, count, nb_base;
	GF_List *multi_pids = NULL;
	u32 set_timescale = 0;
	Bool init_template_done=GF_FALSE;
	Bool use_inband = (ctx->bs_switch==DASHER_BS_SWITCH_INBAND) ? GF_TRUE : GF_FALSE;

	count = gf_list_count(set->representations);

	assert(ctx->template);
	template = ((GF_DashStream *)set->udta)->template;

	for (i=0; i<count; i++) {
		rep = gf_list_get(set->representations, i);
		ds = rep->playback.udta;
		if (!ds->template && !template) {}
		else if (ds->template && !strcmp(ds->template, template) ) {
		} else {
			single_template = GF_FALSE;
		}
		if (ds->template && dasher_template_use_source_url(ds->template) ) {
			single_template = GF_FALSE;
			ctx->template_use_source = GF_TRUE;
		}
	}
	if (!template) template = ctx->template;

	if (single_template) {
		if (count==1) single_template = GF_TRUE;
		//for regular reps, if we depend on filename we cannot mutualize the template
		else if (dasher_template_use_source_url(template) ) {
			single_template = GF_FALSE;
			ctx->template_use_source = GF_TRUE;
		}
		//and for scalable reps, if we don't have bandwidth /repID we cannot mutualize the template
		else if (ds->complementary_reps) {
			if (strstr(template, "$Bandwidth$") != NULL) single_template = GF_FALSE;
			else if (strstr(template, "$RepresentationId$") != NULL) single_template = GF_FALSE;
		}
	}

	if (ctx->timescale>0) set_timescale = ctx->timescale;
	else if (ctx->timescale<0) {
		u32 first_timescale;
		rep = gf_list_get(set->representations, 0);
		ds = rep->playback.udta;
		first_timescale = ds->timescale;
		for (i=1; i<count; i++) {
			rep = gf_list_get(set->representations, i);
			ds = rep->playback.udta;
			if (ds->timescale != first_timescale) {
				//we cannot use a single template if enforcing timescales which are not identical
				single_template = GF_FALSE;
				break;
			}
		}
	}

	//assign PID IDs - we assume only one component of a given media type per adaptation set
	//and assign the same PID ID for each component of the same type
	//we could refine this using roles, but most HAS solutions don't use roles at the mulitplexed level
	for (i=0; i<count; i++) {
		u32 j;
		rep = gf_list_get(set->representations, i);
		ds = rep->playback.udta;
		if (ds->pid_id) continue;
		ds->pid_id = gf_list_find(ctx->pids, ds) + 1;

		for (j=i+1; j<count; j++) {
			GF_DashStream *a_ds;
			rep = gf_list_get(set->representations, j);
			a_ds = rep->playback.udta;
			if (a_ds->pid_id) continue;
			if (a_ds->stream_type == ds->stream_type) a_ds->pid_id = ds->pid_id;
		}
	}
	//this is crude because we don't copy the properties, we just pass a list of pids to the destination muxer !!
	//we should cleanup one of these days
	if (set->bitstream_switching && (ctx->bs_switch==DASHER_BS_SWITCH_MULTI)) {
		multi_pids = gf_list_new();
		for (i=0; i<count; i++) {
			rep = gf_list_get(set->representations, i);
			ds = rep->playback.udta;
			if (ds->owns_set) ds->multi_pids = multi_pids;
			gf_list_add(multi_pids, ds->ipid);
		}
	}

	for (i=0; i<count; i++) {
		GF_Err e;
		u32 init_template_mode = GF_DASH_TEMPLATE_INITIALIZATION_TEMPLATE;
		rep = gf_list_get(set->representations, i);
		ds = rep->playback.udta;

		//remove representations for streams muxed with others, but still open the output
		if (ds->muxed_base) {
			GF_DashStream *ds_set = set->udta;
			gf_list_rem(set->representations, i);
			i--;
			count--;
			assert(ds_set->nb_rep);
			ds_set->nb_rep--;
			assert(ds->muxed_base->dst_filter);
			gf_list_transfer(ds->muxed_base->rep->audio_channels, rep->audio_channels);
			gf_list_transfer(ds->muxed_base->rep->base_URLs, rep->base_URLs);
			gf_list_transfer(ds->muxed_base->rep->content_protection , rep->content_protection);
			gf_list_transfer(ds->muxed_base->rep->essential_properties , rep->essential_properties);
			gf_list_transfer(ds->muxed_base->rep->frame_packing , rep->frame_packing);
			gf_list_transfer(ds->muxed_base->rep->other_descriptors , rep->other_descriptors);
			gf_list_transfer(ds->muxed_base->rep->supplemental_properties , rep->supplemental_properties);

			gf_mpd_representation_free(ds->rep);
			ds->rep = NULL;

			if (!gf_list_count(ds->set->content_component)) {
				dasher_set_content_components(ds->muxed_base);
			}
			dasher_set_content_components(ds);
			assert(!multi_pids);
			//open PID
			dasher_open_pid(filter, ctx, ds, NULL);
			continue;
		}
		if (ds->template) strcpy(szTemplate, ds->template);
		else strcpy(szTemplate, ctx->template);

		if (use_inband) ds->inband_params = GF_TRUE;

		//if bitstream switching and templating, only set for the first one
		if (i && set->bitstream_switching && ctx->stl && single_template) continue;

		if (!set_timescale) set_timescale = ds->timescale;

		if (ctx->timescale<0) ds->mpd_timescale = ds->timescale;
		else ds->mpd_timescale = set_timescale;

		//resolve segment template
		e = gf_filter_pid_resolve_file_template(ds->ipid, szTemplate, szDASHTemplate, 0);
		if (e) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] Cannot resolve template name, cannot derive output segment names, disabling rep %s\n", ds->src_url));
			ds->done = 1;
			continue;
		}
		if (single_template && ds->split_set_names) {
			char szStrName[20];
			sprintf(szStrName, "_set%d", 1 + gf_list_find(ctx->current_period->period->adaptation_sets, set)  );
			strcat(szDASHTemplate, szStrName);
			//don't bother forcing an "init" since we rename the destinations
			init_template_mode = GF_DASH_TEMPLATE_INITIALIZATION_TEMPLATE_SKIPINIT;
		}

		//get final segment template - output file name is NULL, we already have solved this
		gf_media_mpd_format_segment_name(GF_DASH_TEMPLATE_TEMPLATE, set->bitstream_switching, szSegmentName, NULL, ds->rep_id, NULL, szDASHTemplate, (ctx->ext && !stricmp(ctx->ext, "null")) ? NULL : ctx->ext, 0, 0, 0, ctx->stl);

		//get final init name - output file name is NULL, we already have solved this
		gf_media_mpd_format_segment_name(init_template_mode, set->bitstream_switching, szInitSegmentName, NULL, ds->rep_id, NULL, szDASHTemplate, (ctx->ext && !stricmp(ctx->ext, "null")) ? NULL : "mp4", 0, 0, 0, ctx->stl);

		ds->init_seg = gf_strdup(szInitSegmentName);
		ds->seg_template = gf_strdup(szSegmentName);

		/* baseURLs */
		nb_base = ds->p_base_url ? gf_list_count(ds->p_base_url->value.string_list) : 0;
		for (j=0; j<nb_base; j++) {
			GF_MPD_BaseURL *base_url;
			char *url = gf_list_get(ds->p_base_url->value.string_list, j);
			GF_SAFEALLOC(base_url, GF_MPD_BaseURL);
			base_url->URL = gf_strdup(url);
			gf_list_add(rep->base_URLs, base_url);
		}

		//we use segment template
		if (ctx->tpl) {
			GF_MPD_SegmentTemplate *seg_template;
			//bs switching but multiple templates
			if ((count==1) || (!i && (set->bitstream_switching || single_template) )) {
				init_template_done = GF_TRUE;
				GF_SAFEALLOC(seg_template, GF_MPD_SegmentTemplate);
				seg_template->initialization = gf_strdup(szInitSegmentName);
				dasher_open_destination(filter, ctx, rep, seg_template->initialization, GF_FALSE);

				if (single_template) {
					seg_template->media = gf_strdup(szSegmentName);
					seg_template->timescale = ds->mpd_timescale;
					seg_template->start_number = ds->startNumber ? ds->startNumber : 1;
					seg_template->duration = (u64)(ds->dash_dur * ds->mpd_timescale);
					if (ctx->asto < 0) {
						seg_template->availability_time_offset = - (Double) ctx->asto / 1000.0;
					}
				} else {
					seg_template->start_number = (u32)-1;

				}
				set->segment_template = seg_template;
			}
			if (i || !single_template) {
				GF_SAFEALLOC(seg_template, GF_MPD_SegmentTemplate);
				if (!init_template_done) {
					seg_template->initialization = gf_strdup(szInitSegmentName);
					dasher_open_destination(filter, ctx, rep, seg_template->initialization, GF_FALSE);
				} else if (i) {
					dasher_open_destination(filter, ctx, rep, szInitSegmentName, GF_TRUE);
				}
				seg_template->media = gf_strdup(szSegmentName);
				seg_template->duration = (u64)(ds->dash_dur * ds->mpd_timescale);
				seg_template->timescale = ds->mpd_timescale;
				seg_template->start_number = ds->startNumber ? ds->startNumber : 1;
				if (ctx->asto < 0) {
					seg_template->availability_time_offset = - (Double) ctx->asto / 1000.0;
				}
				rep->segment_template = seg_template;
			}
		}
		/*we are using a single file or segment, use base url*/
		else if (ctx->single_segment || ctx->single_file) {
			GF_MPD_BaseURL *baseURL;

			//get rid of default "init" added for init templates
			gf_media_mpd_format_segment_name(GF_DASH_TEMPLATE_INITIALIZATION_SKIPINIT, set->bitstream_switching, szInitSegmentName, NULL, ds->rep_id, NULL, szDASHTemplate, (ctx->ext && !stricmp(ctx->ext, "null")) ? NULL : "mp4", 0, 0, 0, ctx->stl);

			if (ds->init_seg) gf_free(ds->init_seg);
			ds->init_seg = gf_strdup(szInitSegmentName);

			GF_SAFEALLOC(baseURL, GF_MPD_BaseURL);
			if (!rep->base_URLs) rep->base_URLs = gf_list_new();
			gf_list_add(rep->base_URLs, baseURL);

			if (ctx->single_segment) {
				GF_MPD_SegmentBase *segment_base;
				baseURL->URL = gf_strdup(szInitSegmentName);
				GF_SAFEALLOC(segment_base, GF_MPD_SegmentBase);
				rep->segment_base = segment_base;
				dasher_open_destination(filter, ctx, rep, szInitSegmentName, GF_FALSE);
			} else {
				GF_MPD_SegmentList *seg_list;
				GF_SAFEALLOC(seg_list, GF_MPD_SegmentList);
				GF_SAFEALLOC(seg_list->initialization_segment, GF_MPD_URL);
				baseURL->URL = gf_strdup(szInitSegmentName);
				seg_list->dasher_segment_name = gf_strdup(szSegmentName);
				seg_list->timescale = ds->mpd_timescale;
				seg_list->segment_URLs = gf_list_new();
				rep->segment_list = seg_list;
				ds->seg_urls = gf_list_new();

				dasher_open_destination(filter, ctx, rep, szInitSegmentName, GF_FALSE);
			}
		}
		//no template, no single file, we need a file list
		else {
			GF_MPD_SegmentList *seg_list;
			GF_SAFEALLOC(seg_list, GF_MPD_SegmentList);
			GF_SAFEALLOC(seg_list->initialization_segment, GF_MPD_URL);
			seg_list->initialization_segment->sourceURL = gf_strdup(szInitSegmentName);
			seg_list->dasher_segment_name = gf_strdup(szSegmentName);
			seg_list->timescale = ds->mpd_timescale;
			seg_list->segment_URLs = gf_list_new();
			rep->segment_list = seg_list;
			ds->seg_urls = gf_list_new();

			dasher_open_destination(filter, ctx, rep, szInitSegmentName, GF_FALSE);
		}
		//open PID
		dasher_open_pid(filter, ctx, ds, multi_pids);
	}
}

static void dasher_update_period_duration(GF_DasherCtx *ctx)
{
	u32 i, count;
	u64 pdur = 0;
	u64 min_dur = 0;
	count = gf_list_count(ctx->current_period->streams);
	for (i=0; i<count; i++) {
		GF_DashStream *ds = gf_list_get(ctx->current_period->streams, i);
		if (ds->xlink) pdur = 1000*ds->period_dur;
		else if (!min_dur || (min_dur>ds->max_period_dur)) min_dur = ds->max_period_dur;
		if (pdur< ds->max_period_dur) pdur = ds->max_period_dur;
	}

	if (!ctx->check_dur) {
		s32 diff = (s32) ((s64) pdur - (s64) min_dur);
		if (ABS(diff)>2000) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] Adaptation sets in period are of unequal duration min %g max %g seconds\n", ((Double)min_dur)/1000, ((Double)pdur)/1000));
		}
	}

	ctx->current_period->period->duration = pdur;
	if (!ctx->dynamic) {
		if (ctx->current_period->period->start) {
			ctx->mpd->media_presentation_duration = ctx->current_period->period->start + pdur;
		} else {
			ctx->mpd->media_presentation_duration += pdur;
		}
	}
}


GF_Err dasher_send_mpd(GF_Filter *filter, GF_DasherCtx *ctx)
{
	GF_FilterPacket *pck;
	u32 size, nb_read;
	char *output;
	GF_Err e;
	FILE *tmp;


	tmp = gf_temp_file_new(NULL);

	ctx->mpd->publishTime = gf_net_get_ntp_ms();
	dasher_update_mpd(ctx);
	ctx->mpd->write_context = GF_FALSE;
	e = gf_mpd_write(ctx->mpd, tmp);
	if (e) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_DASH, ("[Dasher] failed to write MPD file: %s\n", gf_error_to_string(e) ));
		gf_fclose(tmp);
		return e;
	}
	gf_fseek(tmp, 0, SEEK_END);
	size = (u32) gf_ftell(tmp);
	gf_fseek(tmp, 0, SEEK_SET);

	pck = gf_filter_pck_new_alloc(ctx->opid, size, &output);
	nb_read = gf_fread(output, 1, size, tmp);
	if (nb_read != size) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_DASH, ("[Dasher] Error reading temp MPD file, read %d bytes but file size is %d\n", nb_read, size ));
	}
	gf_filter_pck_set_framing(pck, GF_TRUE, GF_TRUE);
	gf_filter_pck_send(pck);
	gf_fclose(tmp);

	if (ctx->state) {
		tmp = gf_fopen(ctx->state, "w");
		if (!tmp) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_DASH, ("[Dasher] failed to open context MPD %s for write\n", ctx->state ));
			return GF_IO_ERR;
		}
		ctx->mpd->write_context = GF_TRUE;
		e = gf_mpd_write(ctx->mpd, tmp);
		gf_fclose(tmp);
		ctx->mpd->write_context = GF_FALSE;
		if (e) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_DASH, ("[Dasher] failed to write MPD file: %s\n", gf_error_to_string(e) ));
		}
		return e;
	}
	return GF_OK;
}

static void dasher_reset_stream(GF_DashStream *ds, Bool is_destroy)
{
	if (!ds->muxed_base && ds->dst_filter) {
		gf_filter_remove(ds->dst_filter, NULL);
	}
	ds->dst_filter = NULL;
	if (ds->seg_template) gf_free(ds->seg_template);
	if (ds->init_seg) gf_free(ds->init_seg);
	if (ds->multi_pids) gf_list_del(ds->multi_pids);
	ds->multi_pids = NULL;

	if (is_destroy) {
		gf_list_del(ds->complementary_reps);
		gf_free(ds->rep_id);
		return;
	}
	ds->init_seg = ds->seg_template = NULL;
	ds->split_set_names = GF_FALSE;
	ds->nb_sap_3 = 0;
	ds->nb_sap_4 = 0;
	ds->pid_id = 0;
	ds->force_timescale = 0;
	ds->set = NULL;
	ds->owns_set = GF_FALSE;
	ds->rep = NULL;
	ds->muxed_base = NULL;
	ds->nb_comp = ds->nb_comp_done = 0;
	gf_list_reset(ds->complementary_reps);
	ds->inband_params = GF_FALSE;
	ds->seg_start_time = 0;
	ds->seg_number = ds->startNumber;
}

void dasher_context_update_period_end(GF_DasherCtx *ctx)
{
	u32 i, count;

	if (!ctx->mpd) return;

	count = gf_list_count(ctx->current_period->streams);
	for (i=0; i<count; i++) {
		GF_DashStream *ds = gf_list_get(ctx->current_period->streams, i);
		if (!ds->rep->dasher_ctx) continue;
		if (ds->done == 1) {
			ds->rep->dasher_ctx->done = GF_TRUE;
		} else if (ds->done==2) {
			//store all dynamic parameters of the rep
			ds->rep->dasher_ctx->last_pck_idx = ds->nb_pck;
			ds->rep->dasher_ctx->seg_number = ds->seg_number;
			ds->rep->dasher_ctx->next_seg_start = ds->next_seg_start;
			ds->rep->dasher_ctx->first_cts = ds->first_cts;
		}
		assert(ds->rep->dasher_ctx->init_seg);
		assert(ds->rep->dasher_ctx->src_url);
		assert(ds->rep->dasher_ctx->template_seg);
	}
}

void dasher_context_update_period_start(GF_DasherCtx *ctx)
{
	u32 i, count;

	if (!ctx->mpd) return;
	count = gf_list_count(ctx->current_period->streams);
	for (i=0; i<count; i++) {
		GF_DashStream *ds = gf_list_get(ctx->current_period->streams, i);
		if (ds->rep->dasher_ctx) continue;

		//store all static parameters of the rep
		GF_SAFEALLOC(ds->rep->dasher_ctx, GF_DASH_SegmenterContext);
		ds->rep->dasher_ctx->done = GF_FALSE;

		assert(ds->init_seg);
		ds->rep->dasher_ctx->init_seg = gf_strdup(ds->init_seg);
		assert(ds->src_url);
		ds->rep->dasher_ctx->src_url = gf_strdup(ds->src_url);
		assert(ds->seg_template);
		ds->rep->dasher_ctx->template_seg = gf_strdup(ds->seg_template);
		ds->rep->dasher_ctx->pid_id = ds->pid_id;
		ds->rep->dasher_ctx->muxed_comp_id = ds->muxed_base ? ds->muxed_base->pid_id : 0;
		ds->rep->dasher_ctx->period_start = ds->period_start;
		ds->rep->dasher_ctx->period_duration = ds->period_dur;
		ds->rep->dasher_ctx->multi_pids = ds->multi_pids ? GF_TRUE : GF_FALSE;
		ds->rep->dasher_ctx->dash_dur = ds->dash_dur;

		if (strcmp(ds->period_id, "_gpac_dasher_default_period_id"))
			ds->rep->dasher_ctx->period_id = ds->period_id;

		ds->rep->dasher_ctx->owns_set = (ds->set->udta == ds) ? GF_TRUE : GF_FALSE;

	}

}

static GF_DashStream *dasher_get_stream(GF_DasherCtx *ctx, const char *src_url, u32 pid_id)
{
	GF_DashStream *ds;
	u32 i, count = gf_list_count(ctx->pids);
	for (i=0; i<count; i++) {
		ds = gf_list_get(ctx->pids, i);
		if (pid_id && (ds->pid_id==pid_id)) return ds;
		if (src_url && ds->src_url && !strcmp(ds->src_url, src_url)) return ds;
	}
	return NULL;
}

static GF_Err dasher_reload_context(GF_Filter *filter, GF_DasherCtx *ctx)
{
	GF_Err e;
	Bool last_period_active = GF_FALSE;

	u32 i, j, k, nb_p, nb_as, nb_rep;
	GF_DOMParser *mpd_parser;

	ctx->first_context_load = GF_FALSE;

	if (!gf_file_exists(ctx->state)) return GF_OK;

	/* parse the MPD */
	mpd_parser = gf_xml_dom_new();
	e = gf_xml_dom_parse(mpd_parser, ctx->state, NULL, NULL);

	if (e != GF_OK) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_DASH, ("[Dasher] Cannot parse MPD state %s: %s\n", ctx->state, gf_xml_dom_get_error(mpd_parser) ));
		gf_xml_dom_del(mpd_parser);
		return GF_URL_ERROR;
	}
	ctx->mpd = gf_mpd_new();
	e = gf_mpd_init_from_dom(gf_xml_dom_get_root(mpd_parser), ctx->mpd, ctx->state);
	gf_xml_dom_del(mpd_parser);

	if (!ctx->mpd->xml_namespace)
		ctx->mpd->xml_namespace = "urn:mpeg:dash:schema:mpd:2011";
	
	if (e != GF_OK) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_DASH, ("[Dasher] Cannot reload MPD state %s: %s\n", ctx->state, gf_error_to_string(e) ));
		gf_mpd_del(ctx->mpd);
		ctx->mpd = NULL;
		return GF_URL_ERROR;
	}
	//do a first pass to detect any potential changes in input config, if so consider the period over.
	nb_p = gf_list_count(ctx->mpd->periods);
	for (i=0; i<nb_p; i++) {
		Bool all_done_in_period = GF_TRUE;
		GF_MPD_Period *p = gf_list_get(ctx->mpd->periods, i);
		nb_as = gf_list_count(p->adaptation_sets);
		for (j=0; j<nb_as; j++) {
			GF_MPD_AdaptationSet *set = gf_list_get(p->adaptation_sets, j);
			nb_rep = gf_list_count(set->representations);
			for (k=0; k<nb_rep; k++) {
				GF_DashStream *ds;
				char *p_id;
				GF_MPD_Representation *rep = gf_list_get(set->representations, k);
				if (! rep->dasher_ctx) continue;

				if (rep->dasher_ctx->done) {
					all_done_in_period = GF_TRUE;
					continue;
				}
				//ensure we have the same settings - if not consider the dash stream has been resetup for a new period
				ds = dasher_get_stream(ctx, rep->dasher_ctx->src_url, 0);
				if (!ds) {
					rep->dasher_ctx->removed = GF_TRUE;
					continue;
				}
				p_id = "_gpac_dasher_default_period_id";
				if (rep->dasher_ctx->period_id) p_id = rep->dasher_ctx->period_id;

				if (ds->period_id && p_id && !strcmp(ds->period_id, p_id)) {
				} else if (!ds->period_id && !rep->dasher_ctx->period_id) {
				} else {
					rep->dasher_ctx->removed = GF_TRUE;
					continue;
				}
				if (ds->period_start != rep->dasher_ctx->period_start) {
					rep->dasher_ctx->removed = GF_TRUE;
					continue;
				}
				if (ds->period_dur != rep->dasher_ctx->period_duration) {
					rep->dasher_ctx->removed = GF_TRUE;
					continue;
				}
				all_done_in_period = GF_FALSE;
			}
		}
		if (!all_done_in_period) {
			assert(i+1==nb_p);
			last_period_active = GF_TRUE;
		}
	}

	if (!last_period_active) return GF_OK;
	ctx->current_period->period = gf_list_last(ctx->mpd->periods);
	gf_list_reset(ctx->current_period->streams);

	nb_as = gf_list_count(ctx->current_period->period->adaptation_sets);
	for (j=0; j<nb_as; j++) {
		GF_DashStream *set_ds = NULL;
		GF_List *multi_pids = NULL;
		Bool use_multi_pid_init = GF_FALSE;
		GF_MPD_AdaptationSet *set = gf_list_get(ctx->current_period->period->adaptation_sets, j);
		nb_rep = gf_list_count(set->representations);
		for (k=0; k<nb_rep; k++) {
			GF_DashStream *ds;
			GF_MPD_Representation *rep = gf_list_get(set->representations, k);
			if (! rep->dasher_ctx) continue;

			ds = dasher_get_stream(ctx, rep->dasher_ctx->src_url, 0);
			if (!ds) continue;

			//restore everything
			ds->done = rep->dasher_ctx->done;
			ds->seg_number = rep->dasher_ctx->seg_number;
			ds->init_seg = gf_strdup(rep->dasher_ctx->init_seg);
			ds->seg_template = gf_strdup(rep->dasher_ctx->template_seg);
			if (rep->dasher_ctx->period_id)
			ds->period_id = gf_strdup(rep->dasher_ctx->period_id);
			ds->period_start = rep->dasher_ctx->period_start;
			ds->period_dur = rep->dasher_ctx->period_duration;
			ds->pid_id = rep->dasher_ctx->pid_id;
			ds->seek_to_pck = rep->dasher_ctx->last_pck_idx;
			ds->dash_dur = rep->dasher_ctx->dash_dur;
			ds->next_seg_start = rep->dasher_ctx->next_seg_start;
			ds->adjusted_next_seg_start = ds->next_seg_start;
			ds->first_cts = rep->dasher_ctx->first_cts;
			ds->rep_init = GF_TRUE;

			ds->owns_set = rep->dasher_ctx->owns_set;
			if (ds->owns_set) set_ds = ds;

			if (rep->dasher_ctx->done) {
				ds->done = GF_TRUE;
				continue;
			}
			if (rep->dasher_ctx->muxed_comp_id) {
				GF_DashStream *base_ds = dasher_get_stream(ctx, NULL, rep->dasher_ctx->muxed_comp_id);
				ds->muxed_base = base_ds;
				base_ds->nb_comp ++;
			}
			ds->nb_comp = 1;

			if (ds->rep) gf_mpd_representation_free(ds->rep);
			ds->rep = rep;
			ds->set = set;
			rep->playback.udta = ds;
			if (ds->owns_set)
				set->udta = ds;
			if (rep->dasher_ctx->multi_pids)
				use_multi_pid_init = GF_TRUE;

			ds->period = ctx->current_period;

			//move all muxed components at the end
			if (ds->muxed_base)
				gf_list_add(ctx->current_period->streams, ds);
			else
				gf_list_insert(ctx->current_period->streams, ds, 0);
		}
		assert(set_ds);
		set_ds->nb_rep  =gf_list_count(set->representations);

		//if multi PID init, gather pids
		if (use_multi_pid_init) {
			multi_pids = gf_list_new();
			for (i=0; i<nb_rep; i++) {
				GF_MPD_Representation *rep = gf_list_get(set->representations, i);
				GF_DashStream *ds = rep->playback.udta;
				if (ds->owns_set) ds->multi_pids = multi_pids;
				gf_list_add(multi_pids, ds->ipid);
			}
		}
		for (i=0; i<nb_rep; i++) {
			GF_MPD_Representation *rep = gf_list_get(set->representations, i);
			GF_DashStream *ds = rep->playback.udta;
			//open destination, trashing init
			if (!ds->muxed_base)
				dasher_open_destination(filter, ctx, rep, ds->init_seg, GF_TRUE);

			dasher_open_pid(filter, ctx, ds, multi_pids);
		}
	}

	return GF_OK;
}

static GF_Err dasher_switch_period(GF_Filter *filter, GF_DasherCtx *ctx)
{
	u32 i, count, nb_done;
	Bool has_muxed_bases=GF_FALSE;
	char *period_id;
	const char *remote_xlink = NULL;
	Bool empty_period = GF_FALSE;
	Bool is_restore = GF_FALSE;
	GF_DasherPeriod *p;
	Double period_idx, period_start, next_period_start;
	GF_DashStream *first_in_period=NULL;
	p = ctx->current_period;

	if (!ctx->out_path) {
		ctx->out_path = gf_filter_pid_get_destination(ctx->opid);
	}
	if (ctx->current_period->period) {
		//update duration
		dasher_update_period_duration(ctx);

		if (ctx->state)
		 	dasher_context_update_period_end(ctx);
	}
	//we have a MPD ready, flush it
	if (ctx->mpd)
		dasher_send_mpd(filter, ctx);

	if (ctx->subdur_done)
		return GF_EOS;

	if (ctx->state)
		dasher_context_update_period_end(ctx);

	//reset - don't destroy, it is in the MPD
	ctx->current_period->period = NULL;
	//switch
	ctx->current_period = ctx->next_period;
	ctx->next_period = p;
	ctx->template_use_source = GF_FALSE;
	ctx->on_demand_done = GF_FALSE;
	//reset MPD pointers
	count = gf_list_count(ctx->current_period->streams);
	for (i=0; i<count;i++) {
		GF_DashStream *ds = gf_list_get(ctx->current_period->streams, i);
		dasher_reset_stream(ds, GF_FALSE);

		//remove output pids
		if (ds->opid) {
			gf_filter_pid_set_eos(ds->opid);
			gf_filter_pid_remove(ds->opid);
			ds->opid = NULL;
		}
	}

	//figure out next period
	count = gf_list_count(ctx->current_period->streams);
	period_idx = 0;
	period_start = -1;
	for (i=0; i<count; i++) {
		Double pstart;
		GF_DashStream *ds = gf_list_get(ctx->current_period->streams, i);

		if (ds->done) continue;
		if (ds->period_start < 0) {
			pstart = -ds->period_start;
			if (!period_idx || (pstart<period_idx)) period_idx = pstart;
		} else {
			if ((period_start<0) || (ds->period_start < period_start)) period_start = ds->period_start;
		}
	}

	if (period_start>=0)
		period_idx = 0;

	//filter out PIDs not for this period
	count = gf_list_count(ctx->current_period->streams);
	period_id = NULL;
	for (i=0; i<count; i++) {
		Bool in_period=GF_TRUE;
		GF_DashStream *ds = gf_list_get(ctx->current_period->streams, i);

		if (ds->done) {
			in_period=GF_FALSE;
		} else if (!period_id) {
			period_id = ds->period_id;
			first_in_period = ds;
		} else if (strcmp(period_id, ds->period_id)) {
			in_period = GF_FALSE;
		}
		if (in_period) {
			if ((period_start>=0) && (ds->period_start != period_start)) in_period = GF_FALSE;
			else if ((period_idx>0) && (-ds->period_start != period_idx)) in_period = GF_FALSE;
			if (!in_period && (first_in_period == ds))
				period_id = NULL;
		}

		//if not in period, move to next period
		if (!in_period) {
			gf_list_rem(ctx->current_period->streams, i);
			i--;
			count--;
			ds->period = NULL;
			gf_list_add(ctx->next_period->streams, ds);
			continue;
		}
		if (ds->stream_type == GF_STREAM_FILE) {
			if (ds->xlink) remote_xlink = ds->xlink;
			else empty_period = GF_TRUE;
		} else {
			//setup representation - the representation is created independetly from the period
			dasher_setup_rep(ctx, ds);
		}
	}
	count = gf_list_count(ctx->current_period->streams);
	if (!count) {
		count = gf_list_count(ctx->next_period->streams);
		nb_done = 0;
		for (i=0; i<count; i++)	 {
			GF_DashStream *ds = gf_list_get(ctx->next_period->streams, i);
			if (ds->done) nb_done++;
		}
		if (nb_done == count) {
			return GF_EOS;
		}
	}

	if (ctx->first_context_load) {
		GF_Err e = dasher_reload_context(filter, ctx);
		if (e) {
			ctx->setup_failure = e;
			return e;
		}
		if (ctx->current_period->period) is_restore = GF_TRUE;
	}

	//we need a new period unless created during reload, create it
	if (!is_restore) {
		ctx->current_period->period = gf_mpd_period_new();
		if (!ctx->mpd) dasher_setup_mpd(ctx);
		gf_list_add(ctx->mpd->periods, ctx->current_period->period);
	}


	if (remote_xlink) {
		ctx->current_period->period->xlink_href = gf_strdup(remote_xlink);
	}

	assert(period_id);

	next_period_start = -1;
	if (period_start>=0) {
		ctx->current_period->period->start = (u64)(period_start*1000);
		//check next period start
		count = gf_list_count(ctx->next_period->streams);
		for (i=0; i<count; i++)	 {
			GF_DashStream *ds = gf_list_get(ctx->next_period->streams, i);
			if (ds->done) continue;
			if (ds->period_start<period_start) continue;
			if ((next_period_start<0) || (next_period_start>ds->period_start))
				next_period_start = ds->period_start;
		}
		//check current period dur
		count = gf_list_count(ctx->current_period->streams);
		for (i=0; i<count; i++)	 {
			Double dur;
			GF_DashStream *ds = gf_list_get(ctx->current_period->streams, i);
			if (!ds->period_dur) continue;
			dur = period_start + ds->period_dur;
			if ((next_period_start<0) || (next_period_start>dur))
				next_period_start = dur;
		}
		if (next_period_start>0) {
			ctx->current_period->period->duration = (next_period_start - period_start) * 1000;
		}
	}


	//assign period ID if specified
	if (strcmp(period_id, "_gpac_dasher_default_period_id"))
		ctx->current_period->period->ID = gf_strdup(period_id);

	//setup representation dependency / components (muxed)
	count = gf_list_count(ctx->current_period->streams);
	for (i=0; i<count; i++) {
		u32 j;
		Bool remove = GF_FALSE;
		GF_DashStream *ds_video=NULL;
		GF_DashStream *ds = gf_list_get(ctx->current_period->streams, i);
		ds->period = ctx->current_period;

		if (ds->stream_type == GF_STREAM_FILE) {
			remove = GF_TRUE;
		} else if (remote_xlink) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] period uses xlink but other media source %s, ignoring source\n", ds->src_url));
			remove = GF_TRUE;
		} else if (empty_period) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] empty period defines but other media source %s, ignoring source\n", ds->src_url));
			remove = GF_TRUE;
		}

		if (remove) {
			ds->done = 1;
			ds->period = NULL;
			gf_list_rem(ctx->current_period->streams, i);
			gf_list_add(ctx->next_period->streams, ds);
			i--;
			count--;
			continue;
		}

		if (next_period_start>0) {
			ds->force_rep_end = (u64) ((next_period_start - period_start) * ds->timescale);
		}

		if (is_restore) continue;

		//add period descriptors
		dasher_add_descriptors(&ctx->current_period->period->other_descriptors, ds->p_period_desc);
		//add representation descriptors
		dasher_add_descriptors(&ds->rep->other_descriptors, ds->p_rep_desc);

		if (ds->muxed_base) continue;

		if (ds->stream_type==GF_STREAM_VISUAL)
			ds_video = ds;
		ds->nb_comp = 1;

		for (j=0; j<count; j++) {
			GF_DashStream *a_ds;
			if (i==j) continue;
			a_ds = gf_list_get(ctx->current_period->streams, j);
			if (a_ds->dep_id && (a_ds->dep_id==ds->id) ) {
				gf_list_add(ds->complementary_reps, a_ds);
			}
			if (!a_ds->muxed_base && !strcmp(a_ds->rep_id, ds->rep_id) ) {
				char szCodecs[1024];
				a_ds->muxed_base = ds;
				a_ds->dash_dur = ds->dash_dur;
				has_muxed_bases = GF_TRUE;
				ds->nb_comp++;

				if (ctx->bs_switch==DASHER_BS_SWITCH_MULTI) {
					GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] Bitstream Swicthing mode \"multi\" is not supported with multiplexed representations, disabling bitstream switching\n"));
					ctx->bs_switch = DASHER_BS_SWITCH_OFF;
				}
				strcpy(szCodecs, ds->rep->codecs);
				strcat(szCodecs, ",");
				strcat(szCodecs, a_ds->rep->codecs);
				gf_free(ds->rep->codecs);
				ds->rep->codecs = gf_strdup(szCodecs);
			}
		}
		//use video as main stream for segmentation of muxed sources
		if (ds_video != ds) {
			for (j=0; j<count; j++) {
				GF_DashStream *a_ds = gf_list_get(ctx->current_period->streams, j);
				if ((a_ds->muxed_base==ds) || (a_ds==ds)) {
					if (a_ds == ds_video) a_ds->muxed_base = NULL;
					else a_ds->muxed_base = ds_video;
				}
			}
		}
	}

	if (is_restore) return GF_OK;

	//moved all mux components after the base one, so that we do the segmentation on the main component
	if (has_muxed_bases) {
		//setup reps in adaptation sets
		for (i=0; i<count; i++) {
			GF_DashStream *ds = gf_list_get(ctx->current_period->streams, i);
			if (!ds->muxed_base) continue;
			gf_list_rem(ctx->current_period->streams, i);
			gf_list_add(ctx->current_period->streams, ds);
		}
	}

	//setup reps in adaptation sets
	for (i=0; i<count; i++) {
		u32 j;
		GF_DashStream *ds = gf_list_get(ctx->current_period->streams, i);
		if (ds->muxed_base) continue;

		if (!ds->set) {
			ds->set = gf_mpd_adaptation_set_new();
			ds->owns_set = GF_TRUE;
			ds->set->udta = ds;

			if (!ds->set->representations)
			 	ds->set->representations = gf_list_new();
			if (!ds->period->period->adaptation_sets)
				ds->period->period->adaptation_sets = gf_list_new();
			gf_list_add(ds->period->period->adaptation_sets, ds->set);

			gf_list_add(ds->set->representations, ds->rep);
			ds->nb_rep++;

			//add non-conditional adaptation set descriptors
			dasher_add_descriptors(&ds->set->other_descriptors, ds->p_as_any_desc);
			//new AS, add conditionnal adaptation set descriptors
			dasher_add_descriptors(&ds->set->other_descriptors, ds->p_as_desc);
		}
		for (j=i+1; j<count; j++) {
			GF_DashStream *a_ds;
			a_ds = gf_list_get(ctx->current_period->streams, j);
			//we add to the adaptation set even if shared rep, we will remove it when assigning templates and pids
			if (dasher_same_adaptation_set(ctx, ds, a_ds)) {
				a_ds->set = ds->set;
				gf_list_add(ds->set->representations, a_ds->rep);
				ds->nb_rep++;
				//add non-conditional adaptation set descriptors
				dasher_add_descriptors(&ds->set->other_descriptors, a_ds->p_as_any_desc);
			}
		}
	}
	//we need a pass on adaptation sets to figure out if they share the same source URL
	//in case we use file name in templates
	if (ctx->template_use_source) {
		u32 i, j, nb_sets = gf_list_count(ctx->current_period->period->adaptation_sets);

		for (i=0; i<nb_sets; i++) {
			GF_MPD_AdaptationSet *set = gf_list_get(ctx->current_period->period->adaptation_sets, i);
			GF_MPD_Representation *rep = gf_list_get(set->representations, 0);
			GF_DashStream *ds = rep->playback.udta;
			for (j=0; j<nb_sets; j++) {
				Bool split_init = GF_FALSE;
				const GF_PropertyValue *p1, *p2;
				GF_DashStream *a_ds;
				if (i==j) continue;
				set = gf_list_get(ctx->current_period->period->adaptation_sets, j);
				rep = gf_list_get(set->representations, 0);
				a_ds = rep->playback.udta;
				p1 = gf_filter_pid_get_property(ds->ipid, GF_PROP_PID_FILEPATH);
				p2 = gf_filter_pid_get_property(a_ds->ipid, GF_PROP_PID_FILEPATH);
				if (gf_props_equal(p1, p2)) split_init = GF_TRUE;
				p1 = gf_filter_pid_get_property(ds->ipid, GF_PROP_PID_URL);
				p2 = gf_filter_pid_get_property(a_ds->ipid, GF_PROP_PID_URL);
				if (gf_props_equal(p1, p2)) split_init = GF_TRUE;

				if (split_init) {
					ds->split_set_names = GF_TRUE;
					a_ds->split_set_names = GF_TRUE;
				}
			}
		}
	}
	//setup adaptation sets bitstream switching
	for (i=0; i<count; i++) {
		GF_DashStream *ds = gf_list_get(ctx->current_period->streams, i);
		if (!ds->owns_set) continue;
		//check bitstream switching
		dasher_check_bitstream_swicthing(ctx, ds->set);
		//setup AS defaults, roles and co
		dasher_setup_set_defaults(ctx, ds->set);
		//setup sources, templates & co
		dasher_setup_sources(filter, ctx, ds->set);
	}

	//good to go !
	for (i=0; i<count; i++) {
		GF_DashStream *ds = gf_list_get(ctx->current_period->streams, i);
		//setup segmentation
		ds->rep_init = GF_FALSE;
		ds->seg_done = GF_FALSE;
		ds->next_seg_start = ds->dash_dur * ds->timescale;
		ds->adjusted_next_seg_start = ds->next_seg_start;
		ds->segment_started = GF_FALSE;
		ds->seg_number = ds->startNumber;
		ds->first_cts = ds->max_period_dur = 0;
	}

	//init UTC reference time for dynamic
	if (!ctx->generation_start_utc && ctx->dynamic) {
		u32 sec, frac;
		u64 dash_start_date = ctx->ast ? gf_net_parse_date(ctx->ast) : 0;

		gf_net_get_ntp(&sec, &frac);

		if (dash_start_date) {
			u64 start_date_sec_ntp, start_date_sec_ntp_ms_frac;
			Double ms;
			u64 secs = dash_start_date/1000;
			start_date_sec_ntp = (u32) secs;
			start_date_sec_ntp += GF_NTP_SEC_1900_TO_1970;

			ms = (Double) (dash_start_date - secs*1000);
			ms /= 1000.0;
			ms *= 0xFFFFFFFF;
			start_date_sec_ntp_ms_frac = (u32) ms;

			ctx->nb_secs_to_discard = sec;
			ctx->nb_secs_to_discard -= start_date_sec_ntp;
			if (ctx->tsb>=0)
				ctx->nb_secs_to_discard -= ctx->tsb;

			sec = start_date_sec_ntp;
			frac = start_date_sec_ntp_ms_frac;
		}
		ctx->generation_start_utc = sec - GF_NTP_SEC_1900_TO_1970;
		ctx->generation_start_utc *= 1000;
		ctx->generation_start_utc += ((u64) frac) * 1000 / 0xFFFFFFFFUL;
	}
	if (ctx->state)
		dasher_context_update_period_start(ctx);
	return GF_OK;
}

static void dasher_insert_timeline_entry(GF_DasherCtx *ctx, GF_DashStream *ds)
{
	GF_MPD_SegmentTimelineEntry *s;
	u64 duration;
	Bool is_first = GF_FALSE;
	Bool seg_align = GF_FALSE;
	GF_MPD_SegmentTimeline *tl=NULL;

	//we only store segment timeline for the main component in the representation
	if (ds->muxed_base) return;

	//we only use segment timeline with templates
	if (!ctx->stl) return;

	if (gf_list_find(ds->set->representations, ds->rep)==0) is_first = GF_TRUE;
	assert(ds->first_cts_in_next_seg > ds->first_cts_in_seg);
	duration = ds->first_cts_in_next_seg - ds->first_cts_in_seg;
	if (ds->timescale != ds->mpd_timescale) {
		duration *= ds->mpd_timescale;
		duration /= ds->timescale;
	}
	seg_align = (ds->set->segment_alignment || ds->set->subsegment_alignment) ? GF_TRUE : GF_FALSE;
	//not first and segment alignment, ignore
	if (!is_first && seg_align) {
		return;
	}

	//no segment alignment store in each rep
	if (!seg_align) {
		GF_MPD_SegmentTimeline **p_tl=NULL;
		if (ctx->tpl) {
			p_tl = &ds->rep->segment_template->segment_timeline;
			ds->rep->segment_template->duration = 0;
		} else {
			p_tl = &ds->rep->segment_list->segment_timeline;
			ds->rep->segment_list->duration = 0;
		}
		if (! (*p_tl)) {
			(*p_tl) = gf_mpd_segmentimeline_new();
		}
		tl = (*p_tl);
	} else {
		GF_MPD_SegmentTimeline **p_tl=NULL;
		if (ctx->tpl) {
			//in case we had no template at set level
			if (!ds->set->segment_template) {
				GF_SAFEALLOC(ds->set->segment_template, GF_MPD_SegmentTemplate);
			}
			p_tl = &ds->set->segment_template->segment_timeline;
			ds->set->segment_template->duration = 0;
		} else {
			//in case we had no template at set level
			if (!ds->set->segment_list) {
				GF_SAFEALLOC(ds->set->segment_list, GF_MPD_SegmentList);
			}
			p_tl = &ds->set->segment_list->segment_timeline;
			ds->set->segment_list->duration = 0;
		}

		if (! (*p_tl) ) {
			(*p_tl)  = gf_mpd_segmentimeline_new();
		}
		tl = (*p_tl);
	}

	//append to previous entry if possible
	s = gf_list_last(tl->entries);
	if (s && (s->duration == duration) && (s->start_time + (s->repeat_count+1) * s->duration == ds->seg_start_time)) {
		s->repeat_count++;
		return;
	}
	//nope, allocate
	GF_SAFEALLOC(s, GF_MPD_SegmentTimelineEntry);
	s->start_time = ds->seg_start_time;
	s->duration = duration;
	gf_list_add(tl->entries, s);
}

static void dasher_copy_segment_timelines(GF_DasherCtx *ctx, GF_MPD_AdaptationSet *set)
{
	GF_MPD_SegmentTimeline *src_tl = NULL;
	u32 i, j, count, nb_s;
	if (!ctx->stl) return;
	//get as level segment timeline, set it to NULL, reassign it to first rep and clone for other reps
	if (ctx->tpl) {
		assert(set->segment_template->segment_timeline);
		src_tl = set->segment_template->segment_timeline;
		set->segment_template->segment_timeline = NULL;
	} else {
		assert(set->segment_list->segment_timeline);
		src_tl = set->segment_list->segment_timeline;
		set->segment_list->segment_timeline = NULL;
	}
	nb_s = gf_list_count(src_tl->entries);

	count = gf_list_count(set->representations);
	for (i=0; i<count; i++) {
		GF_MPD_SegmentTimeline *tl = NULL;
		GF_MPD_Representation *rep = gf_list_get(set->representations, i);
		if (ctx->tpl) {
			if (!rep->segment_template) {
				GF_SAFEALLOC(rep->segment_template, GF_MPD_SegmentTemplate);
			}
			if (!i) {
				rep->segment_template->segment_timeline = src_tl;
				continue;
			}
			if (!rep->segment_template->segment_timeline) {
				rep->segment_template->segment_timeline = gf_mpd_segmentimeline_new();
			}
			tl = rep->segment_template->segment_timeline;
		} else {
			if (!rep->segment_list) {
				GF_SAFEALLOC(rep->segment_list, GF_MPD_SegmentList);
			}
			if (!i) {
				rep->segment_list->segment_timeline = src_tl;
				continue;
			}
			if (!rep->segment_list->segment_timeline) {
				rep->segment_list->segment_timeline = gf_mpd_segmentimeline_new();
			}
			tl = rep->segment_list->segment_timeline;
		}
		assert(tl);
		for (j=0; j<nb_s; j++) {
			GF_MPD_SegmentTimelineEntry *s;
			GF_MPD_SegmentTimelineEntry *src_s = gf_list_get(src_tl->entries, j);
			GF_SAFEALLOC(s, GF_MPD_SegmentTimelineEntry);
			s->duration = src_s->duration;
			s->repeat_count = src_s->repeat_count;
			s->start_time = src_s->start_time;
			gf_list_add(tl->entries, s);
		}
	}
}

static void dasher_flush_segment(GF_DasherCtx *ctx, GF_DashStream *ds)
{
	u32 i, count;
	GF_DashStream *ds_done = NULL, *ds_not_done = NULL;
	GF_DashStream *set_ds = ds->set->udta;
	GF_DashStream *base_ds = ds->muxed_base ? ds->muxed_base : ds;


	if (ds->segment_started) {
		Double seg_duration = base_ds->first_cts_in_next_seg - ds->first_cts_in_seg;
		seg_duration /= base_ds->timescale;
		assert(seg_duration);

		if (!base_ds->done && !ctx->stl && ctx->tpl) {

			if (seg_duration< ds->dash_dur/2) {

				GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] Segment %d duration %g less than half DASH duration, consider reencoding or using segment timeline\n", ds->seg_number, seg_duration));
			} else if (seg_duration > 3 * ds->dash_dur / 2) {
				GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] Segment %d duration %g more than 3/2 DASH duration, consider reencoding or using segment timeline\n", ds->seg_number, seg_duration));
			}
		}
		dasher_insert_timeline_entry(ctx, base_ds);

		if (ctx->align) {
			if (!set_ds->nb_rep_done || !set_ds->set_seg_duration) {
				set_ds->set_seg_duration = seg_duration;
			} else {
				Double diff = set_ds->set_seg_duration - seg_duration;

				if (ABS(diff) > 0.001) {
					GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] Segments are not aligned across representations: first rep segment duration %g but new segment duration %g for the same segment %d\n", set_ds->set_seg_duration, seg_duration, set_ds->seg_number));

					if (ctx->profile != GF_DASH_PROFILE_FULL) {
						set_ds->set->segment_alignment = GF_FALSE;
						set_ds->set->subsegment_alignment = GF_FALSE;
						ctx->profile = GF_DASH_PROFILE_FULL;
						GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] No segment alignment, switching to full profile\n"));
						dasher_copy_segment_timelines(ctx, set_ds->set);
					}
				}
			}
			set_ds->nb_rep_done++;
			if (set_ds->nb_rep_done < set_ds->nb_rep) return;

			set_ds->set_seg_duration = 0;
			set_ds->nb_rep_done = 0;
		}
	} else {
		if (ctx->align) {
			set_ds->nb_rep_done++;
			if (set_ds->nb_rep_done < set_ds->nb_rep) return;

			set_ds->set_seg_duration = 0;
			set_ds->nb_rep_done = 0;
		}
	}


	count = gf_list_count(ctx->current_period->streams);

	if (ctx->subdur) {
		if (ctx->subdur_done) return;
		for (i=0; i<count; i++) {
			ds = gf_list_get(ctx->current_period->streams, i);
			if (ds->muxed_base) continue;
			if (ds->cumulated_dur >= ctx->subdur * ds->timescale) {
				ctx->subdur_done = GF_TRUE;
			}
		}
	}

	//reset all streams from our rep or our set
	for (i=0; i<count; i++) {
		ds = gf_list_get(ctx->current_period->streams, i);
		//reset all in set if segment alignment
		if (ctx->align) {
			if (ds->set != set_ds->set) continue;
		} else {
			//otherwise reset only media components for this rep
			if ((ds->muxed_base != base_ds) && (ds != base_ds)) continue;
		}

		if (!ds->done) {
			ds->first_cts_in_next_seg = ds->first_cts_in_seg = ds->est_first_cts_in_next_seg = 0;
		}

		if (ds->muxed_base) {
			if (!ds->done) {
				ds->segment_started = GF_FALSE;
				ds->seg_done = GF_FALSE;
			}
			continue;
		}
		base_ds = ds;

		if (base_ds->done)
			ds_done = base_ds;
		else if (base_ds->nb_comp_done==base_ds->nb_comp) ds_not_done = base_ds;

		if (!base_ds->done && base_ds->seg_done) {
			base_ds->seg_done = GF_FALSE;
			base_ds->nb_comp_done = 0;

			assert(base_ds->segment_started);
			base_ds->segment_started = GF_FALSE;

			base_ds->next_seg_start += base_ds->dash_dur * base_ds->timescale;
			while (base_ds->next_seg_start <= base_ds->adjusted_next_seg_start) {
				base_ds->next_seg_start += base_ds->dash_dur * base_ds->timescale;
				if (ctx->skip_seg)
					base_ds->seg_number++;
			}
			base_ds->adjusted_next_seg_start = base_ds->next_seg_start;
			base_ds->seg_number++;
		}
	}

	//some reps are done, other not, force a max time on all AS in the period
	if (ds_done && ds_not_done) {
		for (i=0; i<count; i++) {
			ds = gf_list_get(ctx->current_period->streams, i);

			if (ds->done) {
				if (ds->set->udta == set_ds)
					set_ds->nb_rep_done++;
			} else if (ctx->check_dur && !ds->force_rep_end) {
				ds->force_rep_end = ds_done->first_cts_in_next_seg * ds->timescale / ds_done->timescale;
			}
		}
	}
}

static void dasher_mark_segment_start(GF_DasherCtx *ctx, GF_DashStream *ds, GF_FilterPacket *pck)
{
	char szSegmentName[GF_MAX_PATH];
	GF_DashStream *base_ds = ds->muxed_base ? ds->muxed_base : ds;

	if (ctx->ntp==DASHER_NTP_YES) {
		u64 ntpts = gf_net_get_ntp_ts();
		gf_filter_pck_set_property(pck, GF_PROP_PCK_SENDER_NTP, &PROP_LONGUINT(ntpts));
	} else if (ctx->ntp==DASHER_NTP_REM) {
		gf_filter_pck_set_property(pck, GF_PROP_PCK_SENDER_NTP, NULL);
	}

	gf_filter_pck_set_property(pck, GF_PROP_PCK_FILENUM, &PROP_UINT(base_ds->seg_number ) );

	//only signal file name & insert timelines on one stream for muxed representations
	if (ds->muxed_base) return;

	if (ctx->single_file) {
		if (ds->rep->segment_list) {
			GF_MPD_SegmentURL *seg_url;
			GF_SAFEALLOC(seg_url, GF_MPD_SegmentURL);
			gf_list_add(ds->rep->segment_list->segment_URLs, seg_url);
			gf_list_add(ds->seg_urls, seg_url);
			ctx->nb_seg_url_pending++;
		}
		return;
	}

	ds->seg_start_time = ds->first_cts_in_seg;
	if (ds->timescale != ds->mpd_timescale) {
		ds->seg_start_time *= ds->mpd_timescale;
		ds->seg_start_time /= ds->timescale;
	}

	if (!ctx->stl) {
		Double drift, seg_start = ds->seg_start_time;
		seg_start /= ds->mpd_timescale;
		drift = seg_start - (ds->seg_number - ds->startNumber) * ds->dash_dur;

		if (ABS(drift) > ds->dash_dur/2) {
			u64 cts = gf_filter_pck_get_cts(pck);
			cts -= ds->first_cts;
			GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] First CTS "LLU" in segment %d drifting by %g (more than half a second duration) from segment time, consider reencoding or using segment timeline\n", cts, ds->seg_number,  drift));
		}
	}

	//get final segment template - output file name is NULL, we already have solved this
	gf_media_mpd_format_segment_name(GF_DASH_TEMPLATE_SEGMENT, ds->set->bitstream_switching, szSegmentName, NULL, base_ds->rep_id, NULL, base_ds->seg_template, NULL, base_ds->seg_start_time, base_ds->rep->bandwidth, base_ds->seg_number, ctx->stl);


	if (ctx->out_path) {
		char *rel = gf_url_concatenate(ctx->out_path, szSegmentName);
		if (rel) {
			strcpy(szSegmentName, rel);
			gf_free(rel);
		}
	}


	if (ds->rep->segment_list) {
		GF_MPD_SegmentURL *seg_url;
		GF_SAFEALLOC(seg_url, GF_MPD_SegmentURL);
		gf_list_add(ds->rep->segment_list->segment_URLs, seg_url);
		seg_url->media = gf_strdup(szSegmentName);
		gf_list_add(ds->seg_urls, seg_url);
		ctx->nb_seg_url_pending++;
	}

	gf_filter_pck_set_property(pck, GF_PROP_PCK_FILENAME, &PROP_STRING(szSegmentName) );
}

static void dasher_update_pck_times(GF_DashStream *ds, GF_FilterPacket *dst)
{
	u64 ts;
	ts = gf_filter_pck_get_dts(dst);
	if (ts!=GF_FILTER_NO_TS) {
		ts *= ds->force_timescale;
		ts /= ds->timescale;
		gf_filter_pck_set_dts(dst, ts);
	}
	ts = gf_filter_pck_get_cts(dst);
	if (ts!=GF_FILTER_NO_TS) {
		ts *= ds->force_timescale;
		ts /= ds->timescale;
		gf_filter_pck_set_cts(dst, ts);
	}
	ts = (u64) gf_filter_pck_get_duration(dst);
	if (ts!=GF_FILTER_NO_TS) {
		ts *= ds->force_timescale;
		ts /= ds->timescale;
		gf_filter_pck_set_duration(dst, (u32) ts);
	}
}

static GF_Err dasher_process(GF_Filter *filter)
{
	u32 i, count, nb_init, has_init;
	GF_DasherCtx *ctx = gf_filter_get_udta(filter);
	GF_Err e;

	if (ctx->is_eos) return GF_EOS;
	if (ctx->setup_failure) return ctx->setup_failure;

	nb_init = has_init = 0;
	count = gf_list_count(ctx->current_period->streams);
	for (i=0; i<count; i++) {
		GF_DashStream *base_ds;
		GF_DashStream *ds = gf_list_get(ctx->current_period->streams, i);

		if (ds->done) continue;
		base_ds = ds->muxed_base ? ds->muxed_base : ds;
		//subdur mode abort
		if (ctx->subdur_done) {
			if (!ds->done) {
				ds->done = 2;
				gf_filter_pid_set_eos(ds->opid);
				gf_filter_pid_set_discard(ds->ipid, GF_TRUE);
			}
			continue;
		}
		if (ds->seg_done) continue;

		//flush as mush as possible
		while (1) {
			u32 sap_type, dur, split_dur;
			u64 cts, ncts, split_dur_next;
			Bool seg_over = GF_FALSE;
			Bool is_split = GF_FALSE;
			GF_FilterPacket *pck;
			GF_FilterPacket *dst;

			assert(ds->period == ctx->current_period);
			pck = gf_filter_pid_get_packet(ds->ipid);
			//we may change period after a packet fecth (reconfigure of input pid)
			if (ds->period != ctx->current_period) {
				assert(gf_list_find(ctx->current_period->streams, ds)<0);
				count = gf_list_count(ctx->current_period->streams);
				i--;
				break;
			}

			if (!pck) {
				if (gf_filter_pid_is_eos(ds->ipid)) {
					gf_filter_pid_set_eos(ds->opid);
					ds->done = 1;
					ds->seg_done = GF_TRUE;
					ds->first_cts_in_next_seg = ds->est_first_cts_in_next_seg;
					ds->est_first_cts_in_next_seg = 0;
					base_ds->nb_comp_done ++;
					if (base_ds->nb_comp_done == base_ds->nb_comp) {
						dasher_flush_segment(ctx, base_ds);
					}
				}
				break;
			}
			if (ds->seek_to_pck && ds->nb_pck < ds->seek_to_pck) {
				gf_filter_pid_drop_packet(ds->ipid);
				ds->nb_pck++;
				continue;
			}
			sap_type = gf_filter_pck_get_sap(pck);

			cts = gf_filter_pck_get_cts(pck);
			if (!ds->rep_init) {
				if (!sap_type) {
					gf_filter_pid_drop_packet(ds->ipid);
					break;
				}
				if (!ds->muxed_base) {
					//set AS sap type
					if (!ds->set->starts_with_sap) {
						//don't set SAP type if not a base rep - could be further checked
						if (!gf_list_count(ds->complementary_reps) )
							ds->set->starts_with_sap = sap_type;
					}
					else if (ds->set->starts_with_sap != sap_type) {
						GF_LOG(GF_LOG_ERROR, GF_LOG_DASH, ("[Dasher] Segments do not start with the same SAP types: set initialized with %d but first packet got %d - bitstream will not be compliant\n", ds->set->starts_with_sap, sap_type));
					}
					if (ds->rep->segment_list)
						ds->rep->segment_list->presentation_time_offset = cts;
					else if (ds->rep->segment_template)
						ds->rep->segment_template->presentation_time_offset = cts;
				}

				ds->first_cts = cts;
				ds->rep_init++;
				has_init++;
			}
			nb_init++;
			//ready to write MPD for the first time in dynamic mode
			if (has_init && (nb_init==count) && ctx->dynamic) {
				e = dasher_send_mpd(filter, ctx);
				if (e) return e;
			}
			cts -= ds->first_cts;

			dur = gf_filter_pck_get_duration(pck);
			split_dur = 0;
			split_dur_next = 0;

			//adjust duration and cts
			if (ds->split_dur_next) {
				cts += ds->split_dur_next;
				assert(dur > ds->split_dur_next);
				dur -= ds->split_dur_next;
				split_dur_next = ds->split_dur_next;
				ds->split_dur_next = 0;
				is_split = GF_TRUE;
			}

			if (ds->splitable && !ds->split_dur_next) {
				//adding this sampl would exceed the segment duration
				if ( (cts + dur) * base_ds->timescale >= base_ds->adjusted_next_seg_start * ds->timescale ) {
					//this sample starts in the current segment - split it
					if (cts * base_ds->timescale < base_ds->adjusted_next_seg_start * ds->timescale ) {
						split_dur = (u32) (base_ds->adjusted_next_seg_start * ds->timescale / base_ds->timescale - ds->last_cts);
					}
				}
			}

			//mux rep, wait for a CTS more than our base if base not yet over
			if ((base_ds != ds) && !base_ds->seg_done && (cts * base_ds->timescale > base_ds->last_cts * ds->timescale ) )
				break;

			//forcing max time
			if (base_ds->force_rep_end && (cts * base_ds->timescale >= base_ds->force_rep_end * ds->timescale) ) {
				seg_over = GF_TRUE;
				if (!base_ds->period->period->duration) {
					GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] Inputs duration do not match, %s truncated to %g duration\n", ds->src_url, ((Double)base_ds->force_rep_end)/base_ds->timescale ));
				}
				ds->done = 1;
				gf_filter_pid_set_eos(ds->opid);
				gf_filter_pid_set_discard(ds->ipid, GF_TRUE);
			} else if (cts * base_ds->timescale >= base_ds->adjusted_next_seg_start * ds->timescale ) {
				//no sap, segment is over
				if (! ctx->sap) {
					seg_over = GF_TRUE;
				}
				// sap, segment is over
				else if (sap_type) {

					if (sap_type==3)
						ds->nb_sap_3 ++;
					else if (sap_type>3)
						ds->nb_sap_4 ++;

					/*check requested profiles can be generated, or adjust them*/
					if ((ctx->profile != GF_DASH_PROFILE_FULL) && (ds->nb_sap_4 || (ds->nb_sap_3 > 1)) ) {
						GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] WARNING! Max SAP type %d detected - switching to FULL profile\n", ds->nb_sap_4 ? 4 : 3));
						ctx->profile = GF_DASH_PROFILE_FULL;
						ds->set->starts_with_sap = sap_type;
					}

					seg_over = GF_TRUE;
					if (ds == base_ds) {
						base_ds->adjusted_next_seg_start = cts;
					}
				}
			}
			//if dur=0 (some text streams), don't flush segment
			if (seg_over && dur) {
				assert(!ds->seg_done);
				ds->seg_done = GF_TRUE;
				ds->first_cts_in_next_seg = cts;
				base_ds->nb_comp_done ++;
				if (split_dur_next)
					ds->split_dur_next = split_dur_next;

				if (base_ds->nb_comp_done == base_ds->nb_comp) {
					dasher_flush_segment(ctx, base_ds);
				}
				break;
			}

			ncts = cts + dur;
			if (ncts>ds->est_first_cts_in_next_seg)
				ds->est_first_cts_in_next_seg = ncts;

			ncts *= 1000;
			ncts /= ds->timescale;
			if (ncts>base_ds->max_period_dur)
				base_ds->max_period_dur = ncts;

			ds->last_cts = cts;
			ds->nb_pck ++;
			
			//create new ref to input
			dst = gf_filter_pck_new_ref(ds->opid, NULL, 0, pck);
			//merge all props
			gf_filter_pck_merge_properties(pck, dst);
			if (!ds->segment_started) {
				ds->first_cts_in_seg = cts;
				dasher_mark_segment_start(ctx, ds, dst);
				ds->segment_started = GF_TRUE;
			}
			//if split, adjust duration
			if (split_dur) {
				gf_filter_pck_set_duration(dst, split_dur);
				assert( dur > split_dur);
				ds->split_dur_next = split_dur;
				dur = split_dur;
			}
			//prev packet was split
			else if (is_split) {
				u64 diff;
				u64 ts = gf_filter_pck_get_cts(pck);
				assert (ts != GF_FILTER_NO_TS);
				cts += ds->first_cts;
				assert(cts >= ts);
				diff = cts - ts;

				gf_filter_pck_set_cts(dst, cts);

				ts = gf_filter_pck_get_dts(pck);
				if (ts != GF_FILTER_NO_TS)
					gf_filter_pck_set_dts(dst, ts + diff);


				gf_filter_pck_set_sap(dst, GF_FILTER_SAP_REDUNDANT);
				gf_filter_pck_set_duration(dst, dur);
			}

			//remove NTP
			if (ctx->ntp != DASHER_NTP_KEEP)
				gf_filter_pck_set_property(dst, GF_PROP_PCK_SENDER_NTP, NULL);

			//change packet times
			if (ds->force_timescale) {
				dasher_update_pck_times(ds, dst);
			}

			ds->cumulated_dur += dur;

			//send packet
			gf_filter_pck_send(dst);

			//drop packet if not spliting
			if (!ds->split_dur_next)
				gf_filter_pid_drop_packet(ds->ipid);
		}
	}
	nb_init=0;
	for (i=0; i<count; i++) {
		GF_DashStream *ds = gf_list_get(ctx->current_period->streams, i);
		if (ds->done) nb_init++;
	}
	//still some running steams in period
	if (count && (nb_init<count)) return GF_OK;

	//we need to wait for full flush of packets before switching periods in order to get the
	//proper segment size for segment_list+byte_range mode
	if (ctx->nb_seg_url_pending)
		return GF_OK;
	if (ctx->single_segment && !ctx->on_demand_done) return GF_OK;

	//done with this period, do period switch - this will update the MPD if needed
	e = dasher_switch_period(filter, ctx);
	//no more periods
	if (e==GF_EOS) {
		if (!ctx->is_eos) {
			ctx->is_eos = GF_TRUE;
			gf_filter_pid_set_eos(ctx->opid);
		}
	}
	return e;
}

static Bool dasher_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	u32 i, count;
	Bool flush_mpd = GF_FALSE;
	GF_DasherCtx *ctx = gf_filter_get_udta(filter);
	if (evt->base.type != GF_FEVT_SEGMENT_SIZE) return GF_FALSE;

	count = gf_list_count(ctx->pids);
	for (i=0; i<count; i++) {
		u64 r_start, r_end;
		GF_DashStream *ds = gf_list_get(ctx->pids, i);
		if (ds->opid != evt->base.on_pid) continue;

		if (ds->muxed_base) continue;

		//don't set segment sizes in template mode
		if (ctx->tpl) continue;
		//only set  size/index size for init segment when doing onDemand/single index
		if (ctx->single_segment && !evt->seg_size.is_init) continue;

		if (evt->seg_size.media_range_end) {
			r_start = evt->seg_size.media_range_start;
			r_end = evt->seg_size.media_range_end;
		} else {
			r_start = evt->seg_size.idx_range_start;
			r_end = evt->seg_size.idx_range_end;
		}
		//init segment or representation index, set it in on demand and main single source
		if (ctx->single_file && (evt->seg_size.is_init==1))  {
			GF_MPD_URL *url, **s_url;

			if (ds->rep->segment_base && !evt->seg_size.media_range_end) {
				if (! ds->rep->segment_base->index_range) {
					GF_SAFEALLOC(ds->rep->segment_base->index_range, GF_MPD_ByteRange);
				}
				ds->rep->segment_base->index_range->start_range = r_start;
				ds->rep->segment_base->index_range->end_range = r_end;
				ds->rep->segment_base->index_range_exact = GF_TRUE;
				flush_mpd = GF_TRUE;
				continue;
			}

			GF_SAFEALLOC(url, GF_MPD_URL);
			GF_SAFEALLOC(url->byte_range, GF_MPD_ByteRange);
			url->byte_range->start_range = r_start;
			url->byte_range->end_range = r_end;

			s_url = NULL;
			if (ds->rep->segment_base) {
				if (evt->seg_size.media_range_end) s_url = &ds->rep->segment_base->initialization_segment;
			} else {
				assert(ds->rep->segment_list);
				if (evt->seg_size.media_range_end) s_url = &ds->rep->segment_list->initialization_segment;
				else s_url = &ds->rep->segment_list->representation_index;
			}
			assert(s_url);
			if (*s_url) gf_mpd_url_free(*s_url);
			*s_url = url;
		} else if (ds->rep->segment_list && !evt->seg_size.is_init) {
			GF_MPD_SegmentURL *url = gf_list_pop_front(ds->seg_urls);
			assert(url);
			assert(ctx->nb_seg_url_pending);
			ctx->nb_seg_url_pending--;

			if (!url->media && ctx->single_file) {
				GF_SAFEALLOC(url->media_range, GF_MPD_ByteRange);
				url->media_range->start_range = evt->seg_size.media_range_start;
				url->media_range->end_range = evt->seg_size.media_range_end;
			}
			if (evt->seg_size.idx_range_end) {
				GF_SAFEALLOC(url->index_range, GF_MPD_ByteRange);
				url->index_range->start_range = evt->seg_size.idx_range_start;
				url->index_range->end_range = evt->seg_size.idx_range_end;
			}
		}
	}
	if (!ctx->single_segment || !flush_mpd) return GF_TRUE;

	flush_mpd = GF_TRUE;
	for (i=0; i<count; i++) {
		GF_DashStream *ds = gf_list_get(ctx->pids, i);
		if (!ds->rep) continue;
		if (! ds->rep->segment_base) continue;
		if (ds->rep->segment_base->index_range) continue;
		flush_mpd = GF_FALSE;
		break;
	}
	if (flush_mpd) {
		ctx->on_demand_done = GF_TRUE;
	}
	return GF_TRUE;
}

static GF_Err dasher_setup_profile(GF_DasherCtx *ctx)
{
	switch (ctx->profile) {
	case GF_DASH_PROFILE_AVC264_LIVE:
	case GF_DASH_PROFILE_AVC264_ONDEMAND:
		if (ctx->cp == GF_DASH_CPMODE_REPRESENTATION) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_DASH, ("[Dasher] ERROR! The selected DASH profile (DASH-IF IOP) requires the ContentProtection element to be present in the AdaptationSet element.\n"));
			return GF_BAD_PARAM;
		}
	default:
		break;
	}

	/*adjust params based on profiles*/
	switch (ctx->profile) {
	case GF_DASH_PROFILE_LIVE:
		ctx->single_segment = ctx->single_file = GF_FALSE;
		ctx->tpl = ctx->align = ctx->sap = GF_TRUE;
		break;
	case GF_DASH_PROFILE_HBBTV_1_5_ISOBMF_LIVE:
		ctx->check_main_role = GF_TRUE;
		ctx->bs_switch = DASHER_BS_SWITCH_MULTI;
		GF_LOG(GF_LOG_ERROR, GF_LOG_DASH, ("[Dasher] HBBTV1.5 profile not yet ported to filter architecture.\n"));
		//FALLTHROUGH
	case GF_DASH_PROFILE_AVC264_LIVE:
		ctx->single_segment = ctx->single_file = GF_FALSE;
		ctx->no_fragments_defaults = ctx->align = ctx->tpl = ctx->sap = GF_TRUE;
		break;
	case GF_DASH_PROFILE_AVC264_ONDEMAND:
		ctx->tpl = GF_FALSE;
		ctx->no_fragments_defaults = ctx->align = ctx->single_segment = ctx->sap = GF_TRUE;
		break;
	case GF_DASH_PROFILE_ONDEMAND:
		ctx->single_segment = ctx->align = ctx->sap = ctx->single_file = GF_TRUE;
		ctx->tpl = GF_FALSE;
		if ((ctx->bs_switch != DASHER_BS_SWITCH_DEF) && (ctx->bs_switch != DASHER_BS_SWITCH_OFF)) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] onDemand profile, bitstream switching mode cannot be used, defaulting to off.\n"));
		}
		/*BS switching is meaningless in onDemand profile*/
		ctx->bs_switch = DASHER_BS_SWITCH_OFF;
		break;
	case GF_DASH_PROFILE_MAIN:
		ctx->align = ctx->sap = GF_TRUE;
		ctx->single_segment = ctx->tpl = GF_FALSE;
		break;
	default:
		break;
	}
		//commented out, not sure why we had inband by default in live
	if (ctx->bs_switch == DASHER_BS_SWITCH_DEF) {
#if 0
		ctx->bs_switch = DASHER_BS_SWITCH_INBAND;
#else
		ctx->bs_switch = DASHER_BS_SWITCH_ON;
#endif

	}

	if (! ctx->align) {
		if (ctx->profile != GF_DASH_PROFILE_FULL) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] Segments are not time-aligned in each representation of each period\n\tswitching to FULL profile\n"));
			ctx->profile = GF_DASH_PROFILE_FULL;
		}
		//commented out, this does not seem correct since BS switching is orthogonal to segment alignment
		//one could have inband params working even in non time-aligned setup
#if 0
		if (ctx->bs_switch != DASHER_BS_SWITCH_OFF) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[Dasher] Segments are not time-aligned in each representation of each period\n\tdisabling bitstream switching\n"));
			ctx->bs_switch = DASHER_BS_SWITCH_OFF;
		}
#endif

	}

	//check we have a segment template
	if (!ctx->template) {
		ctx->template = gf_strdup( ctx->single_file ? "$File$_dash" : "$File$_$Number$" );
		GF_LOG(GF_LOG_INFO, GF_LOG_DASH, ("[Dasher] No template assigned, using %s\n", ctx->template));
	}

	if (ctx->single_segment) {
		ctx->subs_per_sidx = 0;
	}
	return GF_OK;
}

static GF_Err dasher_initialize(GF_Filter *filter)
{
	GF_Err e;
	GF_DasherCtx *ctx = gf_filter_get_udta(filter);
	gf_filter_sep_max_extra_input_pids(filter, -1);

	ctx->pids = gf_list_new();

	e = dasher_setup_profile(ctx);
	if (e) return e;

	if (!ctx->ext) ctx->ext = "m4s";
	if (ctx->single_file && ctx->tpl)
		ctx->tpl = GF_FALSE;

	ctx->current_period = dasher_new_period();
	ctx->next_period = dasher_new_period();
	ctx->on_demand_done = GF_TRUE;

	if (ctx->state) {
		ctx->first_context_load = GF_TRUE;
	}
	return GF_OK;
}


static void dasher_finalize(GF_Filter *filter)
{
	GF_DasherCtx *ctx = gf_filter_get_udta(filter);

	while (gf_list_count(ctx->pids)) {
		GF_DashStream *ds = gf_list_pop_back(ctx->pids);
		dasher_reset_stream(ds, GF_TRUE);
		gf_free(ds);
	}
	gf_list_del(ctx->pids);
	if (ctx->mpd) gf_mpd_del(ctx->mpd);

	if (ctx->next_period->period) gf_mpd_period_free(ctx->next_period->period);
	gf_list_del(ctx->current_period->streams);
	gf_free(ctx->current_period);
	gf_list_del(ctx->next_period->streams);
	gf_free(ctx->next_period);
	if (ctx->out_path) gf_free(ctx->out_path);
}

static const GF_FilterCapability DasherCaps[] =
{
	//we accept files as input, but only for NULL file (no source)
	CAP_UINT(GF_CAPS_INPUT,  GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
	//only with no source
	CAP_STRING(GF_CAPS_INPUT_EXCLUDED, GF_PROP_PID_URL, "*"),
	CAP_STRING(GF_CAPS_INPUT_EXCLUDED, GF_PROP_PID_FILEPATH, "*"),

	CAP_UINT(GF_CAPS_OUTPUT_STATIC, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
	CAP_STRING(GF_CAPS_OUTPUT_STATIC, GF_PROP_PID_FILE_EXT, "mpd|m3u8"),
	{0},
	//anything else
	CAP_UINT(GF_CAPS_INPUT_EXCLUDED,  GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
	//only framed
	CAP_BOOL(GF_CAPS_INPUT_EXCLUDED, GF_PROP_PID_UNFRAMED, GF_TRUE),

};


#define OFFS(_n)	#_n, offsetof(GF_DasherCtx, _n)
static const GF_FilterArgs DasherArgs[] =
{
	{ OFFS(dur), "DASH target duration in seconds", GF_PROP_DOUBLE, "1.0", NULL, GF_FALSE},
	{ OFFS(single_segment), "single segment is used", GF_PROP_BOOL, "false", NULL, GF_FALSE},
	{ OFFS(tpl), "use template mode (multiple segment, template URLs)", GF_PROP_BOOL, "true", NULL, GF_FALSE},
	{ OFFS(stl), "use segment timeline (ignored in on_demand mode)", GF_PROP_BOOL, "false", NULL, GF_FALSE},
	{ OFFS(dynamic), "MPD is dynamic (live generation)", GF_PROP_BOOL, "false", NULL, GF_FALSE},
	{ OFFS(single_file), "Segments are contained in a single file (default in on_demand)", GF_PROP_BOOL, "false", NULL, GF_FALSE},
	{ OFFS(align), "Enables segment time alignment between representations", GF_PROP_BOOL, "true", NULL, GF_FALSE},
	{ OFFS(sap), "Enables spliting segments at SAP boundaries", GF_PROP_BOOL, "true", NULL, GF_FALSE},
	{ OFFS(mix_codecs), "Enables mixing different codecs in an adaptation set", GF_PROP_BOOL, "false", NULL, GF_FALSE},
	{ OFFS(ntp), "Inserts/overrides NTP clock at the begining of each segment. rem removes NTP from all input packets. yes inserts NTP at each segment start. keep leaves input packet NTP untouched.", GF_PROP_UINT, "rem", "rem|yes|keep", GF_FALSE},
	{ OFFS(no_sar), "Does not check for identical sample aspect ratio for adaptation sets", GF_PROP_BOOL, "false", NULL, GF_FALSE},
	{ OFFS(for_test), "sets all dates and version info to 0 to enforce same binary result generation", GF_PROP_BOOL, "false", NULL, GF_FALSE},
	{ OFFS(forcep), "forces profile string for avc/hevc/aac", GF_PROP_BOOL, "false", NULL, GF_FALSE},
	{ OFFS(bs_switch), "Bitstream switching mode (single init segment):\n\tdef: resolves to off for onDemand and inband for live\n\toff: disables BS switching\n\ton: enables it if same decoder configuration is possible\n\tinband: moves decoder config inband if possible\n\tforce: enables it even if only one representation\n\tmulti: uses multiple stsd entries in ISOBMFF", GF_PROP_UINT, "def", "def|off|on|inband|force|multi", GF_FALSE},
	{ OFFS(avcp), "AVC|H264 profile to use if no profile could be found. If forcep is set, enforces this profile", GF_PROP_STRING, NULL, NULL, GF_FALSE},
	{ OFFS(hvcp), "HEVC profile to use if no profile could be found. If forcep is set, enforces this profile", GF_PROP_STRING, NULL, NULL, GF_FALSE},
	{ OFFS(aacp), "AAC profile to use if no profile could be found. If forcep is set, enforces this profile", GF_PROP_STRING, NULL, NULL, GF_FALSE},
	{ OFFS(template), "DASH template string to use to generate segment name - see filter help", GF_PROP_STRING, NULL, NULL, GF_FALSE},
	{ OFFS(ext), "File extension to use for segments", GF_PROP_STRING, "m4s", NULL, GF_FALSE},
	{ OFFS(asto), "AvailabilityStartTime offset to use", GF_PROP_UINT, "0", NULL, GF_FALSE},
	{ OFFS(profile), "Specifies the target DASH profile. This will set default option values to ensure conformance to the desired profile. Auto turns profile to live for dynamic and full for non-dynamic.", GF_PROP_UINT, "auto", "auto|live|onDemand|main|full|hbbtv1.5.live|dashavc264.live|dashavc264.onDemand", GF_FALSE },
	{ OFFS(profX), "specifies a list of profile extensions, as used by DASH-IF and DVB. The string will be colon-concatenated with the profile used", GF_PROP_STRING, NULL, NULL, GF_FALSE },
	{ OFFS(cp), "Specifies the content protection element location", GF_PROP_UINT, "set", "set|rep|both", GF_FALSE },
	{ OFFS(buf), "DASH min buffer duration in ms. negative value means percent of segment duration (eg -150 = 1.5*seg_dur)", GF_PROP_SINT, "-100", NULL, GF_FALSE},
	{ OFFS(timescale), "sets timescales for timeline and segment list/template. A value of 0 picks up the first timescale of the first stream in an adaptation set. A negative value forces using stream timescales for each timed element (multiplication of segment list/template/timelines). A positive value enforces the MPD timescale", GF_PROP_SINT, "0", NULL, GF_FALSE},
	{ OFFS(check_dur), "checks duration of sources in period, trying to have roughly equal duration. Enforced whenever period start times are used", GF_PROP_BOOL, "true", NULL, GF_FALSE},
	{ OFFS(skip_seg), "increments segment number whenever an empty segment would be produced - NOT DASH COMPLIANT", GF_PROP_BOOL, "false", NULL, GF_FALSE},
	{ OFFS(title), "sets MPD title", GF_PROP_STRING, NULL, NULL, GF_FALSE},
	{ OFFS(source), "sets MPD Source", GF_PROP_STRING, NULL, NULL, GF_FALSE},
	{ OFFS(info), "sets MPD info url", GF_PROP_STRING, NULL, NULL, GF_FALSE},
	{ OFFS(cprt), "adds copyright string to MPD", GF_PROP_STRING, NULL, NULL, GF_FALSE},
	{ OFFS(lang), "sets lang of MPD Info", GF_PROP_STRING, NULL, NULL, GF_FALSE},
	{ OFFS(location), "sets MPD locations to given URL", GF_PROP_STRING_LIST, NULL, NULL, GF_FALSE},
	{ OFFS(base), "sets base URLs of MPD", GF_PROP_STRING_LIST, NULL, NULL, GF_FALSE},
	{ OFFS(refresh), "MPD refresh rate for dynamic, in seconds. A negative value sets the MPD duration. If 0, uses dash duration", GF_PROP_DOUBLE, "0", NULL, GF_FALSE},
	{ OFFS(tsb), "Sets time-shift buffer depth in seconds. A negative value means infinity", GF_PROP_DOUBLE, "0", NULL, GF_FALSE},
	{ OFFS(subdur), "specifies maximum duration of the input file to be segmentated. This does not change the segment duration, segmentation stops once segments produced exceeded the duration.", GF_PROP_DOUBLE, "0", NULL, GF_FALSE},
	{ OFFS(ast), "for live mode, sets start date (as xs:date, eg YYYY-MM-DDTHH:MM:SSZ. Default is now. !! Do not use with multiple periods, nor when DASH duration is not a multiple of GOP size !!", GF_PROP_STRING, NULL, NULL, GF_FALSE},
	{ OFFS(state), "path to file used to store/reload state info when simulating live. This is stored as a valid MPD with GPAC XML extensions", GF_PROP_STRING, NULL, NULL, GF_FALSE},
	{ OFFS(split), "enables cloning samples for text/metadata/scene description streams, marking further clones as redundant", GF_PROP_BOOL, "true", NULL, GF_FALSE},
	{0}
};


GF_FilterRegister DasherRegister = {
	.name = "dasher",
	.description = "MPEG-DASH / HLS / Smooth segmenter",
	.comment = "GPAC DASH segmenter\n"\
			"The segmenter uses template strings to derive output file names, regardless of the DASH mode (even when templates are not used)\n"\
			"The default template is $File$_dash for ondemand and single file modes, and $File$_$Number$ for seperate segment files\n"\
			"\tEX: template=Great_$File$_$Width$_$Number$ on 640x360 foo.mp4 source will resolve in Great_foo_640_$Number$ for the DASH template\n"\
			"\tEX: template=Great_$File$_$Width$ on 640x360 foo.mp4 source will resolve in Great_foo_640.mp4 for onDemand case\n"\
			"\n"\
			"Standard DASH replacement strings\n"\
	        "\t$Number[%%0Nd]$: is replaced by the segment number, possibly prefixed with 0\n"\
	        "\t$RepresentationID$ is replaced by representation name\n"\
	        "\t$Time$ is replaced by segment start time\n"\
	        "\t$Bandwidth$ is replaced by representation bandwidth.\n"\
			"\n"\
			"Additionnal replacement strings (not DASH, not generic GPAC replacements but may occur multiple times in template):\n"\
	        "\t$Init=NAME$ is replaced by NAME for init segment, ignored otherwise\n"\
	        "\t$Index=NAME$ is replaced by NAME for index segments, ignored otherwise\n"\
	        "\t$Path=PATH$ is replaced by PATH when creating segments, ignored otherwise\n"\
	        "\t$Segment=NAME$ is replaced by NAME for media segments, ignored for init segments\n"\
			"\n"\

			"To assign PIDs into periods and adaptation sets and configure the session, the dasher looks for the following properties on each input pid:\n"\
			"\tRepresentation: assigns representation ID to input pid. If not set, the default behaviour is to have each media component in different adaptation sets. Setting the RepresentationID allows explicit multiplexing of the source(s)\n"\
			"\tPeriod: assigns period ID to input pid. If not set, the default behaviour is to have all media in the same period with the same start time\n"
			"\tPStart: assigns period start. If not set, 0 is assumed, and periods appear in the Period ID declaration order. If negative, this gives the period order (-1 first, then -2 ...). If positive, this gives the true start time and will abort DASHing at period end\n"\
			"\t\tWhen both positive and negative values are found, the by-order periods (negative) will be inserted AFTER the timed period (positive)\n"\
			"\txlink: for remote periods, only checked for null pid\n"\
			"\tRole, PDesc, ASDesc, ASCDesc, RDesc: various descriptors to set for period, AS or representation\n"\
			"\tBUrl: base URLs to use for the pid (per representation)\n"\
			"\tTemplate: overrides dasher template for this PID\n"\
			"\tDashDur: overrides dasher segment duration for this PID\n"\
			"\tStartNumber: sets the start number for the first segment in the PID, default is 1\n"
			"\tNon-dash properties: Bitrate, SAR, Language, Width, Height, SampleRate, NumChannels, Language, ID, DependencyID, FPS, Interlaced. These properties are used to setup each representation and can be overriden on input PIDs using the general PID property settings (cf global help).\n"\
			"\tEX: \"src=test.mp4:#Bitrate=1M dst=test.mpd\" will force declaring a bitrate of 1M for the representation, regardless of actual source bitrate\n"\
			"\tEX: \"src=muxav.mp4 dst=test.mpd\" will create unmuxed DASH segments\n"\
			"\tEX: \"src=muxav.mp4:#Representation=1 dst=test.mpd\" will create muxed DASH segments\n"\
			"\tEX: \"src=m1.mp4 src=m2.mp4:#Period=Yep dst=test.mpd\" will put src m1.mp4 in first period, m2.mp4 in second period\n"\
			"\tEX: \"src=m1.mp4:#BUrl=http://foo/bar dst=test.mpd\" will assign a base URL to src m1.mp4\n"\
			"\tEX: \"src=m1.mp4:#ASCDesc=<ElemName val=\"attval\">text</ElemName> dst=test.mpd\" will assign the specified XML descriptor to the adaptation set.\n"\
			"\t\tNote that this can be used to inject most DASH descriptors not natively handled by the dasher\n"\
			"\t\tThe dasher handles the XML descriptor as a string and does not attempt to validate it.\n"\
			"\t\tDescriptors, as well as some dasher filter arguments, are string lists (comma-separated by default), so that multiple descriptors can be added:\n"\
			"\tEX: \"src=m1.mp4:#RDesc=<Elem attribute=\"1\"/>,<Elem2>text</Elem2> dst=test.mpd\" will insert two descriptors in the representation(s) of m1.mp4\n"\
			"\tEX: \"src=video.mp4:#Template=foo$Number$ src=audio.mp4:#Template=bar$Number$ dst=test.mpd\" will assign different templates to the audio and video sources.\n"\
			"\tEX: \"src=null:#xlink=http://foo/bar.xml:#PDur=4 src=m.mp4:#PStart=-1\" will insert an create an MPD with first a remote period then a regular one\n"\
			"\tEX: \"src=null:#xlink=http://foo/bar.xml:#PStart=6 src=m.mp4\" will insert an create an MPD with first a regular period, dashing ony 6s of content, then a remote one\n"\
			"\n"\
			"The dasher will create muxing filter chains for each representation and will reassign PID IDs\n"\
			"so that each media component (video, audio, ...) in an adaptation set has the same ID\n"\
			"\n"\
			"Note to developpers: output muxers allowing segmented output must obey the following:\n"\
			"* add a \"DashMode\" capability to their input caps (value of the cap is ignored, only its presence is required)\n"\
			"* inspect packet properties, \"FileNumber\" giving the signal of a new DASH segment, \"FileName\" giving the optional file name (if not present, ouput shall be a single file). \n"\
			"\t\t\"FileName\" property is only set for packet carrying the \"FileNumber\" property\n"\
			"\t\t\"FileName\" property is only on one PID (usually the first) for multiplexed outputs\n"\
			"* for each segment done, send a downstream event on the first connected PID signaling the size of the segment and the size of its index if any\n"\
			"* for muxers with init data, send a downstream event signaling the size of the init and the size of the global index if any\n"\
			"* the following filter options are passed to muxers, which should declare them as arguments:\n"\
			"\t\tnoinit: disables output of init segment for the muxer (used to handle bitstream switching with single init in DASH)\n"\
			"\t\tfrag: indicates muxer shall used fragmented format (used for ISOBMFF mostly)\n"\
			"\t\tsubs_sidx=0: indicates an SIDX shall be generated - only added if not already specified by user\n"\
			"\t\txps_inband=all|no: indicates AVC/HEVC/... parameter sets shall be sent inband or out of band\n"\
			"\t\tno_frags_def: indicates fragment defaults should be set in each segment rather than in init segment\n"\
			"\n"\
			"The dasher will add the following properties to the output PIDs:\n"\
			"* DashMode: identifies VoD (single file with global index) or regular DASH mode used by dasher\n"\
			"* DashDur: identifies target DASH segment duration - this can be used to estimate the SIDX size for example\n"\
			,
	.private_size = sizeof(GF_DasherCtx),
	.args = DasherArgs,
	.initialize = dasher_initialize,
	.finalize = dasher_finalize,
	SETCAPS(DasherCaps),
	.configure_pid = dasher_configure_pid,
	.process = dasher_process,
	.process_event = dasher_process_event,
};


const GF_FilterRegister *dasher_register(GF_FilterSession *session)
{
	return &DasherRegister;
}