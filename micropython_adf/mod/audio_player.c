/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2019 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <stdio.h>
#include <string.h>

#include "py/objstr.h"
#include "py/runtime.h"

#include "esp_audio.h"

#include "amr_decoder.h"
#include "mp3_decoder.h"
#include "wav_decoder.h"

#include "http_stream.h"
#include "vfs_stream.h"
#include "i2s_stream.h"

#include "board_init.h"

typedef struct _audio_player_obj_t {
    mp_obj_base_t base;
    mp_obj_t callback;

    esp_audio_handle_t player;
    mp_obj_dict_t *state;
} audio_player_obj_t;

static const qstr player_info_fields[] = {
    MP_QSTR_input, MP_QSTR_codec
};

static const MP_DEFINE_STR_OBJ(player_info_input_obj, "http|file stream");
static const MP_DEFINE_STR_OBJ(player_info_codec_obj, "mp3|amr");

static MP_DEFINE_ATTRTUPLE(
    player_info_obj,
    player_info_fields,
    2,
    (mp_obj_t)&player_info_input_obj,
    (mp_obj_t)&player_info_codec_obj);

extern const mp_obj_type_t audio_player_type;

static mp_obj_t player_info(void)
{
    return (mp_obj_t)&player_info_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_0(audio_player_info_obj, player_info);

static void audio_state_cb(esp_audio_state_t *state, void *ctx)
{
    audio_player_obj_t *self = (audio_player_obj_t *)ctx;

    if (self->callback != mp_const_none) {
        mp_obj_dict_t *dict = self->state;

        mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_status), MP_OBJ_TO_PTR(mp_obj_new_int(state->status)));
        mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_err_msg), MP_OBJ_TO_PTR(mp_obj_new_int(state->err_msg)));
        mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_media_src), MP_OBJ_TO_PTR(mp_obj_new_int(state->media_src)));

        mp_sched_schedule(self->callback, dict);
    }
}

static int _http_stream_event_handle(http_stream_event_msg_t *msg)
{
    if (msg->event_id == HTTP_STREAM_RESOLVE_ALL_TRACKS) {
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_FINISH_TRACK) {
        return http_stream_next_track(msg->el);
    }
    if (msg->event_id == HTTP_STREAM_FINISH_PLAYLIST) {
        return http_stream_restart(msg->el);
    }
    return ESP_OK;
}

static esp_audio_handle_t audio_player_create(void)
{
    // init player
    esp_audio_cfg_t cfg = DEFAULT_ESP_AUDIO_CONFIG();
    cfg.vol_handle = board_codec_init();
    cfg.vol_set = (audio_volume_set)audio_hal_set_volume;
    cfg.vol_get = (audio_volume_get)audio_hal_get_volume;
    cfg.resample_rate = 48000;
    cfg.prefer_type = ESP_AUDIO_PREFER_MEM;
    esp_audio_handle_t player = esp_audio_create(&cfg);

    // add input stream
    // fatfs stream
    vfs_stream_cfg_t fs_reader = VFS_STREAM_CFG_DEFAULT();
    fs_reader.type = AUDIO_STREAM_READER;
    fs_reader.task_core = 1;
    esp_audio_input_stream_add(player, vfs_stream_init(&fs_reader));
    // http stream
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.event_handle = _http_stream_event_handle;
    http_cfg.type = AUDIO_STREAM_READER;
    http_cfg.enable_playlist_parser = true;
    http_cfg.task_core = 1;
    audio_element_handle_t http_stream_reader = http_stream_init(&http_cfg);
    esp_audio_input_stream_add(player, http_stream_reader);

    // add decoder
    // mp3
    mp3_decoder_cfg_t mp3_dec_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_dec_cfg.task_core = 1;
    esp_audio_codec_lib_add(player, AUDIO_CODEC_TYPE_DECODER, mp3_decoder_init(&mp3_dec_cfg));
    // amr
    amr_decoder_cfg_t amr_dec_cfg = DEFAULT_AMR_DECODER_CONFIG();
    amr_dec_cfg.task_core = 1;
    esp_audio_codec_lib_add(player, AUDIO_CODEC_TYPE_DECODER, amr_decoder_init(&amr_dec_cfg));
    // wav
    wav_decoder_cfg_t wav_dec_cfg = DEFAULT_WAV_DECODER_CONFIG();
    wav_dec_cfg.task_core = 1;
    esp_audio_codec_lib_add(player, AUDIO_CODEC_TYPE_DECODER, wav_decoder_init(&wav_dec_cfg));

    // Create writers and add to esp_audio
    i2s_stream_cfg_t i2s_writer = I2S_STREAM_CFG_DEFAULT_WITH_PARA(I2S_NUM_0, 48000, I2S_DATA_BIT_WIDTH_16BIT, AUDIO_STREAM_WRITER);
    i2s_writer.task_core = 1;
    i2s_writer.uninstall_drv = false;
    esp_audio_output_stream_add(player, i2s_stream_init(&i2s_writer));

    return player;
}

