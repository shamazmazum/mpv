/*
 * NAS audio output driver
 *
 * copyright (c) 2019 Vasily Postnicov <shamaz.mazum@gmail.com>
 *
 * Based on the ao_nas.c for MPlayer. Author of the original code is
 * Tobias Diedrich.
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Theory of operation:
 *
 * The NAS consists of two parts, a server daemon and a client.
 * We setup the server to use a buffer of size bytes_per_second
 * with a low watermark of buffer_size - NAS_FRAG_SIZE.
 * Upon starting the flow the server will generate a buffer underrun
 * event and the event handler will fill the buffer for the first time.
 * Now the server will generate a lowwater event when the server buffer
 * falls below the low watermark value. The event handler gets called
 * again and refills the buffer by the number of bytes requested by the
 * server (usually a multiple of 4096). To prevent stuttering on
 * startup (start of playing, seeks, unpausing) the client buffer should
 * be bigger than the server buffer. (For debugging we also do some
 * accounting of what we think how much of the server buffer is filled)
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>

#include "config.h"
#include "options/options.h"
#include "common/common.h"
#include "common/msg.h"
#include "osdep/timer.h"
#include "osdep/endian.h"

#include <audio/audiolib.h>

#include "audio/format.h"

#include "ao.h"
#include "internal.h"

#if !HAVE_GPL
#error GPL only
#endif

/* NAS_FRAG_SIZE must be a power-of-two value */
#define NAS_FRAG_SIZE 4096

static const char * const nas_event_types[] = {
    "Undefined",
    "Undefined",
    "ElementNotify",
    "GrabNotify",
    "MonitorNotify",
    "BucketNotify",
    "DeviceNotify"
};

static const char * const nas_elementnotify_kinds[] = {
    "LowWater",
    "HighWater",
    "State",
    "Unknown"
};

static const char * const nas_states[] = {
    "Stop",
    "Start",
    "Pause",
    "Any"
};

static const char * const nas_reasons[] = {
    "User",
    "Underrun",
    "Overrun",
    "EOF",
    "Watermark",
    "Hardware",
    "Any"
};

static const char* nas_reason(unsigned int reason)
{
    if (reason > 6) reason = 6;
    return nas_reasons[reason];
}

static const char* nas_elementnotify_kind(unsigned int kind)
{
    if (kind > 2) kind = 3;
    return nas_elementnotify_kinds[kind];
}

static const char* nas_event_type(unsigned int type) {
    if (type > 6) type = 0;
    return nas_event_types[type];
}

static const char* nas_state(unsigned int state) {
    if (state>3) state = 3;
    return nas_states[state];
}

struct priv {
    AuServer    *aud;
    AuFlowID    flow;
    AuDeviceID  dev;
    AuFixedPoint    gain;
    int outburst;
    int buffersize;

    unsigned int state;
    int expect_underrun;

    char *client_buffer;
    char *server_buffer;
    unsigned int client_buffer_size;
    unsigned int client_buffer_used;
    unsigned int server_buffer_size;
    unsigned int server_buffer_used;
    pthread_mutex_t buffer_mutex;

    pthread_t event_thread;
    int stop_thread;
};

static void nas_print_error(struct ao *ao, const char *prefix, AuStatus as)
{
    struct priv *p = ao->priv;
    AuServer *aud = p->aud;
    char s[100];
    AuGetErrorText(aud, as, s, 100);
    MP_ERR(ao, "%s: returned status %d (%s)\n", prefix, as, s);
}

