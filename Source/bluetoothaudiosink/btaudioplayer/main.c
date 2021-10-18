/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2021 Metrological
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <alloca.h>
#include <pthread.h>
#include <signal.h>

#include <bluetoothaudiosink.h>


#define CONNECTOR "/tmp/btaudiobuffer"
#define CDDA_FRAMERATE (75 /* fps */)

#define TRACE(format, ...) fprintf(stderr, "btaudioplayer: " format "\n", ##__VA_ARGS__)


typedef struct {
    const char* file;
    bool playing;
    bool session_open;
    bluetoothaudiosink_format_t format;
    pthread_t thread;
} context_t;

typedef struct {
    char riff[4];
    uint32_t riff_size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t type;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t pad1;
    uint16_t pad2;
    uint16_t resolution;
    char data[4];
    uint32_t data_size;
} __attribute__((packed)) wav_header_t;

static volatile bool user_break = false;


static void* playback_task(void *user_data)
{
    context_t *context = (context_t*)user_data;

    TRACE("File streaming thread started");

    if (bluetoothaudiosink_speed(100) == 0) {
        FILE *f = fopen(context->file, "rb+");
        if (f != NULL) {

            const uint16_t bufferSize = ((context->format.channels * context->format.resolution * (context->format.sample_rate / context->format.frame_rate)) / 8);
            uint8_t *data = alloca(bufferSize);
            uint16_t to_play = 0;

            TRACE("Opened file '%s', read buffer size %i bytes", context->file, bufferSize);

            fseek(f, sizeof(wav_header_t), SEEK_SET);

            while (context->playing == true) {
                uint16_t played = 0;

                if (to_play == 0) {
                    to_play = fread(data, 1, bufferSize, f);
                }

                if (bluetoothaudiosink_frame(to_play, data, &played) != 0) {
                    TRACE("Failed to send audio frame!");
                    break;
                }

                if (to_play != bufferSize) {
                    TRACE("EOF reached");
                    break;
                }

                if (played < to_play) {
                    usleep(1 * 1000);
                }

                to_play -= played;
            }

            fclose(f);
            TRACE("Closed file '%s'", context->file);
        } else {
            TRACE("Failed to open file '%s'", context->file);
        }

        if (bluetoothaudiosink_speed(0) != 0) {
            TRACE("Failed to set audio speed 0%!");
        }
    } else {
        TRACE("Failed to set audio speed 100%!");
    }

    TRACE("File streaming thread terminated!");

    return (NULL);
}

static void audio_sink_connected(context_t * context)
{
    if (context->session_open == false) {
        if (bluetoothaudiosink_acquire(CONNECTOR, &context->format, 8) != 0) {
            TRACE("Failed to open Bluetooth audio sink device!");
        } else {
            TRACE("Successfully opened Bluetooth audio sink device");
        }
    } else {
        context->session_open = false;
    }
}

static void audio_sink_ready(context_t *context)
{
    if (context->session_open == false) {
        TRACE("Starting a playback session...");
        context->playing = true;
        context->session_open = true;
        pthread_create(&context->thread, NULL, playback_task, (void*)context);
    } else {
        TRACE("Tearing down the playback session...");
        context->playing = false;
        pthread_join(context->thread, NULL);
        bluetoothaudiosink_relinquish();
    }
}

static void audio_sink_disconnected(context_t *context)
{
    if (context->session_open == true) {
        // Device disconnected abruptly, clean up!
        context->playing = false;
        pthread_join(context->thread, NULL);
        context->session_open = false;
    }
}

static void audio_sink_state_update(void *user_data)
{
    bluetoothaudiosink_state_t state = BLUETOOTHAUDIOSINK_STATE_UNKNOWN;

    if (bluetoothaudiosink_state(&state) == 0) {
        switch (state) {
        case BLUETOOTHAUDIOSINK_STATE_UNASSIGNED:
            TRACE("Bluetooth audio sink is currently unassigned!");
            break;
        case BLUETOOTHAUDIOSINK_STATE_CONNECTED:
            TRACE("Bluetooth audio sink now available!");
            audio_sink_connected((context_t*)user_data);
            break;
        case BLUETOOTHAUDIOSINK_STATE_CONNECTED_BAD_DEVICE:
            TRACE("Invalid device connected - cant't play");
            break;
        case BLUETOOTHAUDIOSINK_STATE_CONNECTED_RESTRICTED:
            TRACE("Restricted Bluetooth audio device connected - won't play");
            break;
        case BLUETOOTHAUDIOSINK_STATE_DISCONNECTED:
            TRACE("Bluetooth Audio sink is now disconnected!");
            audio_sink_disconnected((context_t*)user_data);
            break;
        case BLUETOOTHAUDIOSINK_STATE_READY:
            TRACE("Bluetooth Audio sink now ready!");
            audio_sink_ready((context_t*)user_data);
            break;
        case BLUETOOTHAUDIOSINK_STATE_STREAMING:
            TRACE("Bluetooth Audio sink is now streaming!");
            break;
        default:
            break;
        }
    }
}

