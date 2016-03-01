#include "oss_media_ts_stream.h"

static int16_t oss_media_get_digit_num(int32_t value)
{
    int16_t digit_num = 0;
    do {
        value = value / 10;
        digit_num++;
    } while (value > 0);
    return digit_num;
}


static char *oss_media_create_new_ts_file_name(
        const oss_media_ts_stream_option_t *option,
        oss_media_ts_stream_t *stream)
{
        
    int16_t pos_digit_num = oss_media_get_digit_num(stream->ts_file_index);
    char pos_str[pos_digit_num + 1];
    sprintf(pos_str, "%d", stream->ts_file_index++);
    
    return apr_psprintf(stream->pool, "%.*s%.*s%.*s",
                        strlen(option->ts_name_prefix), option->ts_name_prefix,
                        strlen(pos_str), pos_str,
                        strlen(OSS_MEDIA_TS_FILE_SURFIX), OSS_MEDIA_TS_FILE_SURFIX);
}


int oss_media_ts_stream_open(auth_fn_t auth_func,
                             const oss_media_ts_stream_option_t *option,
                             oss_media_ts_stream_t **stream)
{
    *stream = (oss_media_ts_stream_t*)malloc(sizeof(oss_media_ts_stream_t));
    (*stream)->option = option;
    (*stream)->ts_file_index = 0;
    (*stream)->current_file_begin_pts = -1;

    aos_pool_create(&(*stream)->pool, NULL);

    char *ts_file_name = oss_media_create_new_ts_file_name(option, *stream);
    int ret = oss_media_ts_open(option->bucket_name, ts_file_name,
                                auth_func, &((*stream)->ts_file));
    if (ret != 0) {
        aos_error_log("open ts file[%s] failed.", ts_file_name);
        return ret;
    }

    ret = oss_media_ts_open(option->bucket_name, option->m3u8_name,
                            auth_func, &((*stream)->m3u8_file));
    if (ret != 0) {
        aos_error_log("open m3u8 file[%s] failed.", option->m3u8_name);
        return ret;
    }

    // update m3u8 file mode to 'w' for live scene
    if (option->is_live) {
        (*stream)->m3u8_file->file->mode = "w";
        (*stream)->m3u8_infos = (oss_media_ts_m3u8_info_t*)malloc(
                sizeof(oss_media_ts_m3u8_info_t) * option->hls_list_size);
    } else {
        (*stream)->m3u8_infos = (oss_media_ts_m3u8_info_t*)malloc(
                sizeof(oss_media_ts_m3u8_info_t));
    }

    (*stream)->video_frame = 
        (oss_media_ts_frame_t*)malloc(sizeof(oss_media_ts_frame_t));
    (*stream)->video_frame->stream_type = st_h264;
    (*stream)->video_frame->continuity_counter = 1;
    (*stream)->video_frame->key = 1;
    (*stream)->video_frame->pts = 5000;
    (*stream)->video_frame->dts = 5000;
    (*stream)->video_frame->pos = NULL;
    (*stream)->video_frame->end = NULL;

    (*stream)->audio_frame = 
        (oss_media_ts_frame_t*)malloc(sizeof(oss_media_ts_frame_t));
    (*stream)->audio_frame->stream_type = st_aac;
    (*stream)->audio_frame->continuity_counter = 1;
    (*stream)->audio_frame->key = 1;
    (*stream)->audio_frame->pts = 5000;
    (*stream)->audio_frame->dts = 5000;
    (*stream)->audio_frame->pos = NULL;
    (*stream)->audio_frame->end = NULL;

    return 0;
}