static int nas_readBuffer(struct ao *ao, unsigned int num)
{
    struct priv *nas_data = ao->priv;
    AuStatus as;

    pthread_mutex_lock(&nas_data->buffer_mutex);
    MP_DBG(ao, "nas_readBuffer(): num=%d client=%d/%d server=%d/%d\n",
            num,
            nas_data->client_buffer_used, nas_data->client_buffer_size,
            nas_data->server_buffer_used, nas_data->server_buffer_size);

    if (nas_data->client_buffer_used == 0) {
        MP_DBG(ao, "buffer is empty, nothing read.\n");
        pthread_mutex_unlock(&nas_data->buffer_mutex);
        return 0;
    }
    if (num > nas_data->client_buffer_used)
        num = nas_data->client_buffer_used;

    /*
     * It is not appropriate to call AuWriteElement() here because the
     * buffer is locked and delays writing to the network will cause
     * other threads to block waiting for buffer_mutex.  Instead the
     * data is copied to "server_buffer" and written to the network
     * outside of the locked section of code.
     *
     * (Note: Rather than these two buffers, a single circular buffer
     *  could eliminate the memcpy/memmove steps.)
     */
    /* make sure we don't overflow the buffer */
    if (num > nas_data->server_buffer_size)
        num = nas_data->server_buffer_size;
    memcpy(nas_data->server_buffer, nas_data->client_buffer, num);

    nas_data->client_buffer_used -= num;
    nas_data->server_buffer_used += num;
    memmove(nas_data->client_buffer, nas_data->client_buffer + num, nas_data->client_buffer_used);
    pthread_mutex_unlock(&nas_data->buffer_mutex);

    /*
     * Now write the new buffer to the network.
     */
    AuWriteElement(nas_data->aud, nas_data->flow, 0, num, nas_data->server_buffer, AuFalse, &as);
    if (as != AuSuccess)
        nas_print_error(ao, "nas_readBuffer(): AuWriteElement", as);

    return num;
}

static int nas_writeBuffer(struct ao *ao, void *data, unsigned int len)
{
    struct priv *nas_data = ao->priv;
    pthread_mutex_lock(&nas_data->buffer_mutex);
    MP_DBG(ao, "nas_writeBuffer(): len=%d client=%d/%d server=%d/%d\n",
           len, nas_data->client_buffer_used, nas_data->client_buffer_size,
           nas_data->server_buffer_used, nas_data->server_buffer_size);

    /* make sure we don't overflow the buffer */
    if (len > nas_data->client_buffer_size - nas_data->client_buffer_used)
        len = nas_data->client_buffer_size - nas_data->client_buffer_used;
    memcpy(nas_data->client_buffer + nas_data->client_buffer_used, data, len);
    nas_data->client_buffer_used += len;

    pthread_mutex_unlock(&nas_data->buffer_mutex);

    return len;
}

static int nas_empty_event_queue(struct priv *nas_data)
{
    AuEvent ev;
    int result = 0;

    while (AuScanForTypedEvent(nas_data->aud, AuEventsQueuedAfterFlush,
                   AuTrue, AuEventTypeElementNotify, &ev)) {
        AuDispatchEvent(nas_data->aud, &ev);
        result = 1;
    }
    return result;
}

static void *nas_event_thread_start(void *data)
{
    struct ao *ao = data;
    struct priv *nas_data = ao->priv;

    do {
        MP_DBG(ao, "event thread heartbeat (state=%s)\n",
               nas_state(nas_data->state));
        nas_empty_event_queue(nas_data);
        usleep(1000);
    } while (!nas_data->stop_thread);

    return NULL;
}

static AuBool nas_error_handler(AuServer* aud, AuErrorEvent* ev)
{
#if 0
    char s[100];
    AuGetErrorText(aud, ev->error_code, s, 100);
    MP_ERR(ao, "ao_nas: error [%s]\n"
        "error_code: %d\n"
        "request_code: %d\n"
        "minor_code: %d\n",
        s,
        ev->error_code,
        ev->request_code,
        ev->minor_code);
#endif
    return AuTrue;
}

