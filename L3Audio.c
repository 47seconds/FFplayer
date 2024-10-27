#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>
#include <assert.h>
#include <stdio.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000
#define NUMBER_OF_FRAMES 5

typedef struct AudioState {
    SwrContext *swr_ctx;
    enum AVSampleFormat out_sample_fmt;
    int out_sample_rate;
    int out_channels;
} AudioState;

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

int quit = 0;
PacketQueue audioq;

void packet_queue_init(PacketQueue* q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacketList *pkt1;
    
    // Create a new packet list entry
    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1) return -1;
    
    // Clone the packet
    pkt1->pkt = *pkt;  // This creates a shallow copy
    if (av_packet_make_refcounted(&pkt1->pkt) < 0) {
        av_free(pkt1);
        return -1;
    }
    pkt1->next = NULL;
    
    SDL_LockMutex(q->mutex);
    
    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
        
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
    
    return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    while (1) {
        if (quit) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt) q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else SDL_CondWait(q->cond, q->mutex);
    }
    
    SDL_UnlockMutex(q->mutex);
    return ret;
}

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size) {
    static AudioState audio_state = {0};
    static AVFrame *frame = NULL;
    static AVPacket pkt;
    int data_size = 0;
    int ret;

    // Initialize audio conversion context if not done
    if (!audio_state.swr_ctx) {
        AVChannelLayout out_ch_layout = {0};
        
        // Set output format to SDL's preferred format (S16)
        audio_state.out_sample_fmt = AV_SAMPLE_FMT_S16;
        audio_state.out_sample_rate = aCodecCtx->sample_rate;
        audio_state.out_channels = aCodecCtx->ch_layout.nb_channels;

        // Initialize output channel layout
        av_channel_layout_default(&out_ch_layout, audio_state.out_channels);
        
        // Create resampling context
        SwrContext *swr_ctx = NULL;
        ret = swr_alloc_set_opts2(&swr_ctx,
            &out_ch_layout,                    // output channel layout
            audio_state.out_sample_fmt,        // output sample format
            audio_state.out_sample_rate,       // output sample rate
            &aCodecCtx->ch_layout,            // input channel layout
            aCodecCtx->sample_fmt,            // input sample format
            aCodecCtx->sample_rate,           // input sample rate
            0,                                // logging offset
            NULL);                           // log context

        if (ret < 0) {
            fprintf(stderr, "Could not allocate resampler context\n");
            av_channel_layout_uninit(&out_ch_layout);
            return -1;
        }

        ret = swr_init(swr_ctx);
        if (ret < 0) {
            fprintf(stderr, "Failed to initialize the resampling context\n");
            swr_free(&swr_ctx);
            av_channel_layout_uninit(&out_ch_layout);
            return -1;
        }

        audio_state.swr_ctx = swr_ctx;
        av_channel_layout_uninit(&out_ch_layout);
    }

    if (!frame) {
        frame = av_frame_alloc();
        if (!frame) return -1;
    }

    while (1) {
        ret = avcodec_receive_frame(aCodecCtx, frame);
        if (ret == 0) {
            // Calculate number of samples to convert
            int out_samples = av_rescale_rnd(
                swr_get_delay(audio_state.swr_ctx, frame->sample_rate) + frame->nb_samples,
                audio_state.out_sample_rate,
                frame->sample_rate,
                AV_ROUND_UP
            );

            uint8_t *converted_buffer = NULL;
            
            // Allocate buffer for converted samples
            ret = av_samples_alloc(&converted_buffer, NULL,
                audio_state.out_channels,
                out_samples,
                audio_state.out_sample_fmt,
                0
            );
            
            if (ret < 0) {
                fprintf(stderr, "Failed to allocate converted buffer\n");
                av_frame_unref(frame);
                return -1;
            }

            // Convert the samples
            int converted_samples = swr_convert(
                audio_state.swr_ctx,
                &converted_buffer,
                out_samples,
                (const uint8_t**)frame->data,
                frame->nb_samples
            );

            if (converted_samples < 0) {
                fprintf(stderr, "Failed to convert samples\n");
                av_freep(&converted_buffer);
                av_frame_unref(frame);
                return -1;
            }

            // Calculate size of converted data
            data_size = av_samples_get_buffer_size(
                NULL,
                audio_state.out_channels,
                converted_samples,
                audio_state.out_sample_fmt,
                1
            );

            if (data_size > buf_size) {
                fprintf(stderr, "Buffer too small for converted data\n");
                av_freep(&converted_buffer);
                av_frame_unref(frame);
                return -1;
            }

            // Copy converted data to output buffer
            memcpy(audio_buf, converted_buffer, data_size);
            av_freep(&converted_buffer);
            av_frame_unref(frame);
            return data_size;
        }

        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
            ret = packet_queue_get(&audioq, &pkt, 1);
            if (ret < 0) {
                return -1;
            }
            ret = avcodec_send_packet(aCodecCtx, &pkt);
            av_packet_unref(&pkt);
            if (ret < 0) {
                fprintf(stderr, "Error submitting packet: %s\n", av_err2str(ret));
                return -1;
            }
            continue;
        }
        return -1;
    }
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
    AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;
    int len1, audio_size;

    static uint8_t audio_buf[192000];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;

    // Clear output stream
    SDL_memset(stream, 0, len);

    while (len > 0) {
        if (audio_buf_index >= audio_buf_size) {
            // Get more data
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
            if (audio_size < 0) {
                // Error, output silence
                audio_buf_size = 1024;
                memset(audio_buf, 0, audio_buf_size);
            } else {
                audio_buf_size = audio_size;
            }
            audio_buf_index = 0;
        }
        
        len1 = audio_buf_size - audio_buf_index;
        if (len1 > len) len1 = len;
        
        // Mix audio instead of direct copy
        SDL_MixAudioFormat(stream, 
                          audio_buf + audio_buf_index,
                          AUDIO_S16SYS,
                          len1,
                          SDL_MIX_MAXVOLUME);
        
        len -= len1;
        stream += len1;
        audio_buf_index += len1;
    }
}

