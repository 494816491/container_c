#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/avassert.h>
#include <pthread.h>
#include "container.h"
#include "debug.h"

#define REGISTER_INDEX 0
#define REGISTER_FILE_SIZE_BOUND 120000

struct global_param_s{
    get_ripe_stream_fun ripe_func;
    void *ripe_data;
    struct chn_param_s *chn_params;
};
//muxer
struct chn_param_s{
    int chn_num;
    bool video_enable;
    bool audio_enable;
    bool audio_write_ready;
    bool is_writing_file;
    char file_dir[MAX_FILE_NAME_LEN];
    char file_name[MAX_FILE_NAME_LEN];
    uint64_t  last_time;

    SIZE_S video_size;
    int video_frame_rate;

    int audio_sample_rate;
    int audio_bit_width;
    int audio_sound_mode;
    int audio_num_pre_frame;

    AVFormatContext *out_context;
    AVStream *video_stream;
    AVStream *audio_stream;

    uint64_t first_packet_pts;
    uint64_t last_packet_pts;

    int64_t register_size_bound;

    pthread_mutex_t mutex;

    char *video_codec_extradata;
    int video_codec_extradata_size;
    int should_switch_new_file_async;
};


static struct global_param_s *global_param;


int container_init(get_ripe_stream_fun callback, void *ripe_data)
{
    av_register_all();
    //av_log_set_level(AV_LOG_TRACE);
    //av_log_set_level(AV_LOG_DEBUG);
    //1.alloc global_param space
    global_param = malloc(sizeof(struct global_param_s));
    if(global_param == NULL){
        err_msg("malloc failed");
        return -1;
    }
    memset(global_param, 0, sizeof(struct global_param_s));
    //alloc space of chn_params
    global_param->chn_params = malloc(sizeof(struct chn_param_s) * 8);
    if(global_param->chn_params == NULL){
        err_msg("malloc failed");
        return -1;
    }
    memset(global_param->chn_params, 0, sizeof(struct chn_param_s) * 8);

    //initilaze mutex
    int i;
    for(i = 0; i < 8; i++){
        //set mutex recursive
        pthread_mutexattr_t    attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP);
        pthread_mutexattr_destroy(&attr);

        pthread_mutex_init(&global_param->chn_params[i].mutex, &attr);
        global_param->chn_params[i].chn_num = i;
    }

    //2.hook callback fun
    global_param->ripe_func = callback;
    global_param->ripe_data = ripe_data;

    return 0;
}

static inline int param_copy(struct chn_param_s *chn_param, struct container_param_s *param)
{
    chn_param->last_time = param->last_time;
    //convert file_name
    strncpy(chn_param->file_dir, param->file_path, sizeof(chn_param->file_name));

    chn_param->video_size.u32Height = param->video_size.u32Height;
    chn_param->video_size.u32Width = param->video_size.u32Width;
    chn_param->video_frame_rate = param->video_frame_rate;

    chn_param->audio_enable = param->audio_enable;
    chn_param->audio_sample_rate = param->audio_sample_rate;
    chn_param->audio_num_pre_frame = param->audio_num_pre_frame;

    return 0;
}
int set_container_param(int chn, struct container_param_s *param)
{
    if(chn < 0 || chn >=8){
        err_msg("chn num is wrong");
        return -1;
    }
    //calculate chn_param position
    struct chn_param_s *chn_param = global_param->chn_params + chn;

    pthread_mutex_lock(&chn_param->mutex);

    param_copy(chn_param, param);

    pthread_mutex_unlock(&chn_param->mutex);

    return 0;
}

int get_container_param(int chn, struct container_param_s *param)
{
    struct chn_param_s *chn_param = global_param->chn_params + chn;
    pthread_mutex_lock(&chn_param->mutex);

    param->last_time = chn_param->last_time;
    strncpy(param->file_path, chn_param->file_dir, sizeof(param->file_path));

    param->video_size.u32Height = chn_param->video_size.u32Height;
    param->video_size.u32Width = chn_param->video_size.u32Width;
    param->video_frame_rate = chn_param->video_frame_rate;

    param->audio_enable = chn_param->audio_enable;
    param->audio_sample_rate = chn_param->audio_sample_rate;
    param->audio_num_pre_frame = chn_param->audio_num_pre_frame;

    pthread_mutex_unlock(&chn_param->mutex);

    return 0;
}