static char* oss_media_create_ts_full_url(aos_pool_t *pool,
        oss_media_file_t *file) 
{
    oss_config_t config;
    aos_str_set(&config.endpoint, file->endpoint);
    config.is_cname = file->is_cname;

    oss_request_options_t options;
    options.config = &config;
    options.pool = pool;

    aos_http_request_t req;
    aos_string_t bucket;
    aos_string_t key;
    aos_str_set(&bucket, file->bucket_name);
    aos_str_set(&key, file->object_key);

    oss_get_object_uri(&options, &bucket, &key, &req);
    
    char *proto = AOS_HTTP_PREFIX;
    if (req.proto != NULL && 0 != strlen(req.proto)) {
        proto = req.proto;
    }

    return apr_psprintf(pool, "%.*s%.*s/%.*s%",
                        strlen(proto), proto,
                        strlen(req.host), req.host,
                        strlen(req.uri), req.uri);
}

static void oss_media_set_m3u8_info(int32_t pos,
                                    float duration,
                                    oss_media_ts_stream_t *stream)
{
    aos_pool_t *sub_pool;
    aos_pool_create(&sub_pool, stream->pool);
    char *ts_url = oss_media_create_ts_full_url(sub_pool, stream->ts_file->file);
    memcpy(stream->m3u8_infos[pos].url, ts_url, strlen(ts_url) + 1);
    aos_pool_destroy(sub_pool);
    
    stream->m3u8_infos[pos].duration = duration;
}

static int oss_media_write_m3u8(float duration,
                                oss_media_ts_stream_t *stream)
{
    int i;
    int ret;
    int32_t pos;
    int32_t cur_m3u8_count;
    
    if (stream->option->is_live) {
        int32_t hls_list_size = stream->option->hls_list_size;
        cur_m3u8_count = hls_list_size > stream->ts_file_index ? 
                         stream->ts_file_index : hls_list_size;
        
        oss_media_ts_begin_m3u8(stream->option->hls_time,
                                stream->ts_file_index - cur_m3u8_count,
                                stream->m3u8_file);
        
        if (stream->ts_file_index > hls_list_size) {
            for (i = 0; i < hls_list_size - 1; i++) {
                stream->m3u8_infos[i].duration = stream->m3u8_infos[i + 1].duration;
                memcpy(stream->m3u8_infos[i].url, stream->m3u8_infos[i+1].url,
                       OSS_MEDIA_M3U8_URL_LENGTH);
            }
        }
    } else {
        if (stream->m3u8_file->file->_stat.length == 0) {
            oss_media_ts_begin_m3u8(stream->option->hls_time, 0,
                    stream->m3u8_file);
        }

        cur_m3u8_count = 1;
    }

    pos = cur_m3u8_count - 1;
    oss_media_set_m3u8_info(pos, duration, stream);
    
    return oss_media_ts_write_m3u8(cur_m3u8_count, stream->m3u8_infos,
                                   stream->m3u8_file);
}

static int close_and_open_new_file(oss_media_ts_stream_t *stream) {
    int ret;
    
    // temp store auth_func point
    auth_fn_t auth_func = stream->ts_file->file->auth_func;

    // close current ts file
    ret = oss_media_ts_close(stream->ts_file);
    if (ret != 0) {
        aos_error_log("close ts file failed.");
        return ret;
    }

    // open next ts file
    char *ts_file_name = oss_media_create_new_ts_file_name(stream->option, stream);
    ret = oss_media_ts_open(stream->option->bucket_name, ts_file_name,
                            auth_func, &stream->ts_file);
    if (ret != 0) {
        aos_error_log("open ts file[%s] failed.", ts_file_name);
        return -1;
    }
    stream->current_file_begin_pts = -1;

    return 0;
}

static int oss_media_ts_stream_flush(float duration,
                                     oss_media_ts_stream_t *stream) 
{
    int ret;

    // flush ts file
    oss_media_ts_flush(stream->ts_file);
        
    // write m3u8 index
    ret = oss_media_write_m3u8(duration, stream);
    if (ret != 0) {
        aos_error_log("write m3u8 file failed.", stream->option->m3u8_name);
        return ret;
    }
    
    return 0;
}