void display(AVFormatContext *pFormatCtx, AVPacket packet, int audioStream, int videoStream, 
            AVCodecContext *pCodecCtx, AVFrame *pFrame, struct SwsContext *swsCtx, 
            AVFrame* pFrameYUV, SDL_Texture *texture, SDL_Renderer *renderer, SDL_Event event) {
    int i = 0;
    
    while (av_read_frame(pFormatCtx, &packet) >= 0) {
        if (packet.stream_index == videoStream) {
            // Process video packet
            int ret = avcodec_send_packet(pCodecCtx, &packet);
            av_packet_unref(&packet);  // Unref after sending
            
            if (ret < 0) {
                fprintf(stderr, "Error sending packet: %s\n", av_err2str(ret));
                break;
            }
            
            while (ret >= 0) {
                ret = avcodec_receive_frame(pCodecCtx, pFrame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                if (ret < 0) {
                    fprintf(stderr, "Error receiving frame: %s\n", av_err2str(ret));
                    break;
                }
                
                // Convert and display frame
                sws_scale(swsCtx, (uint8_t const * const *)pFrame->data, 
                         pFrame->linesize, 0, pCodecCtx->height,
                         pFrameYUV->data, pFrameYUV->linesize);
                         
                SDL_UpdateYUVTexture(texture, NULL,
                    pFrameYUV->data[0], pFrameYUV->linesize[0],
                    pFrameYUV->data[1], pFrameYUV->linesize[1],
                    pFrameYUV->data[2], pFrameYUV->linesize[2]);
                    
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, NULL, NULL);
                SDL_RenderPresent(renderer);
                
                // Add a small delay to control framerate
                SDL_Delay(1);
            }
        }
        else if (packet.stream_index == audioStream) {
            packet_queue_put(&audioq, &packet);
            // packet is now owned by the queue
        }
        else {
            av_packet_unref(&packet);
        }
        
        SDL_PollEvent(&event);
        if (event.type == SDL_QUIT) {
            quit = 1;
            break;
        }
    }
}