static AuBool nas_event_handler(AuServer *aud, AuEvent *ev, AuEventHandlerRec *hnd)
{
    AuElementNotifyEvent *event = (AuElementNotifyEvent *) ev;
    struct ao *ao = hnd->data;
    struct priv *nas_data = ao->priv;

    MP_DBG(ao, "event_handler(): type %s kind %s state %s->%s reason %s numbytes %d expect_underrun %d\n",
        nas_event_type(event->type),
        nas_elementnotify_kind(event->kind),
        nas_state(event->prev_state),
        nas_state(event->cur_state),
        nas_reason(event->reason),
        (int)event->num_bytes,
        nas_data->expect_underrun);

    if (event->num_bytes > INT_MAX) {
        MP_ERR(ao, "num_bytes > 2GB, server buggy?\n");
    }

    if (event->num_bytes > nas_data->server_buffer_used)
        event->num_bytes = nas_data->server_buffer_used;
    nas_data->server_buffer_used -= event->num_bytes;

    switch (event->reason) {
    case AuReasonWatermark:
        nas_readBuffer(ao, event->num_bytes);
        break;
    case AuReasonUnderrun:
        // buffer underrun -> refill buffer
        nas_data->server_buffer_used = 0;
        if (nas_data->expect_underrun) {
            nas_data->expect_underrun = 0;
        } else {
            static int hint = 1;
            MP_WARN(ao, "Buffer underrun.\n");
            if (hint) {
                hint = 0;
                MP_WARN(ao,
                       "Possible reasons are:\n"
                       "1) Network congestion.\n"
                       "2) Your NAS server is too slow.\n"
                       "Try renicing your nasd to e.g. -15.\n");
            }
        }
        if (nas_readBuffer(ao,
                           nas_data->server_buffer_size -
                           nas_data->server_buffer_used) != 0) {
            event->cur_state = AuStateStart;
            break;
        }
        MP_WARN(ao, "Can't refill buffer, stopping flow.\n");
        AuStopFlow(aud, nas_data->flow, NULL);
        break;
    default:
        break;
    }
    nas_data->state=event->cur_state;
    return AuTrue;
}

static AuDeviceID nas_find_device(AuServer *aud, int nch)
{
    int i;
    for (i = 0; i < AuServerNumDevices(aud); i++) {
        AuDeviceAttributes *dev = AuServerDevice(aud, i);
        if ((AuDeviceKind(dev) == AuComponentKindPhysicalOutput) &&
             AuDeviceNumTracks(dev) == nch) {
            return AuDeviceIdentifier(dev);
        }
    }
    return AuNone;
}

static unsigned int nas_aformat_to_auformat(unsigned int *format)
{
    switch (*format) {
    case    AF_FORMAT_U8:
        return AuFormatLinearUnsigned8;
    case    AF_FORMAT_S16:
        return AuFormatLinearSigned16LSB;
    default:
        *format=AF_FORMAT_S16;
        return nas_aformat_to_auformat(format);
    }
}

// to set/get/query special features/parameters
static int control(struct ao *ao, enum aocontrol cmd, void *arg)
{
    AuElementParameters aep;
    AuStatus as;
    int retval = CONTROL_UNKNOWN;

    float *vol = arg;
    struct priv *nas_data = ao->priv;

    switch (cmd) {
    case AOCONTROL_GET_VOLUME:

        *vol = (float)nas_data->gain/AU_FIXED_POINT_SCALE*50;

        MP_DBG(ao, "AOCONTROL_GET_VOLUME: %.2f\n", *vol);
        retval = CONTROL_OK;
        break;

    case AOCONTROL_SET_VOLUME:
        nas_data->gain = AU_FIXED_POINT_SCALE*(*vol)/50;
        MP_DBG(ao, "AOCONTROL_SET_VOLUME: %.2f\n", *vol);

        aep.parameters[AuParmsMultiplyConstantConstant]=nas_data->gain;
        aep.flow = nas_data->flow;
        aep.element_num = 1;
        aep.num_parameters = AuParmsMultiplyConstant;

        AuSetElementParameters(nas_data->aud, 1, &aep, &as);
        if (as != AuSuccess) {
            nas_print_error(ao, "control(): AuSetElementParameters", as);
            retval = CONTROL_ERROR;
        } else retval = CONTROL_OK;
        break;
    };

    return retval;
}

