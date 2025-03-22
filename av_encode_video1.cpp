#include <iostream>
#include <fstream>
#include <string>
#include <ffmpeg/avcodec.h>
#include <ffmpeg/avformat.h>
#include <ffmpeg/swscale.h>

int main(int argc, char* argv[]) {
    if (argc!= 4) {
        std::cout << "Usage: " << argv[0] << " input_file output_file bitrate" << std::endl;
        return -1;
    }

    const std::string input_file = argv[1];
    const std::string output_file = argv[2];
    int bitrate = std::stoi(argv[3]);

    // Initialize FFmpeg libraries
    av_register_all();

    // Open input file
    AVFormatContext* input_format_context = nullptr;
    if (avformat_open_input(&input_format_context, input_file.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "Failed to open input file: " << input_file << std::endl;
        return -1;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(input_format_context, nullptr) < 0) {
        std::cerr << "Failed to find stream information" << std::endl;
        return -1;
    }

    // Find video stream
    int video_stream_index = -1;
    for (int i = 0; i < input_format_context->nb_streams; ++i) {
        if (input_format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
    }
    if (video_stream_index == -1) {
        std::cerr << "Failed to find video stream" << std::endl;
        return -1;
    }

    // Get a pointer to the codec context for the video stream
    AVCodecParameters* video_codecpar = input_format_context->streams[video_stream_index]->codecpar;
    AVCodec* video_codec = avcodec_find_decoder(video_codecpar->codec_id);
    if (!video_codec) {
        std::cerr << "Failed to find video codec" << std::endl;
        return -1;
    }

    // Create a codec context for the encoder
    AVCodecContext* video_encoder_ctx = avcodec_alloc_context3(video_codec);
    if (!video_encoder_ctx) {
        std::cerr << "Failed to allocate video encoder context" << std::endl;
        return -1;
    }

    // Copy the codec context from the input to the encoder
    if (avcodec_parameters_to_context(video_encoder_ctx, video_codecpar) < 0) {
        std::cerr << "Failed to copy codec parameters" << std::endl;
        return -1;
    }

    // Set the bitrate
    video_encoder_ctx->bit_rate = bitrate * 1000;

    // Open the encoder
    if (avcodec_open2(video_encoder_ctx, video_codec, nullptr) < 0) {
        std::cerr << "Failed to open video encoder" << std::endl;
        return -1;
    }

    // Create a new output context
    AVFormatContext* output_format_context = nullptr;
    if (avformat_alloc_output_context2(&output_format_context, nullptr, "3gpp", output_file.c_str()) < 0) {
        std::cerr << "Failed to create output context" << std::endl;
        return -1;
    }

    // Find the video stream id for the output
    int video_stream_id = av_find_best_stream(output_format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_id == -1) {
        std::cerr << "Failed to find video stream id" << std::endl;
        return -1;
    }

    // Create an encoder output packet
    AVPacket* encoder_packet = av_packet_alloc(sizeof(AVPacket));
    if (!encoder_packet) {
        std::cerr << "Failed to allocate encoder packet" << std::endl;
        return -1;
    }

    // Create a SW scaler for resolution conversion
    AVFrame* input_frame = av_frame_alloc();
    if (!input_frame) {
        std::cerr << "Failed to allocate input frame" << std::endl;
        return -1;
    }

    SwsContext* sws_context = sws_getContext(
        video_codecpar->width, video_codecpar->height,
        video_encoder_ctx->width, video_encoder_ctx->height,
        AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!sws_context) {
        std::cerr << "Failed to create SW scaler" << std::endl;
        return -1;
    }

    AVFrame* output_frame = av_frame_alloc();
    if (!output_frame) {
        std::cerr << "Failed to allocate output frame" << std::endl;
        return -1;
    }

    // Open the output file
    AVIOContext* output_io_context = nullptr;
    if (output_format_context->oformat->flags & AVFMT_NOFILE) {
        output_io_context = avio_alloc_context(av_malloc(4096), 4096, 1, nullptr, nullptr, [](uint8_t* ptr) {}, nullptr);
        output_format_context->pb = output_io_context;
    } else {
        if (avio_open(&output_format_context->pb, output_file.c_str(), AVIO_FLAG_WRITE) < 0) {
            std::cerr << "Failed to open output file: " << output_file << std::endl;
            return -1;
        }
    }

    // Write the output format header
    if (avformat_write_header(output_format_context) < 0) {
        std::cerr << "Failed to write output header" << std::endl;
        return -1;
    }

    // Main encoding loop
    while (av_read_frame(input_format_context, encoder_packet) >= 0) {
        if (encoder_packet->stream_index == video_stream_index) {
            if (avcodec_send_frame(video_encoder_ctx, input_frame) < 0) {
                std::cerr << "Error sending a frame for encoding" << std::endl;
                break;
            }

            while (avcodec_receive_packet(video_encoder_ctx, encoder_packet) == 0) {
                av_packet_rescale_ts(encoder_packet, input_format_context->streams[video_stream_index], output_format_context->streams[video_stream_id]);
                av_interleaved_write_frame(output_format_context, encoder_packet);
            }
        }

        av_packet_unref(encoder_packet);
    }

    // Flush the encoder
    while (avcodec_receive_packet(video_encoder_ctx, encoder_packet) == 0) {
        av_packet_rescale_ts(encoder_packet, input_format_context->streams[video_stream_index], output_format_context->streams[video_stream_id]);
        av_interleaved_write_frame(output_format_context, encoder_packet);
    }

    // Write the trailer
    av_write_trailer(output_format_context);

    // Close the output file
    if (output_format_context->oformat->flags & AVFMT_NOFILE) {
        avio_context_free(&output_format_context->pb);
    } else {
        avio_closep(&output_format_context->pb);
    }

    // Free the resources
    av_frame_free(&output_frame);
    sws_freeContext(sws_context);
    av_frame_free(&input_frame);
    av_packet_free(&encoder_packet);
    avcodec_free_context(&video_encoder_ctx);
    avcodec_parameters_destroy(video_codecpar);

    // Close the input file
    avformat_close_input(&input_format_context);

    std::cout << "Conversion completed successfully!" << std::endl;

    return 0;
}