static int oss_media_need_flush(float duration, 
                                int32_t hls_time,
                                oss_media_ts_frame_t *frame)
{
    if (duration < hls_time) {
        return 0;
    }
    
    if (frame->stream_type == st_aac || frame->stream_type == st_mp3) {
        return 0;
    }

    return 1;
}

static int oss_media_write_stream_frame(oss_media_ts_frame_t *frame,
                                        oss_media_ts_stream_t *stream)
{
    if (stream->current_file_begin_pts == -1) {
        stream->current_file_begin_pts = frame->pts;
    }

    float duration = (frame->pts - stream->current_file_begin_pts) / 90000.0;
    if (oss_media_need_flush(duration, stream->option->hls_time, frame))
    {
        if(0 != oss_media_ts_stream_flush(duration, stream)) {
            aos_error_log("flush stream data failed.");
            return -1;
        }

        if (0 != close_and_open_new_file(stream)) {
            aos_error_log("close file and open new file failed.");
            return -1;
        }
    }
    
    int ret = oss_media_ts_write_frame(frame, stream->ts_file);
    if (ret != 0) {
        aos_error_log("write frame failed.");
        return ret;
    }
    return 0;
}

static int oss_media_extract_frame(uint8_t *buf, 
                                   int64_t last_pos, 
                                   int64_t cur_pos, 
                                   uint64_t inc_pts,
                                   oss_media_ts_frame_t *frame)
{
    if (last_pos != -1 && cur_pos > last_pos) {
        frame->pts += inc_pts;
        frame->dts = frame->pts;
        
        frame->pos = buf + last_pos;
        frame->end = buf + cur_pos;
        return 1;
    }
    return 0;
}

static int oss_media_get_video_frame(uint8_t *buf, uint64_t len,
                                     oss_media_ts_stream_t *stream)
{
    int i = 0;
    int64_t cur_pos = -1;
    int64_t last_pos = -1;
    int64_t inc_pts = 0;
    oss_media_ts_frame_t *frame = stream->video_frame;

    if (len <= 0) {
        aos_debug_log("no video data occur, ignore.");
        return 0;
    }

    if (frame->pos != frame->end) {
        aos_debug_log("pre video frame is not consume, continue");
        return 1;
    }

    last_pos = frame->pos - buf;
    cur_pos = last_pos;
    inc_pts = 90 * stream->option->video_frame_rate;
    for (i = frame->end - buf; i < len - 3; i++) {
        if ((buf[i] & 0x0F) == 0x00 && buf[i+1] == 0x00
            && buf[i+2] == 0x00 && buf[i+3] == 0x01)
        {
            cur_pos = i;
        }

        if (oss_media_extract_frame(buf, last_pos, cur_pos, inc_pts, frame)) {
            return 1;
        }
    }

    if (oss_media_extract_frame(buf, last_pos, len, inc_pts, frame)) {
        return 1;
    }

    return 0;
}

static int oss_media_get_samples_per_frame(const oss_media_ts_stream_t *stream) 
{
    return stream->audio_frame->stream_type == st_mp3 ? 
        OSS_MEDIA_MP3_SAMPLE_RATE : OSS_MEDIA_AAC_SAMPLE_RATE;
}


static int oss_media_get_audio_frame(uint8_t *buf, uint64_t len, 
                                     oss_media_ts_stream_t *stream)
{
    int i = 0;
    int64_t cur_pos = -1;
    int64_t last_pos = -1;
    int64_t inc_pts = 0;
    int samples_per_frame = 0;
    oss_media_ts_frame_t *frame = NULL;

    if (len <= 0) {
        aos_debug_log("no audio data occur, ignore.");
        return 0;
    }

    frame = stream->audio_frame;

    if (frame->pos != frame->end) {
        aos_debug_log("pre audio frame is not consume, continue");
        return 1;
    }

    last_pos = frame->pos - buf;
    samples_per_frame = oss_media_get_samples_per_frame(stream);
    inc_pts = (90000 * samples_per_frame) / stream->option->audio_sample_rate;
    
    for (i = frame->end - buf; i < len - 1; i++) {
        if (buf[i] == 0xFF && (buf[i+1] & 0xF0) == 0xF0)
        {
            cur_pos = i;
        }

        if (oss_media_extract_frame(buf, last_pos, cur_pos, inc_pts, frame)) {
            return 1;
        }
    }

    if (oss_media_extract_frame(buf, last_pos, len, inc_pts, frame)) {
        return 1;
    }
    return 0;
}

