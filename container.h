#ifndef __CONTAINER__
#define __CONTAINER__



#if __cplusplus
extern "C"{
#endif


#include <stdbool.h>
#include <stdint.h>
#include "streamblock.h"
//
#define MAX_FILE_NAME_LEN 1024

int get_raw_stream_v(int chn, Mal_StreamBlock *block, void* opaque);
int get_raw_stream_a(int chn, Mal_StreamBlock *block, void* opaque);

int container_send_audio( Mal_StreamBlock *block);
int container_send_video( Mal_StreamBlock *block);

int container_send_two_frame_video( Mal_StreamBlock *block, int is_next_is_key);

int set_event_callback();


//send ripe mp4 data interface
enum CONTAINER_EVENT{
    CONTAINER_CREATE_FILE, //struct container_param_s
    CONTAINER_CLOSE_FILE, //struct container_param_s
    WRITE_INTERLEAVE_FAILED,
};

struct ripe_stream_s{
    int event;
    void *data;
    void *callback_opaque;
};

typedef void (*get_ripe_stream_fun)(struct ripe_stream_s *stream);



//get and set param of container
struct container_param_s{
    bool audio_enable;

    char file_path[MAX_FILE_NAME_LEN];
    int last_time;

    SIZE_S video_size;
    int video_frame_rate;

    int fps;

    int audio_sample_rate;
    int audio_bit_width;
    int audio_sound_mode;
    int audio_num_pre_frame;
};


int get_container_param(int chn, struct container_param_s *param);
int set_container_param(int chn, struct container_param_s *param);
//
int set_container_codec_extradata(int chn, char *extradata, int size);



int container_start_new_file(char *file_name, int chn);

//async
int switch_new_file(int chn, struct container_param_s *param);
int switch_new_file_async(int chn, struct container_param_s *param);
int stop_container(int chn);

//construct and destory
int container_init(get_ripe_stream_fun callback, void *ripe_data);
int container_destory();
#if __cplusplus
}
#endif
#endif

