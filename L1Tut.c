#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <stdio.h>

#define NUMBER_OF_FRAMES 5

void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame) {
    FILE *pFile;
    char szFilename[32];

    // open file
    sprintf(szFilename, "frame%d.ppm", iFrame);
    pFile = fopen(szFilename, "wb");
    if (pFile == NULL) return; // Failed opening/creating file

    // Write header
    fprintf(pFile, "P6\n%d %d\n255\n", width, height);

    // Write pixel data
    for (int i = 0; i < height; i++) fwrite(pFrame->data[0] + i * pFrame->linesize[0], 1, width * 3, pFile);

    // Close file
    fclose(pFile);
}

int main(int argc, char *argv[]) {

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
    int videoStream = -1;
    for(int i = 0; i < pFormatCtx->nb_streams; i++) {
        if(pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) videoStream = i;
        if (videoStream != -1) break;
    }

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
    
    if (avcodec_parameters_to_context(pCodecCtx, pCodecCtxparam)) {
        fprintf(stderr, "Couldn't copy codec context\n");
        return -1; // Error copying codec context
    }

    // Open codec
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) return -1; // Failed to open codec
    // Note that we must not use the AVCodecContext from the video stream directly! So we have to use avcodec_copy_context() to copy the context to a new location (after allocating memory for it, of course)

    // Now we need a place to actually store the frame
    AVFrame *pFrame = NULL;

    // allocate videa frame memory
    pFrame = av_frame_alloc();

    // we're planning to output PPM files, which are stored in 24-bit RGB, we're going to have to convert our frame from its native format to RGB.
    // For most projects, we're going to convert our initial frame to a specific format. Let's allocate a frame for the converted frame now
    AVFrame *pFrameRGB = NULL;
    pFrameRGB = av_frame_alloc();
    
    // Even though we've allocated the frame, we still need a place to put the raw data when we convert it. We use avpicture_get_size to get the size we need, and allocate the space manually
    uint8_t *buffer = NULL;
    int numBytes;
    // Determine required buffer size and allocate buffer
    numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height, 1);
    buffer = (uint8_t*) av_malloc(numBytes * sizeof(uint8_t));

    // Assign appropriate parts of buffer to image planes in pFrameRGB
    // Note that pFrameRGB is an AVFrame, but AVFrame is a superset of AVPicture
    av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, buffer, AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height, 1);

    // Finally! Now we're ready to read from the stream! 

    // Read through the entire video stream by reading in the packet, decoding it into our frame, and once our frame is complete, we will convert and save it
    struct SwsContext *swsCtx = NULL;
    int frameFinished;
    AVPacket packet;

    // initialize SWS context for software scaling
    swsCtx = sws_getContext(
        pCodecCtx->width,
        pCodecCtx->height,
        pCodecCtx->pix_fmt,
        pCodecCtx->width,
        pCodecCtx->height,
        AV_PIX_FMT_RGB24,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL
    );

    int i = 0;
    while(av_read_frame(pFormatCtx, &packet) >= 0) {
        // Is this a packet from the video stream?
        if (packet.stream_index == videoStream) {
            // Decode video frame by sending the packet to the decoder
            if (avcodec_send_packet(pCodecCtx, &packet) < 0) {
                fprintf(stderr, "Failed to send packed to decode\n");
                break;
            }

            // Receive the decoded frames
            while(1) {
                int ret = avcodec_receive_frame(pCodecCtx, pFrame);
                if (!ret) {
                    // Successfully received a frame
                    // Convert the image from its native format to RGB
                    sws_scale(swsCtx, (uint8_t const * const *) pFrame->data, pFrame->linesize, 0, pCodecCtx->height, pFrameRGB->data, pFrameRGB->linesize);

                    // Save the first NUMBER_OF_FRAMES frames to disk
                    if (i++ < NUMBER_OF_FRAMES) SaveFrame(pFrameRGB, pCodecCtx->width, pCodecCtx->height, i);
                } else if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    // Need more packets or reached end of stream
                    break;
                } else {
                    fprintf(stderr, "Error during decoding\n");
                    break;
                }
            }
        }

        // Free the packet that was allocated by av_read_frame
        av_packet_unref(&packet);
    }

    // Free the RGB image
    av_free(buffer);
    av_free(pFrameRGB);

    // Free the YUV frame
    av_free(pFrame);

    // Close the codecs
    avcodec_free_context(&pCodecCtx);

    // Close the video file
    avformat_close_input(&pFormatCtx);

    return 0;
}

// Complile using: gcc -o L1Tut L1Tut.c -lavutil -lavformat -lavcodec -lswscale -lz -lm
// Run program withL L1Tut <videoPath.mp4/mpg/...>