// open & setup audio device
// return: 0=success -1=fail
static int init(struct ao *ao)
{
    struct priv *nas_data = ao->priv;
    int rate = ao->samplerate;
    int channels = ao->channels.num;
    int format = ao->format;

    AuElement elms[3];
    AuStatus as;
    unsigned char auformat = nas_aformat_to_auformat(&format);
    int bytes_per_sample = channels * AuSizeofFormat(auformat);
    int buffer_size;
    char *server;

    if (channels > 2) {
        MP_ERR (ao, "%i channels is unsupported with NAS. Try --audio-channels=stereo\n", channels);
        return -1;
    }

    MP_INFO(ao, "ao2: %d Hz  %d chans %s\n", rate, channels, af_fmt_to_str (format));

    buffer_size = rate * bytes_per_sample; /* buffer 1 second */
    nas_data->outburst = NAS_FRAG_SIZE;
    nas_data->outburst -= nas_data->outburst % bytes_per_sample;

    buffer_size /= 3;
    buffer_size -= buffer_size % nas_data->outburst;
    nas_data->buffersize = buffer_size*3;

    nas_data->client_buffer_size = buffer_size*2;
    nas_data->client_buffer = malloc(nas_data->client_buffer_size);
    nas_data->server_buffer_size = buffer_size;
    nas_data->server_buffer = malloc(nas_data->server_buffer_size);

    ao->format = format;
    ao->device_buffer = nas_data->client_buffer_size / bytes_per_sample;

    if (!bytes_per_sample) {
        MP_ERR(ao, "init(): Zero bytes per sample -> nosound\n");
        return -1;
    }

    if (!(server = getenv("AUDIOSERVER")) &&
        !(server = getenv("DISPLAY"))) {
        MP_ERR(ao, "init(): AUDIOSERVER environment variable not set -> nosound\n");
        return -1;
    }

    MP_VERBOSE(ao, "ao_nas: init(): Using audioserver %s\n", server);

    nas_data->aud = AuOpenServer(server, 0, NULL, 0, NULL, NULL);
    if (!nas_data->aud) {
        MP_ERR(ao, "init(): Can't open nas audio server -> nosound\n");
        return -1;
    }

    nas_data->dev = nas_find_device(nas_data->aud, channels);
    if (nas_data->dev == AuNone) {
        MP_ERR(ao, "Cannot find device with %i channels\n", channels);
        AuCloseServer(nas_data->aud);
        nas_data->aud = 0;
        return -1;
    }

    nas_data->flow = AuCreateFlow(nas_data->aud, NULL);
    if (nas_data->flow == 0) {
        MP_ERR(ao, "init(): Can't find a suitable output device -> nosound\n");
        AuCloseServer(nas_data->aud);
        nas_data->aud = 0;
        return -1;
    }

    AuMakeElementImportClient(elms, rate, auformat, channels, AuTrue,
                buffer_size / bytes_per_sample,
                (buffer_size - nas_data->outburst) /
                bytes_per_sample, 0, NULL);
    nas_data->gain = AuFixedPointFromFraction(1, 1);
    AuMakeElementMultiplyConstant(elms+1, 0, nas_data->gain);
    AuMakeElementExportDevice(elms+2, 1, nas_data->dev, rate,
                AuUnlimitedSamples, 0, NULL);
    AuSetElements(nas_data->aud, nas_data->flow, AuTrue, sizeof(elms)/sizeof(*elms), elms, &as);
    if (as != AuSuccess) {
        nas_print_error(ao, "init(): AuSetElements", as);
        AuCloseServer(nas_data->aud);
        nas_data->aud = 0;
        return -1;
    }
    AuRegisterEventHandler(nas_data->aud, AuEventHandlerIDMask |
                AuEventHandlerTypeMask,
                AuEventTypeElementNotify, nas_data->flow,
                nas_event_handler, (AuPointer) ao);
    AuSetErrorHandler(nas_data->aud, nas_error_handler);
    nas_data->state=AuStateStop;
    nas_data->expect_underrun=0;

    pthread_mutex_init(&nas_data->buffer_mutex, NULL);
    pthread_create(&nas_data->event_thread, NULL, &nas_event_thread_start, ao);

    return 0;
}