static int add_video_stream(struct chn_param_s *chn_param)
{
    AVCodecContext *codec_context;

    chn_param->video_stream= avformat_new_stream(chn_param->out_context, NULL);
    if(chn_param->video_stream == NULL){
        err_msg("avformat_new_stream error\n");
        return -1;
    }
    codec_context = chn_param->video_stream->codec;
    //timestamp time_base
    chn_param->video_stream->time_base.num = 1;
    chn_param->video_stream->time_base.den = chn_param->video_frame_rate;

    //Mandatory fields as specified in AVCodecContext documentation must be set even if this AVCodecContext is not actually used for encoding
    codec_context = chn_param->video_stream->codec;
    //codec_context->codec_id = codec_id;
    codec_context->codec_id = AV_CODEC_ID_H264;
    codec_context->codec_type = AVMEDIA_TYPE_VIDEO;

    //must set width and height
    codec_context->width = chn_param->video_size.u32Width;
    codec_context->height = chn_param->video_size.u32Height;
    codec_context->time_base.num = 1;
    codec_context->time_base.den = chn_param->video_frame_rate;
    codec_context->gop_size = chn_param->video_frame_rate;

    //set extdata
    if(chn_param->video_codec_extradata){
        info_msg("-------------set video_codec_extradata----------");
        codec_context->extradata = av_malloc(chn_param->video_codec_extradata_size);
        memcpy(codec_context->extradata, chn_param->video_codec_extradata, chn_param->video_codec_extradata_size);
        //printf("codec_context->extradata = %p\n", ext_data);
        codec_context->extradata_size = chn_param->video_codec_extradata_size;
    }

    // some formats want stream headers to be separate
    if(chn_param->out_context->oformat->flags & AVFMT_GLOBALHEADER){
        codec_context->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }

    return 0;
}

static int add_audio_stream(struct chn_param_s *chn_param)
{
    //int ret;
    AVStream *audio_stream;
    AVCodecContext *codec_context;
#if 1
    chn_param->audio_stream = avformat_new_stream(chn_param->out_context, NULL);
    if(chn_param->audio_stream == NULL){
        err_msg( "avformat_new_stream error,audio_stream = %p\n", chn_param->audio_stream);
        return -1;
    }
#endif
    audio_stream = chn_param->audio_stream;
#if 1
    //printf("before set codec par\n");
    codec_context = audio_stream->codec;
    //AV_CODEC_ID_PCM_ALAW

    //codec_context->bits_per_coded_sample = AV_SAMPLE_FMT_S16;

    codec_context->codec_id = AV_CODEC_ID_PCM_ALAW;
    codec_context->codec_type = AVMEDIA_TYPE_AUDIO;

    codec_context->sample_rate = chn_param->audio_sample_rate;
    codec_context->channels = 1;
    //codec_context->sample_fmt = AV_SAMPLE_FMT_S16;
    codec_context->bits_per_coded_sample = 8;
    //codec_context->bit_rate = 64000;
    codec_context->frame_size = 320;
    codec_context->time_base.den = 25;
    codec_context->time_base.num = 1;

    /* time base: this is the fundamental unit of time (in seconds) in terms
       timebase should be 1/framerate and timestamp increments should be
       identically 1. */
    audio_stream->time_base.num = 1;
    audio_stream->time_base.den = 25;
    // some formats want stream headers to be separate
    if(chn_param->out_context->oformat->flags & AVFMT_GLOBALHEADER){
        codec_context->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }
    //printf("after set codec par\n");
#endif
    return 0;
}

static int report_file_event(int chn, int file_event)
{
    if(global_param->ripe_func){
        //callback new file
        struct ripe_stream_s event;
        event.event = file_event;

        struct container_param_s param;
        get_container_param(chn, &param);

        event.data = &param;
        event.callback_opaque = global_param->ripe_data;
        global_param->ripe_func(&event);
    }

    return 0;
}


