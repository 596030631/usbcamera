#include <fcntl.h>

#define LOG_TAG "AvcEncoder"

#include <cstdio>
#include <cstdlib>
#include <utilbase.h>
#include "AvcEncoder.h"
#include "sys/select.h"

const int ERROR = -1;
const int READY = 0;
const int STOP = 1;
const int RECORD = 2;
const int MUXER_START = 3;

int AvcEncoder::status = READY;

int frame_rate = 25;

AvcEncoder::AvcEncoder() {
//    pthread_mutex_init(&media_mutex, nullptr);
    fpsTime = 1000 / frame_rate;

    yuv420_buf = new unsigned char[3 * 640 * 480 / 2 * sizeof(unsigned char)];

}

AvcEncoder::~AvcEncoder() {
//    pthread_mutex_destroy(&media_mutex);
    delete (yuv420_buf);
    yuv420_buf = nullptr;

}

int64_t system_nano_time() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000000000LL + now.tv_nsec;
}

static void sleep_ms(unsigned int secs) {
    struct timeval tval{};
    tval.tv_sec = secs / 1000;
    tval.tv_usec = (secs * 1000) % 1000000;
    select(0, nullptr, nullptr, nullptr, &tval);
}

bool AvcEncoder::prepare_start(AvcArgs arguments) {
//    pthread_mutex_lock(&media_mutex);
    if (AvcEncoder::status != READY) {
        __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "编码器未释放，请稍后重试");
        return false;
    }
    AMediaFormat *videoFormat = AMediaFormat_new();
    AMediaFormat_setString(videoFormat, AMEDIAFORMAT_KEY_MIME, VIDEO_MIME);
    AMediaFormat_setInt32(videoFormat, AMEDIAFORMAT_KEY_WIDTH, 640);
    AMediaFormat_setInt32(videoFormat, AMEDIAFORMAT_KEY_HEIGHT, 480);
    AMediaFormat_setInt32(videoFormat, AMEDIAFORMAT_KEY_BIT_RATE, 1024);
    AMediaFormat_setInt32(videoFormat, AMEDIAFORMAT_KEY_FRAME_RATE, frame_rate);
    AMediaFormat_setInt32(videoFormat, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    AMediaFormat_setInt32(videoFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT, 19);
    uint8_t sps[2] = {0x12, 0x12};
    uint8_t pps[2] = {0x12, 0x12};
    AMediaFormat_setBuffer(videoFormat, "csd-0", sps, 2); // sps
    AMediaFormat_setBuffer(videoFormat, "csd-1", pps, 2); // pps
    videoCodec = AMediaCodec_createEncoderByType(VIDEO_MIME);


    media_status_t videoConfigureStatus = AMediaCodec_configure(videoCodec, videoFormat, nullptr,
                                                                nullptr,
                                                                AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    if (AMEDIA_OK != videoConfigureStatus) {
        __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "Media Config Failed!");
//        pthread_mutex_unlock(&media_mutex);
        status = ERROR;
        return false;
    }
    AMediaFormat_delete(videoFormat);


    int fd = open(arguments.path_name, O_CREAT | O_RDWR, 0666);
    if (!fd) {
//        pthread_mutex_unlock(&media_mutex);
        return false;
    }
    muxer = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    AMediaMuxer_setOrientationHint(muxer, 180);
    nanoTime = system_nano_time();
    media_status_t videoStatus = AMediaCodec_start(videoCodec);
    if (AMEDIA_OK != videoStatus) {
//        pthread_mutex_unlock(&media_mutex);
        return false;
    }
    AvcEncoder::status = RECORD;

    pthread_create(&videoThread, nullptr, videoStep, this);
//    pthread_mutex_unlock(&media_mutex);
    return true;
}

void AvcEncoder::feedData(void *data) {
    if (AvcEncoder::status >= RECORD) {
        frame_queue.push(data);
    }
}

void AvcEncoder::stop() {
    __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "设定停止状态");
    AvcEncoder::status = STOP;
}