static void oss_media_sync_pts_dts(oss_media_ts_stream_t *stream)
{
    if (stream->video_frame->pts <= stream->audio_frame->pts) {
        stream->video_frame->pts = stream->audio_frame->pts;
        stream->video_frame->dts = stream->audio_frame->dts;
    } else {
        stream->audio_frame->pts = stream->video_frame->pts;
        stream->audio_frame->dts = stream->video_frame->dts;
    }
}

int oss_media_ts_stream_write(uint8_t *video_buf,
                              uint64_t video_len,
                              uint8_t *audio_buf,
                              uint64_t audio_len,
                              oss_media_ts_stream_t *stream)
{
    stream->video_frame->end = video_buf;
    stream->video_frame->pos = video_buf;
    stream->audio_frame->end = audio_buf;
    stream->audio_frame->pos = audio_buf;

    oss_media_sync_pts_dts(stream);

    while (1)
    {
        int ret;
        int get_video_ret = oss_media_get_video_frame(video_buf, video_len, stream);
        int get_audio_ret = oss_media_get_audio_frame(audio_buf, audio_len, stream);

        if (get_video_ret && get_audio_ret) {
            if (stream->video_frame->pts < stream->audio_frame->pts) {
                ret = oss_media_write_stream_frame(stream->video_frame, stream);
            } else if (stream->video_frame->pts == stream->audio_frame->pts) {
                ret = oss_media_write_stream_frame(stream->video_frame, stream);
                ret = ret && oss_media_write_stream_frame(stream->audio_frame, stream);
            } else {
                ret = oss_media_write_stream_frame(stream->audio_frame, stream);
            }
        } else if (get_video_ret && !get_audio_ret) {
            ret = oss_media_write_stream_frame(stream->video_frame, stream);
        } else if (!get_video_ret && get_audio_ret) {
            ret = oss_media_write_stream_frame(stream->audio_frame, stream);
        } else {
            break;
        }
        
        if (ret != 0) {
            aos_error_log("write stream frame failed.");
            return ret;
        }
    }

    return 0;
}

int oss_media_ts_stream_close(oss_media_ts_stream_t *stream) {
    float duration = 0.0;
    if (stream->audio_frame->pts > stream->current_file_begin_pts) {
        duration = (stream->audio_frame->pts - 
                    stream->current_file_begin_pts) / 90000.0;
    }
    if (stream->video_frame->pts > stream->current_file_begin_pts) {
        float video_duration = (stream->video_frame->pts - 
                                stream->current_file_begin_pts) / 90000.0;
        if (video_duration - duration > 0.00001) {
            duration = video_duration;
        }
    }

    if (duration > 0.0) {
        if (oss_media_ts_stream_flush(duration, stream) != 0) {
            aos_error_log("flush stream failed.");
            return -1;
        }
    }

    if (oss_media_ts_close(stream->ts_file) != 0) {
        aos_error_log("close ts file failed.");
        return -1;
    }

    // write end flag to m3u8 file and close for vod ts
    if (!stream->option->is_live) {
        oss_media_ts_end_m3u8(stream->m3u8_file);
    }

    if (oss_media_ts_close(stream->m3u8_file) != 0) {
        aos_error_log("close m3u8 file failed.");
        return -1;
    }

    aos_pool_destroy(stream->pool);
    
    free(stream->m3u8_infos);
    free(stream->audio_frame);
    free(stream->video_frame);

    free(stream);
    stream = NULL;

    return 0;
}