static int create_new_file(struct chn_param_s *chn_param)
{
    int ret;


    char file_time[MAX_FILE_NAME_LEN] = {0};
    char file_pre_dir[MAX_FILE_NAME_LEN] = {0};
    time_t get_time = time(NULL);
    //strftime(file_time, MAX_FILE_NAME_LEN, "%Y-%m-%d-%H-%M-%S", gmtime(&get_time));
    struct tm disperse_time;
    localtime_r(&get_time, &disperse_time);

    //is dir exist , if not mkdir it
    sprintf(file_pre_dir, "%s/%04dyear/%02dmonth/%02dday", chn_param->file_dir, disperse_time.tm_year + 1900, disperse_time.tm_mon + 1, disperse_time.tm_mday);
    ret = access(file_pre_dir, F_OK);
    if(ret != 0){
        char shell_cmd[200] = {0};
        sprintf(shell_cmd, "mkdir -p %s", file_pre_dir);
        ret = system(shell_cmd);
        if(ret != 0){
            err_msg("mkdir failed\n");
            return -1;
        }
    }
    strftime(file_time, MAX_FILE_NAME_LEN, "%Y-%m-%d-%H-%M-%S", &disperse_time);
    memset(chn_param->file_name, 0, sizeof(chn_param->file_name));
    sprintf(chn_param->file_name, "%s/%s-%d.mkv",  file_pre_dir, file_time, chn_param->chn_num);
    //sprintf(chn_param->file_name, "%s/%s-%d.mp4",  file_pre_dir, file_time, chn_param->chn_num);
    info_msg("new file %s",chn_param->file_name);

    ret = avformat_alloc_output_context2(&chn_param->out_context, NULL, NULL, chn_param->file_name);
    if(ret < 0){
        err_msg("avformat_alloc_output_context2 failed\n");
        return -1;
    }

    ret =add_video_stream(chn_param);
    if(ret != 0){
        err_msg("add_video_stream failed\n");
        return -1;
    }

    if(chn_param->audio_enable == true){
        ret =add_audio_stream(chn_param);
        if(ret != 0){
            err_msg("add_video_stream failed\n");
            return -1;
        }
    }

    //av_dump_format(chn_param->out_context, 0, chn_param->file_name, 1);
    if (!(chn_param->out_context->oformat->flags & AVFMT_NOFILE))
    {
        //info_msg( "before avio_open\n");
        ret = avio_open(&chn_param->out_context->pb, chn_param->file_name, AVIO_FLAG_WRITE);
        if (ret < 0)
        {
            char buf[1024];
            av_strerror(ret, buf, sizeof(buf));
            printf("could not open '%s': %d, err = %s\n", chn_param->file_name, ret, buf);
            return 1;
        }
    }

    ret = avformat_write_header(chn_param->out_context, NULL);
    if (ret < 0) {
        err_msg( "Fail to write the header of output ");
        return 0;
    }


#if REGISTER_INDEX
    ret = create_file_index(chn_param->chn_num, chn_param->file_name, chn_param->audio_enable);
    if(ret != 0){
        err_msg("create_file_index failed\n");
    }
    chn_param->register_size_bound = REGISTER_FILE_SIZE_BOUND;

#endif

    chn_param->first_packet_pts = -1;

    return 0;
}

static int write_tailer_to_file(struct chn_param_s *chn_param)
{
    int ret;
    ret =av_write_trailer(chn_param->out_context);
    if(ret != 0){
        err_msg("av_write_trailer failed\n");
        return -1;
    }

    ret = avio_close(chn_param->out_context->pb);
    if(ret != 0){
        err_msg("avio_close failed\n");
    }

    avformat_free_context(chn_param->out_context);

#if REGISTER_INDEX
    ret = register_file_index_finished(chn_param->chn_num, chn_param->file_name);
    if(ret != 0){
        err_msg("register_file_index_finished failed\n");
        return -1;
    }
#endif
    return 0;
}

static inline int judge_write_tailer_clean_flag(struct chn_param_s *chn_param)
{
#if 1
    if(chn_param->out_context){
        write_tailer_to_file(chn_param);

        chn_param->first_packet_pts = -1;
        chn_param->out_context = NULL;
        chn_param->audio_stream = NULL;
        chn_param->video_stream = NULL;
        report_file_event(chn_param->chn_num, CONTAINER_CLOSE_FILE);
        return 0;
    }
#endif
    return 1;
}