int AvcEncoder::yuyvToYuv420P(const unsigned char *in, unsigned char *out, unsigned int width,
                              unsigned int height) {
    if (in == nullptr || out == nullptr) {
        __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "数据空指针");
        return -1;
    }
    unsigned char *y = out;
    unsigned char *u = out + width * height;
    unsigned char *v = out + width * height + width * height / 4;
    unsigned int i, j;
    unsigned int base_h;
    unsigned int is_u = 1;
    unsigned int y_index = 0, u_index = 0, v_index = 0;
    unsigned long yuv422_length = 2 * width * height;
    //序列为YU YV YU YV，一个yuv422帧的长度 width * height * 2 个字节
    //丢弃偶数行 u v
    for (i = 0; i < yuv422_length; i += 2) {
        *(y + y_index) = *(in + i);
        y_index++;
    }
    for (i = 0; i < height; i += 2) {
        base_h = i * width * 2;
        for (j = base_h + 1; j < base_h + width * 2; j += 2) {
            if (is_u) {
                *(u + u_index) = *(in + j);
                u_index++;
                is_u = 0;
            } else {
                *(v + v_index) = *(in + j);
                v_index++;
                is_u = 1;
            }
        }
    }
    return 1;
}


void *AvcEncoder::videoStep(void *obj) {
    auto *avc = (AvcEncoder *) obj;
    if (AvcEncoder::status != RECORD) return nullptr;
    __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "videoStep start");
//    pthread_mutex_lock(&AvcEncoder::getInstance().media_mutex);
    while (AvcEncoder::status >= RECORD) {
        if (avc->videoCodec == nullptr) {
            __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "videoCodec not ready");
            sleep_ms(50);
        }
        if (avc->frame_queue.empty()) {
            sleep_ms(50);
            continue;
        }
        void *data = *avc->frame_queue.wait_and_pop();
        if (data == nullptr) {
            __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "bye");

            continue;
        }
        int ret = avc->yuyvToYuv420P((const unsigned char *) data,
                                     avc->yuv420_buf, 640,
                                     480);
        if (ret < 0) {
            __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "EEEEEEEEEEEEEEEEEEEE");
            sleep_ms(50);
            continue;
        }

        ssize_t index = AMediaCodec_dequeueInputBuffer(avc->videoCodec,10000);
        size_t out_size;
        if (index >= 0) {
            uint8_t *buffer = AMediaCodec_getInputBuffer(avc->videoCodec,
                                                         index, &out_size);
            if (avc->yuv420_buf != nullptr && out_size > 0) {
                memcpy(buffer, avc->yuv420_buf, out_size);

                AMediaCodec_queueInputBuffer(avc->videoCodec, index, 0,
                                             out_size,
                                             (system_nano_time() -
                                              avc->nanoTime) / 1000,
                                             AvcEncoder::status != STOP ? 0
                                                                        : AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            }
        }
        auto *info = (AMediaCodecBufferInfo *) malloc(
                sizeof(AMediaCodecBufferInfo));
        ssize_t outIndex;
        do {
            outIndex = AMediaCodec_dequeueOutputBuffer(avc->videoCodec, info,
                                                       0);
            size_t outSize;
            if (outIndex >= 0) {
                uint8_t *outputBuffer = AMediaCodec_getOutputBuffer(
                        avc->videoCodec, outIndex,
                        &outSize);
                if (AvcEncoder::status >= RECORD && info->size > 0 &&
                    info->presentationTimeUs > 0) {
                    AMediaMuxer_writeSampleData(avc->muxer,
                                                0,
                                                outputBuffer,
                                                info);
                }
                AMediaCodec_releaseOutputBuffer(avc->videoCodec, outIndex,
                                                false);
            } else if (outIndex == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat *outFormat = AMediaCodec_getOutputFormat(
                        avc->videoCodec);
                AMediaMuxer_addTrack(avc->muxer, outFormat);
                const char *s = AMediaFormat_toString(outFormat);
                __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "%s%s", "视频格式：", s);
                if (AvcEncoder::status >= RECORD) {
                    AMediaMuxer_start(avc->muxer);
                    AvcEncoder::status = MUXER_START;
                }
            }
        } while (outIndex >= 0);
    }

    if (avc->muxer != nullptr) {
        if (AvcEncoder::status == MUXER_START) {
            AMediaMuxer_stop(avc->muxer);
            AMediaMuxer_delete(avc->muxer);
        }
        avc->muxer = nullptr;
    }
    if (avc->videoCodec != nullptr) {
        AMediaCodec_stop(avc->videoCodec);
        AMediaCodec_delete(avc->videoCodec);
        avc->videoCodec = nullptr;
    }

    AvcEncoder::status = READY;
    __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "编码线程结束");
    return nullptr;
}