// close audio device
static void uninit(struct ao *ao){
    struct priv *nas_data = ao->priv;
    MP_DBG(ao, "uninit()\n");

    nas_data->expect_underrun = 1;
    while (nas_data->state != AuStateStop) usleep(1000);
    nas_data->stop_thread = 1;
    pthread_join(nas_data->event_thread, NULL);
    AuCloseServer(nas_data->aud);
    nas_data->aud = 0;
    free(nas_data->client_buffer);
    free(nas_data->server_buffer);
}

static void get_state(struct ao *ao, struct mp_pcm_state *state)
{
    struct priv *nas_data = ao->priv;

    pthread_mutex_lock(&nas_data->buffer_mutex);
    state->free_samples = (nas_data->client_buffer_size - nas_data->client_buffer_used) / ao->sstride;
    state->free_samples = state->free_samples / nas_data->outburst * nas_data->outburst;
    state->delay = ((double)(nas_data->client_buffer_used +
                             nas_data->server_buffer_used)) / (double)nas_data->buffersize;
    state->queued_samples = ao->device_buffer - state->free_samples;
    state->playing = nas_data->state == AuStateStart || nas_data->state == AuStatePause;
    pthread_mutex_unlock(&nas_data->buffer_mutex);
}

static bool set_pause(struct ao *ao, bool paused)
{
    AuStatus as;
    struct priv *nas_data = ao->priv;
    bool res = true;

    if (paused) {
        AuStopFlow(nas_data->aud, nas_data->flow, &as);
    } else {
        AuStartFlow(nas_data->aud, nas_data->flow, &as);
        if (as != AuSuccess) {
            nas_print_error(ao, "play(): AuStartFlow", as);
            res = false;
        }
    }

    return res;
}

// stop playing and empty buffers (for seeking/pause)
static void reset(struct ao *ao){
    AuStatus as;
    struct priv *nas_data = ao->priv;

    MP_DBG(ao, "reset()\n");

    pthread_mutex_lock(&nas_data->buffer_mutex);
    nas_data->client_buffer_used = 0;
    pthread_mutex_unlock(&nas_data->buffer_mutex);
    while (nas_data->state != AuStateStop) {
        AuStopFlow(nas_data->aud, nas_data->flow, &as);
        if (as != AuSuccess)
            nas_print_error(ao, "reset(): AuStopFlow", as);
        usleep(1000);
    }
}

static void audio_start(struct ao *ao)
{
    struct priv *nas_data = ao->priv;
    AuStatus as;

    if (nas_data->state != AuStateStart) {
        MP_DBG(ao, "play(): Starting flow.\n");
        nas_data->expect_underrun = 1;
        AuStartFlow(nas_data->aud, nas_data->flow, &as);
        if (as != AuSuccess) {
            nas_print_error(ao, "play(): AuStartFlow", as);
        }
    }
}

static int audio_write(struct ao *ao, void **data, int samples)
{
    struct priv *nas_data = ao->priv;
    int len = samples * ao->sstride;
    int written, maxbursts = 0, playbursts = 0;
    AuStatus as;

    MP_DBG(ao, "play(%p, samples=%d, len=%d)\n",
           data, samples, len);

    if (len == 0)
        return true;

    pthread_mutex_lock(&nas_data->buffer_mutex);
    maxbursts = (nas_data->client_buffer_size -
                 nas_data->client_buffer_used) / nas_data->outburst;
    playbursts = len / nas_data->outburst;
    len = (playbursts > maxbursts ? maxbursts : playbursts) *
        nas_data->outburst;
    pthread_mutex_unlock(&nas_data->buffer_mutex);
    written = nas_writeBuffer(ao, data[0], len);

    return (written > 0)? true: false;
}

const struct ao_driver audio_out_nas = {
    .description = "Network Audio Server audio output",
    .name = "nas",
    .init = init,
    .uninit = uninit,
    .control = control,
    .get_state = get_state,
    .set_pause = set_pause,
    .write = audio_write,
    .start = audio_start,
    .reset = reset,
    .priv_size = sizeof(struct priv)
};