int switch_new_file(int chn, struct container_param_s *param)
{
    struct chn_param_s *chn_param = global_param->chn_params + chn;
    int ret;

    pthread_mutex_lock(&chn_param->mutex);

    judge_write_tailer_clean_flag(chn_param);

    //param_copy(chn_param, param);

    ret = create_new_file(chn_param);
    if(ret != 0){
        err_msg("create_new_file failed \n");
        ret = -1;
    }else{
        report_file_event(chn, CONTAINER_CREATE_FILE);
    }


    pthread_mutex_unlock(&chn_param->mutex);
    return 0;
}

int switch_new_file_async(int chn, struct container_param_s *param)
{
    struct chn_param_s *chn_param = global_param->chn_params + chn;

    pthread_mutex_lock(&chn_param->mutex);
    chn_param->should_switch_new_file_async = 1;
    pthread_mutex_unlock(&chn_param->mutex);
}

int stop_container(int chn)
{
    struct chn_param_s *chn_param = global_param->chn_params + chn;

    pthread_mutex_lock(&chn_param->mutex);

    judge_write_tailer_clean_flag(chn_param);

    pthread_mutex_unlock(&chn_param->mutex);
    return 0;
}

static int write_video_packet_to_file(struct chn_param_s *chn_param, Mal_StreamBlock *block, int64_t *pts)
{
    //printf("%s\n", __FUNCTION__);
    char *packet_buff = NULL;
    int packet_size = 0;
    AVPacket pkt;
    AVRational hi_pts_avrational;
    int ret;

    packet_buff = (char *)block->p_buffer;
    packet_size = block->i_buffer;

    hi_pts_avrational.den = 1000000;
    hi_pts_avrational.num = 1;

    av_init_packet(&pkt);

    pkt.pts = av_rescale_q(block->i_pts - chn_param->first_packet_pts, hi_pts_avrational, chn_param->video_stream->time_base);

    *pts = pkt.pts;


    pkt.dts = pkt.pts;
    pkt.pts = pkt.dts;
    pkt.data = (unsigned char *)packet_buff;
    pkt.size = packet_size;
    pkt.stream_index= chn_param->video_stream->index;
    if(block->i_flags == BLOCK_H264E_NALU_ISLICE ){
        pkt.flags = AV_PKT_FLAG_KEY;
    }

    if(pkt.pts >= chn_param->register_size_bound){
#if REGISTER_INDEX
        register_file_size(chn_param->chn_num, chn_param->file_name);
#endif
        chn_param->register_size_bound += REGISTER_FILE_SIZE_BOUND;
    }

    ret = av_interleaved_write_frame(chn_param->out_context, &pkt);
    if(ret != 0){

        char err_buf[1024] = {};
        av_strerror(ret, err_buf, 1024);
        err_msg("av_interleaved_write_frame error video,chn_num = %d, %s, pts = %lld, dts = %lld, i_pts = %lld\n",chn_param->chn_num, err_buf,  pkt.pts, pkt.dts, block->i_pts);
        report_file_event(chn_param->chn_num, WRITE_INTERLEAVE_FAILED);

        return -1;
    }
    //info_msg("info pkt.pts = %l64d\n", pkt.pts);
    return 0;
}

static int write_audio_packet_to_file(struct chn_param_s *chn_param, Mal_StreamBlock *block)
{
    AVPacket pkt;
    AVRational hi_pts_avrational;
    int ret;

    hi_pts_avrational.den = 1000000;
    hi_pts_avrational.num = 1;

    av_init_packet(&pkt);

    if(chn_param->first_packet_pts == 0){
        chn_param->first_packet_pts = block->i_pts;
        pkt.pts = 0;
    }else{
        pkt.pts= av_rescale_q(block->i_pts - chn_param->first_packet_pts, hi_pts_avrational, chn_param->audio_stream->time_base);
    }

    pkt.dts= pkt.pts;
    if(pkt.size <= 4){
        return -1;
    }
    pkt.data = block->p_buffer;
    pkt.size = block->i_buffer;
    pkt.stream_index= chn_param->audio_stream->index;

    //ret = av_write_frame(mkv_info->out_context, &pkt);
    ret = av_interleaved_write_frame(chn_param->out_context, &pkt);
    if(ret != 0){
        printf("av_interleaved_write_frame error, ret = %d\n", ret);
        return -1;
    }

    return 0;
}

