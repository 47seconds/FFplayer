#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>
#include <stdio.h>

#define NUMBER_OF_FRAMES 5

void display (AVFormatContext *pFormatCtx, AVPacket packet, int videoStream, AVCodecContext *pCodecCtx, AVFrame *pFrame, struct SwsContext *swsCtx, AVFrame* pFrameYUV, SDL_Texture *texture, SDL_Renderer *renderer) {
    while(av_read_frame(pFormatCtx, &packet) >= 0) {
        if (packet.stream_index == videoStream) {
            if (avcodec_send_packet(pCodecCtx, &packet) < 0) break;

            while (!avcodec_receive_frame(pCodecCtx, pFrame)) {
                // Convert to YUV format
                sws_scale(swsCtx, (uint8_t const * const *) pFrame->data, pFrame->linesize, 0, pCodecCtx->height, pFrameYUV->data, pFrameYUV->linesize);

                // Update texture with YUV frame data
                SDL_UpdateYUVTexture(
                    texture,
                    NULL,
                    pFrameYUV->data[0], pFrameYUV->linesize[0],
                    pFrameYUV->data[1], pFrameYUV->linesize[1],
                    pFrameYUV->data[2], pFrameYUV->linesize[2]
                );

                // Clear and render texture
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, NULL, NULL);
                SDL_RenderPresent(renderer);
            }
        }

        av_packet_unref(&packet);
        SDL_PollEvent(NULL);
    }
}

int findVideoStream (AVFormatContext *pFormatCtx) {
    int streamInd = -1;

    for (int i = 0; i < pFormatCtx->nb_streams; i++) {
        if(pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) streamInd = i;
        if (streamInd != -1) break;
    }

    return streamInd;
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
    AVCodecParameters *pCodecCtxparam = NULL;
    AVCodecContext *pCodecCtx = NULL;

    // Find the first video stream
    int videoStream = findVideoStream(pFormatCtx);
    if (videoStream == -1) return -1; // Didn't find a video stream

    pCodecCtxparam = pFormatCtx->streams[videoStream]->codecpar;
    // The stream's information about the codec is in what we call the "codec context." This contains all the information about the codec that the stream is using, and now we have a pointer to it. But we still have to find the actual codec and open it
    // Find the decoder for the video stream
    const AVCodec *pCodec = avcodec_find_decoder(pCodecCtxparam->codec_id);

    if (pCodec == NULL) {
        fprintf(stderr, "Unsupported Codec!\n");
        return -1; // Codec not found
    }

    // Copy context
    pCodecCtx = avcodec_alloc_context3(pCodec);
    if (!pCodecCtx) {
        fprintf(stderr, "Couldn't allocate codec context\n");
        return -1; // Allocation error
    }
    
    if (avcodec_parameters_to_context(pCodecCtx, pCodecCtxparam) < 0) {
        fprintf(stderr, "Couldn't copy codec context\n");
        return -1; // Error copying codec context
    }

    // Open codec
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) return -1; // Failed to open codec
    // Note that we must not use the AVCodecContext from the video stream directly! So we have to use avcodec_copy_context() to copy the context to a new location (after allocating memory for it, of course)
    
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
    display(pFormatCtx, packet, videoStream, pCodecCtx, pFrame, swsCtx, pFrameYUV, texture, renderer);

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

// Complile using: gcc -o L2Display L2Display.c -lavcodec -lavformat -lavutil -lswscale -lSDL2 -lm
// Run program with: ./L2Display <videoPath.mp4/mpg/...>