static mp_obj_t audio_player_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    mp_arg_check_num(n_args, n_kw, 1, 1, false);

    static esp_audio_handle_t basic_player = NULL;

    audio_player_obj_t *self = mp_obj_malloc_with_finaliser(audio_player_obj_t, &audio_player_type);
    self->callback = args[0];
    if (basic_player == NULL) {
        basic_player = audio_player_create();
    }
    self->player = basic_player;
    self->state = mp_obj_new_dict(3);

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audio_player_play_helper(audio_player_obj_t *self, mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum {
        ARG_uri,
        ARG_pos,
        ARG_sync,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_uri, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_pos, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_sync, MP_ARG_BOOL, { .u_obj = mp_const_false } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_uri].u_obj != mp_const_none) {
        const char *uri = mp_obj_str_get_str(args[ARG_uri].u_obj);
        int pos = args[ARG_pos].u_int;

        esp_audio_state_t state = { 0 };
        esp_audio_state_get(self->player, &state);
        if (state.status == AUDIO_STATUS_RUNNING || state.status == AUDIO_STATUS_PAUSED) {
            esp_audio_stop(self->player, TERMINATION_TYPE_NOW);
            int wait = 20;
            esp_audio_state_get(self->player, &state);
            while (wait-- && (state.status == AUDIO_STATUS_RUNNING || state.status == AUDIO_STATUS_PAUSED)) {
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_audio_state_get(self->player, &state);
            }
        }
        esp_audio_callback_set(self->player, audio_state_cb, self);
        if (args[ARG_sync].u_obj == mp_const_false) {
            mp_obj_t dest[2];
            mp_load_method(self->state, MP_QSTR_clear, dest);
            mp_call_method_n_kw(0, 0, dest);

            mp_obj_dict_store(self->state, MP_ROM_QSTR(MP_QSTR_status),     MP_OBJ_TO_PTR(mp_obj_new_int(AUDIO_STATUS_UNKNOWN)));
            mp_obj_dict_store(self->state, MP_ROM_QSTR(MP_QSTR_err_msg),    MP_OBJ_TO_PTR(mp_obj_new_int(ESP_ERR_AUDIO_NO_ERROR)));
            mp_obj_dict_store(self->state, MP_ROM_QSTR(MP_QSTR_media_src),  MP_OBJ_TO_PTR(mp_obj_new_int(0)));

            return mp_obj_new_int(esp_audio_play(self->player, AUDIO_CODEC_TYPE_DECODER, uri, pos));
        } else {
            return mp_obj_new_int(esp_audio_sync_play(self->player, uri, pos));
        }
    } else {
        return mp_obj_new_int(ESP_ERR_AUDIO_INVALID_PARAMETER);
    }
}

static mp_obj_t audio_player_play(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    return audio_player_play_helper(args[0], n_args - 1, args + 1, kw_args);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audio_player_play_obj, 1, audio_player_play);

static mp_obj_t audio_player_stop_helper(audio_player_obj_t *self, mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum {
        ARG_termination,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_termination, MP_ARG_INT, { .u_int = TERMINATION_TYPE_NOW } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    return mp_obj_new_int(esp_audio_stop(self->player, args[ARG_termination].u_int));
}

static mp_obj_t audio_player_stop(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    return audio_player_stop_helper(args[0], n_args - 1, args + 1, kw_args);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audio_player_stop_obj, 1, audio_player_stop);

static mp_obj_t audio_player_pause(mp_obj_t self_in)
{
    audio_player_obj_t *self = self_in;
    return mp_obj_new_int(esp_audio_pause(self->player));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audio_player_pause_obj, audio_player_pause);

static mp_obj_t audio_player_resume(mp_obj_t self_in)
{
    audio_player_obj_t *self = self_in;
    return mp_obj_new_int(esp_audio_resume(self->player));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audio_player_resume_obj, audio_player_resume);

static mp_obj_t audio_player_vol_helper(audio_player_obj_t *self, size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum {
        ARG_vol,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_vol, MP_ARG_INT, { .u_int = 0xffff } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_vol].u_int == 0xffff) {
        int vol = 0;
        esp_audio_vol_get(self->player, &vol);
        return mp_obj_new_int(vol);
    } else {
        if (args[ARG_vol].u_int >= 0 && args[ARG_vol].u_int <= 100) {
            return mp_obj_new_int(esp_audio_vol_set(self->player, args[ARG_vol].u_int));
        } else {
            return mp_obj_new_int(ESP_ERR_AUDIO_INVALID_PARAMETER);
        }
    }
}