int container_start_new_file(char *file_name, int chn)
{
    int ret;
    struct chn_param_s *chn_param = global_param->chn_params + chn;
    //1.create new file or not
    pthread_mutex_lock(&chn_param->mutex);
    //info_msg("----------------------------------create_new_file----------------------\n");
    ret = create_new_file(chn_param);
    //info_msg("after create_new_file\n");
    if(ret != 0){
        err_msg("create_new_file failed \n");
        ret = -1;
        goto unlock_label;
    }
    report_file_event(chn, CONTAINER_CREATE_FILE);

unlock_label:
    pthread_mutex_unlock(&chn_param->mutex);
    return 0;
}


int get_raw_stream_v(int chn, Mal_StreamBlock *block, __attribute__((unused)) void* opaque)
{
    struct chn_param_s *chn_param = global_param->chn_params + chn;

    pthread_mutex_lock(&chn_param->mutex);
    if(!chn_param->out_context || !chn_param->video_stream){
        pthread_mutex_unlock(&chn_param->mutex);
        return 0;
    }
    //3.write data to file
    if(chn_param->first_packet_pts == -1){
        chn_param->first_packet_pts = block->i_pts;
    }
    int64_t pts = 0;
    write_video_packet_to_file(chn_param, block, &pts);

    pthread_mutex_unlock(&chn_param->mutex);
#if 0
#if REGISTER_INDEX
    if(block->i_flags == BLOCK_H264E_NALU_ISLICE ){
        ret = register_file_piece(chn_param->chn_num, chn_param->file_name);
        if(ret != 0){
            err_msg("register_file_piece error\n");
        }
    }
#endif
#endif

    return 0;
}



int container_send_video( Mal_StreamBlock *block)
{
    get_raw_stream_v(block->chn_num, block, (void *)block->chn_num);
    return 0;
}

int container_send_two_frame_video( Mal_StreamBlock *block, int is_next_is_key)
{
    int chn = block->chn_num;
    //printf("container_send_two_frame_video\n");

    if(is_next_is_key){
        struct chn_param_s *chn_param = global_param->chn_params + chn;

        pthread_mutex_lock(&chn_param->mutex);
        if(chn_param->should_switch_new_file_async){
            chn_param->should_switch_new_file_async = 0;
            pthread_mutex_unlock(&chn_param->mutex);
            get_raw_stream_v(block->chn_num, block, NULL);
            switch_new_file(chn, 0);
        }else{
            pthread_mutex_unlock(&chn_param->mutex);
            get_raw_stream_v(block->chn_num, block, NULL);
        }

    }else{
        get_raw_stream_v(block->chn_num, block, NULL);
    }
    return 0;
}

int container_send_audio( Mal_StreamBlock *block)
{
    get_raw_stream_a(block->chn_num, block, block->chn_num);
    return 0;
}

int get_raw_stream_a(__attribute__((unused)) int chn, Mal_StreamBlock *block, void* opaque)
{
    int video_chn = (int )opaque;

    struct chn_param_s *chn_param = global_param->chn_params + video_chn;

    pthread_mutex_lock(&chn_param->mutex);
    if(!chn_param->out_context || !chn_param->audio_stream){
        pthread_mutex_unlock(&chn_param->mutex);
        return 0;
    }
    write_audio_packet_to_file(chn_param, block);
    pthread_mutex_unlock(&chn_param->mutex);
    return 0;
}


int container_destory()
{
    int i;
    for(i = 0; i < 8; i++){
        pthread_mutex_destroy(&global_param->chn_params[i].mutex);
    }

    free(global_param->chn_params);
    free(global_param);
    return 0;
}

int set_container_codec_extradata(int chn, char *extradata, int size)
{
    if(chn < 0 || chn >=8){
        err_msg("chn num is wrong");
        return -1;
    }
#if 0
    struct chn_param_s *chn_param = global_param->chn_params + chn;

    pthread_mutex_lock(&chn_param->mutex);
    if(!chn_param->out_context || !chn_param->video_stream){
        pthread_mutex_unlock(&chn_param->mutex);
        return 0;
    }
#endif
    //calculate chn_param position
    struct chn_param_s *chn_param = global_param->chn_params + chn;
    chn_param->video_codec_extradata = malloc(size);
    memcpy(chn_param->video_codec_extradata, extradata, size);

    chn_param->video_codec_extradata_size = size;
}