static void audio_sink_operational_state_update(const bool running, void *user_data)
{
    if (running == true) {
        bluetoothaudiosink_state_t state = BLUETOOTHAUDIOSINK_STATE_UNKNOWN;
        bluetoothaudiosink_state(&state);

        if (state == BLUETOOTHAUDIOSINK_STATE_UNKNOWN) {
            TRACE("Unknown Bluetooth Audio Sink failure!");
        } else {
            TRACE("Bluetooth Audio Sink service now available");
            if (bluetoothaudiosink_register_state_update_callback(audio_sink_state_update, user_data) != 0) {
                TRACE("Failed to register sink sink update callback!");
            }
        }
    } else {
        TRACE("Bluetooth Audio Sink service is now unvailable");
    }
}

void ctrl_c_handler(int signal)
{
    (void) signal;
    user_break = true;
}


int main(int argc, const char* argv[])
{
    int result = 1;

    printf("Plays a .wav file over a Bluetooth speaker device\n");

    if (argc == 2) {
        FILE *f;
        context_t context;
        memset(&context, 0, sizeof(context));

        context.file = argv[1];
        context.playing = false;
        context.session_open = false;

        f = fopen(context.file, "rb+");
        if (f != NULL) {
            wav_header_t header;

            if (fread(&header, 1, sizeof(header), f) == sizeof(header)) {
                context.format.sample_rate = header.sample_rate;
                context.format.frame_rate = CDDA_FRAMERATE;
                context.format.channels = header.channels;
                context.format.resolution = header.resolution;

                TRACE("Input format: PCM %i Hz, %i bit (signed, little endian), %i channels @ %i Hz",
                    context.format.sample_rate, context.format.resolution, context.format.channels, context.format.frame_rate);
            }

            fclose(f);
            f = NULL;

            if (bluetoothaudiosink_register_operational_state_update_callback(&audio_sink_operational_state_update, &context) != 0) {
                TRACE("Failed to register Bluetooths Audio Sink operational callback");
            } else {
                const uint32_t timeout = 120 /* sec */;
                uint32_t time = timeout;
                unsigned int second = 1000 * 1000;

                // Poor man's synchronisation.....
                TRACE("Waiting for Bluetooth audio sink device to connect...");

                while ((context.playing != true) && (time != 0)) {
                    usleep(second);
                    time--;
                }

                if (context.playing == true) {
                    TRACE("Playing...");

                    struct sigaction handler;
                    handler.sa_handler = ctrl_c_handler;
                    sigemptyset(&handler.sa_mask);
                    handler.sa_flags = 0;
                    sigaction(SIGINT, &handler, NULL);

                    while (context.playing != false) {
                        uint32_t playtime = 0;
                        bluetoothaudiosink_time(&playtime);
                        fprintf(stderr, "Time: %02i:%02i:%03i\r", ((playtime / 1000) / 60), ((playtime / 1000) % 60), playtime % 1000);

                        usleep(second / 10);

                        if ((user_break != false) && (context.playing == true)) {
                            TRACE("User break! Stopping playback...");
                            context.playing = false;
                        }
                    }

                    printf("\n");

                    while (context.session_open != false) {
                        usleep(second);
                    }

                    usleep(second);

                    result = 0;
                } else {
                    TRACE("Bluetooth audio sink device not connected in %i seconds, terminating!", timeout);
                }

                bluetoothaudiosink_dispose();
            }
        } else {
            TRACE("Failed to open the source file!");
        }
    } else {
        TRACE("arguments:\n%s <file.wav>", argv[0]);
    }

    return (result);
}