int main (int argc, char *argv[]) {

    if(argc < 2) {
        fprintf(stderr, "Usage: ./L2Display <videoPath.mp4/mkv/mpg/...>\n");
        exit(1);
    }

    // SDL_Init() essentially tells the library what features we're going to use
    if(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }

    // This function reads the file header and stores information about the file format in the AVFormatContext structure we have given it. The last three arguments are used to specify the file format, buffer size, and format options, but by setting this to NULL or 0, libavformat will auto-detect these.
    AVFormatContext *pFormatCtx = NULL;
    if (avformat_open_input(&pFormatCtx, argv[1], NULL, 0)) return -1; // Failed to open file

    // This function only looks at the header, so next we need to check out the stream information in the file
    if(avformat_find_stream_info(pFormatCtx, NULL) < 0) return -1; // Couldn't find stream information

    // Dump information about file onto standard error for debugging purposes (pFormatCtx->stream has stream details)
    av_dump_format(pFormatCtx, 0, argv[1], 0);

    // Now pFormatCtx->streams is just an array of pointers, of size pFormatCtx->nb_streams, so let's walk through it until we find a video stream
    AVCodecParameters *aCodecCtxparam = NULL;
    AVCodecContext *aCodecCtx = NULL;
    AVCodecParameters *pCodecCtxparam = NULL;
    AVCodecContext *pCodecCtx = NULL;

    // Find the first audio and video stream
    int audioStream = -1;
    int videoStream = -1;
    for (int i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStream < 0) audioStream = i;
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStream < 0) videoStream = i;
        if (audioStream != -1 && videoStream != -1) break;
    }
    if (audioStream == -1) return -1; // Didn't find a audio stream
    if (videoStream == -1) return -1; // Didn't find a video stream

    aCodecCtxparam = pFormatCtx->streams[audioStream]->codecpar;
    pCodecCtxparam = pFormatCtx->streams[videoStream]->codecpar;
    // The stream's information about the codec is in what we call the "codec context." This contains all the information about the codec that the stream is using, and now we have a pointer to it. But we still have to find the actual codec and open it
    // Find the decoder for the video stream
    const AVCodec *aCodec = avcodec_find_decoder(aCodecCtxparam->codec_id);
    const AVCodec *pCodec = avcodec_find_decoder(pCodecCtxparam->codec_id);

    if (pCodec == NULL || aCodec == NULL) {
        fprintf(stderr, "Unsupported Codec!\n");
        return -1; // Codec not found
    }

    // Copy context
    aCodecCtx = avcodec_alloc_context3(aCodec);
    if (!aCodecCtx) {
        fprintf(stderr, "Couldn't allocate Audio codec context\n");
        return -1; // Allocation error
    }
    if (avcodec_parameters_to_context(aCodecCtx, aCodecCtxparam) < 0) {
        fprintf(stderr, "Couldn't copy Audio codec context\n");
        return -1; // Error copying codec context
    }

    pCodecCtx = avcodec_alloc_context3(pCodec);
    if (!pCodecCtx) {
        fprintf(stderr, "Couldn't allocate Video codec context\n");
        return -1; // Allocation error
    }

    if (avcodec_parameters_to_context(pCodecCtx, pCodecCtxparam) < 0) {
        fprintf(stderr, "Couldn't copy Video codec context\n");
        return -1; // Error copying codec context
    }

    SDL_AudioSpec wanted_spec, spec;
    wanted_spec.freq = aCodecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;  // Most compatible format
    wanted_spec.channels = aCodecCtx->ch_layout.nb_channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = 2048;  // Adjust this buffer size if needed
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = aCodecCtx;

    if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        return -1;
    }

    // Open codec
    if (avcodec_open2(aCodecCtx, aCodec, NULL) < 0) return -1; // Failed to open codec
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) return -1; // Failed to open codec
    // Note that we must not use the AVCodecContext from the video stream directly! So we have to use avcodec_copy_context() to copy the context to a new location (after allocating memory for it, of course)

    packet_queue_init(&audioq);
    SDL_PauseAudio(0);

    // We need a place on the screen to put stuff. The basic area for displaying images with SDL is called a surface
    // Initialize SDL window, renderer, and texture
    SDL_Window *window = SDL_CreateWindow("FFplayer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, pCodecCtx->width, pCodecCtx->height, SDL_WINDOW_SHOWN);
    SDL_Texture *texture = NULL;
    SDL_Renderer *renderer = NULL;

    renderer = SDL_CreateRenderer(window, -1, 0);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, pCodecCtx->width, pCodecCtx->height);

    // Set up scaling context for YUV conversion
    struct SwsContext *swsCtx = NULL;
    swsCtx = sws_getContext(
        pCodecCtx->width,
        pCodecCtx->height,
        pCodecCtx->pix_fmt,
        pCodecCtx->width,
        pCodecCtx->height,
        AV_PIX_FMT_YUV420P,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL
    );

    // Now we need a place to actually store the frame
    AVFrame *pFrame = NULL;

    // Allocate frames for original and converted formats
    pFrame = av_frame_alloc();
    AVFrame *pFrameYUV = av_frame_alloc();

    // Allocate buffer for YUV frame
    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 1);
    uint8_t *buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
    av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, buffer, AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 1);

    AVPacket packet;

    // Read frames and display them
    SDL_Event event;
    display(pFormatCtx, packet, audioStream, videoStream, pCodecCtx, pFrame, swsCtx, pFrameYUV, texture, renderer, event);

    // Free allocated resources
    av_free(buffer);
    av_frame_free(&pFrame);
    av_frame_free(&pFrameYUV);
    avcodec_free_context(&pCodecCtx);
    avformat_close_input(&pFormatCtx);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

// Complile using: gcc -o L3Audio L3Audio.c -lavcodec -lavformat -lavutil -lswresample -lswscale -lSDL2 -lm 
// Run program with: ./L3Audio <videoPath.mp4/mpg/...>