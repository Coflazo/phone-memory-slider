// Spike 6: how fast can we get a 256 px RGB proxy out of real phone formats?
//   JPEG  -> QImageReader scaled decode (libjpeg DCT scaling)
//   HEIC  -> libheif thumbnail item when present, else full primary decode
//   Video -> libav: 8 evenly spaced keyframes, file protocol only
#include <QCoreApplication>
#include <QImage>
#include <QImageReader>
#include <QDir>
#include <chrono>
#include <cstdio>
#include <string>
#include <libheif/heif.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

static double jpeg_proxy(const QString& path) {
    const auto t0 = Clock::now();
    QImageReader reader(path);
    const QSize full = reader.size();
    reader.setScaledSize(full.scaled(256, 256, Qt::KeepAspectRatioByExpanding));
    const QImage img = reader.read();
    return img.isNull() ? -1.0 : ms_since(t0);
}

static double heic_proxy(const std::string& path, bool use_thumbnail) {
    const auto t0 = Clock::now();
    heif_context* ctx = heif_context_alloc();
    if (heif_context_read_from_file(ctx, path.c_str(), nullptr).code != heif_error_Ok) { heif_context_free(ctx); return -1; }
    heif_image_handle* primary = nullptr;
    heif_context_get_primary_image_handle(ctx, &primary);
    heif_image_handle* source = primary;
    heif_image_handle* thumb = nullptr;
    if (use_thumbnail && heif_image_handle_get_number_of_thumbnails(primary) > 0) {
        heif_item_id id = 0;
        heif_image_handle_get_list_of_thumbnail_IDs(primary, &id, 1);
        heif_image_handle_get_thumbnail(primary, id, &thumb);
        source = thumb;
    }
    heif_image* img = nullptr;
    const heif_error err = heif_decode_image(source, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGB, nullptr);
    const double t = err.code == heif_error_Ok ? ms_since(t0) : -1.0;
    if (img) heif_image_release(img);
    if (thumb) heif_image_handle_release(thumb);
    heif_image_handle_release(primary);
    heif_context_free(ctx);
    return t;
}

static double video_keyframes(const std::string& path, int frames) {
    const auto t0 = Clock::now();
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "protocol_whitelist", "file", 0);  // never let a media file reach the network
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, &opts) < 0) { av_dict_free(&opts); return -1; }
    av_dict_free(&opts);
    avformat_find_stream_info(fmt, nullptr);
    const AVCodec* dec = nullptr;
    const int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (si < 0 || !dec) { avformat_close_input(&fmt); return -1; }
    AVStream* st = fmt->streams[si];
    AVCodecContext* cc = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(cc, st->codecpar);
    cc->thread_count = 0;
    avcodec_open2(cc, dec, nullptr);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    SwsContext* sws = nullptr;
    uint8_t* rgb = static_cast<uint8_t*>(av_malloc(256 * 256 * 3));
    const int64_t dur = st->duration > 0 ? st->duration : av_rescale_q(fmt->duration, AV_TIME_BASE_Q, st->time_base);
    int got = 0;
    for (int k = 0; k < frames; ++k) {
        av_seek_frame(fmt, si, dur * (2 * k + 1) / (2 * frames), AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(cc);
        bool done = false;
        while (!done && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == si && avcodec_send_packet(cc, pkt) >= 0 && avcodec_receive_frame(cc, frame) == 0) {
                sws = sws_getCachedContext(sws, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                           256, 256, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
                uint8_t* dst[1] = {rgb};
                const int stride[1] = {256 * 3};
                sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, stride);
                ++got;
                done = true;
            }
            av_packet_unref(pkt);
        }
    }
    av_free(rgb);
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);
    return got == frames ? ms_since(t0) : -1.0;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QDir dir(argc > 1 ? argv[1] : "media");
    for (const QString& f : dir.entryList({"*.jpg"}, QDir::Files, QDir::Name))
        std::printf("jpeg  %-18s %7.1f ms\n", qPrintable(f), jpeg_proxy(dir.filePath(f)));
    for (const QString& f : dir.entryList({"*.heic"}, QDir::Files, QDir::Name)) {
        const std::string p = dir.filePath(f).toStdString();
        std::printf("heic  %-18s thumb %7.1f ms   full %7.1f ms\n", qPrintable(f), heic_proxy(p, true), heic_proxy(p, false));
    }
    for (const QString& f : dir.entryList({"*.mp4", "*.mov"}, QDir::Files, QDir::Name))
        std::printf("video %-18s 8 keyframes %7.1f ms\n", qPrintable(f), video_keyframes(dir.filePath(f).toStdString(), 8));
    return 0;
}