static mp_obj_t audio_player_vol(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    return audio_player_vol_helper(args[0], n_args - 1, args + 1, kw_args);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audio_player_vol_obj, 1, audio_player_vol);

static mp_obj_t audio_player_get_vol(mp_obj_t self_in)
{
    audio_player_obj_t *self = self_in;
    int vol = 0;
    esp_audio_vol_get(self->player, &vol);
    return mp_obj_new_int(vol);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audio_player_get_vol_obj, audio_player_get_vol);

static mp_obj_t audio_player_set_vol(mp_obj_t self_in, mp_obj_t vol)
{
    audio_player_obj_t *self = self_in;
    int volume = mp_obj_get_int(vol);
    return mp_obj_new_int(esp_audio_vol_set(self->player, volume));
}
static MP_DEFINE_CONST_FUN_OBJ_2(audio_player_set_vol_obj, audio_player_set_vol);

static mp_obj_t audio_player_state(mp_obj_t self_in)
{
    audio_player_obj_t *self = self_in;
    esp_audio_state_t state = { 0 };
    esp_audio_state_get(self->player, &state);
    mp_obj_dict_t *dict = self->state;

    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_status), MP_OBJ_TO_PTR(mp_obj_new_int(state.status)));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_err_msg), MP_OBJ_TO_PTR(mp_obj_new_int(state.err_msg)));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_media_src), MP_OBJ_TO_PTR(mp_obj_new_int(state.media_src)));

    return self->state;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audio_player_state_obj, audio_player_state);

static mp_obj_t audio_player_pos(mp_obj_t self_in)
{
    audio_player_obj_t *self = self_in;
    int pos = -1;
    int err = esp_audio_pos_get(self->player, &pos);
    if (err == ESP_ERR_AUDIO_NO_ERROR) {
        return mp_obj_new_int(pos);
    } else {
        return mp_const_none;
    }
}
static MP_DEFINE_CONST_FUN_OBJ_1(audio_player_pos_obj, audio_player_pos);

static mp_obj_t audio_player_time(mp_obj_t self_in)
{
    audio_player_obj_t *self = self_in;
    int time = 0;
    int err = esp_audio_time_get(self->player, &time);
    if (err == ESP_ERR_AUDIO_NO_ERROR) {
        return mp_obj_new_int(time);
    } else {
        return mp_const_none;
    }
}
static MP_DEFINE_CONST_FUN_OBJ_1(audio_player_time_obj, audio_player_time);

static const mp_rom_map_elem_t player_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&audio_player_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audio_player_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audio_player_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_pause), MP_ROM_PTR(&audio_player_pause_obj) },
    { MP_ROM_QSTR(MP_QSTR_resume), MP_ROM_PTR(&audio_player_resume_obj) },
    { MP_ROM_QSTR(MP_QSTR_vol), MP_ROM_PTR(&audio_player_vol_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_vol), MP_ROM_PTR(&audio_player_get_vol_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_vol), MP_ROM_PTR(&audio_player_set_vol_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_state), MP_ROM_PTR(&audio_player_state_obj) },
    { MP_ROM_QSTR(MP_QSTR_pos), MP_ROM_PTR(&audio_player_pos_obj) },
    { MP_ROM_QSTR(MP_QSTR_time), MP_ROM_PTR(&audio_player_time_obj) },

    // esp_audio_status_t
    { MP_ROM_QSTR(MP_QSTR_STATUS_UNKNOWN), MP_ROM_INT(AUDIO_STATUS_UNKNOWN) },
    { MP_ROM_QSTR(MP_QSTR_STATUS_RUNNING), MP_ROM_INT(AUDIO_STATUS_RUNNING) },
    { MP_ROM_QSTR(MP_QSTR_STATUS_PAUSED), MP_ROM_INT(AUDIO_STATUS_PAUSED) },
    { MP_ROM_QSTR(MP_QSTR_STATUS_STOPPED), MP_ROM_INT(AUDIO_STATUS_STOPPED) },
    { MP_ROM_QSTR(MP_QSTR_STATUS_FINISHED), MP_ROM_INT(AUDIO_STATUS_FINISHED) },
    { MP_ROM_QSTR(MP_QSTR_STATUS_ERROR), MP_ROM_INT(AUDIO_STATUS_ERROR) },

    // audio_termination_type
    { MP_ROM_QSTR(MP_QSTR_TERMINATION_NOW), MP_ROM_INT(TERMINATION_TYPE_NOW) },
    { MP_ROM_QSTR(MP_QSTR_TERMINATION_DONE), MP_ROM_INT(TERMINATION_TYPE_DONE) },
};

static MP_DEFINE_CONST_DICT(player_locals_dict, player_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audio_player_type,
    MP_QSTR_audio_player,
    MP_TYPE_FLAG_NONE,
    make_new, audio_player_make_new,
    locals_dict, &player_locals_dict
    );