// srt_genclip: build synthetic clips for verification.
//
// Every frame carries:
//   - a large decimal frame number (unique frame id),
//   - a 10-bit binary barcode of the frame number (robust to lossy coding),
//   - non-uniform presentation timestamps (explicit VFR),
// Optional: an audio tone track; optional: a deliberately truncated/corrupt
// tail (file is cut after muxing so the last frames become undecodable).
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QStringList>
#include <cmath>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

struct GenOpts {
    QString out;
    int width = 640;
    int height = 360;
    int frames = 24;
    bool audio = false;
    bool corruptTail = false;
    // Non-uniform per-frame durations in us, cycled. Sum != frames * fixed.
    QList<int64_t> patternUs = {50000, 33333, 66667, 40000, 80000};
};

static void paintFrame(QImage& img, int frameNo)
{
    img.fill(Qt::white);
    QPainter p(&img);
    p.fillRect(0, 0, img.width(), 112, Qt::black);

    // Start guard bar (white), then 10 bits LSB-first, then end guard.
    const int barW = 40;
    int x = 20;
    p.fillRect(x, 16, barW, 80, Qt::white);
    x += barW + 8;
    for (int bit = 0; bit < 10; ++bit) {
        bool on = (frameNo >> bit) & 1;
        p.fillRect(x, 16, barW, 80, on ? Qt::white : Qt::black);
        p.setPen(on ? Qt::black : Qt::white);
        p.drawRect(x, 16, barW, 80);
        p.setPen(Qt::white);
        x += barW + 8;
    }
    p.fillRect(x, 16, barW, 80, Qt::white);

    p.setPen(Qt::black);
    QFont f = p.font();
    f.setPointSize(64);
    f.setBold(true);
    p.setFont(f);
    p.drawText(QRect(0, 120, img.width(), img.height() - 120),
               Qt::AlignCenter, QString::number(frameNo));
    // Small footer text so a human can see timestamps too.
    QFont sf = p.font();
    sf.setPointSize(14);
    sf.setBold(false);
    p.setFont(sf);
    p.drawText(QRect(12, img.height() - 34, img.width() - 24, 28),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("FRAME %1").arg(frameNo, 4, 10, QChar('0')));
}

// Decode helper lives in the probe; genclip only writes.
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    GenOpts o;
    for (int i = 1; i < argc; ++i) {
        QString a = argv[i];
        auto next = [&](QString name) -> QString {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", qPrintable(name)); exit(2); }
            return QString::fromLocal8Bit(argv[++i]);
        };
        if (a == "-o") o.out = next(a);
        else if (a == "-w") o.width = next(a).toInt();
        else if (a == "-h2") o.height = next(a).toInt();
        else if (a == "-n") o.frames = next(a).toInt();
        else if (a == "--audio") o.audio = true;
        else if (a == "--corrupt-tail") o.corruptTail = true;
        else if (a == "--help" || a == "-h") {
            printf("usage: srt_genclip -o out.mkv [-n frames] [--audio] [--corrupt-tail]\n");
            return 0;
        }
    }
    if (o.out.isEmpty()) { fprintf(stderr, "-o required\n"); return 2; }

    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, nullptr,
                                       o.out.toUtf8().constData()) < 0) {
        fprintf(stderr, "alloc output failed\n"); return 1;
    }
    const AVCodec* vc = avcodec_find_encoder(AV_CODEC_ID_H264);
    AVStream* vs = avformat_new_stream(oc, nullptr);
    AVCodecContext* vctx = avcodec_alloc_context3(vc);
    vctx->width = o.width;
    vctx->height = o.height;
    vctx->pix_fmt = AV_PIX_FMT_YUV420P;
    vctx->time_base = AVRational{1, 1'000'000};
    vctx->framerate = AVRational{25, 1}; // nominal only; PTS is explicitly VFR
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_opt_set(vctx->priv_data, "preset", "veryfast", 0);
    av_opt_set(vctx->priv_data, "crf", "18", 0);
    // Every frame a keyframe? No: keep realistic GOP so seek-through is tested.
    av_opt_set(vctx->priv_data, "g", "12", 0);
    if (avcodec_open2(vctx, vc, nullptr) < 0) { fprintf(stderr, "venc open\n"); return 1; }
    avcodec_parameters_from_context(vs->codecpar, vctx);
    vs->time_base = vctx->time_base;

    AVStream* as = nullptr;
    AVCodecContext* actx = nullptr;
    SwrContext* toneSwr = nullptr;
    if (o.audio) {
        const AVCodec* ac = avcodec_find_encoder(AV_CODEC_ID_AAC);
        as = avformat_new_stream(oc, nullptr);
        actx = avcodec_alloc_context3(ac);
        actx->sample_rate = 48000;
        actx->sample_fmt = ac->sample_fmts ? ac->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&actx->ch_layout, 2);
        actx->bit_rate = 128000;
        actx->time_base = AVRational{1, actx->sample_rate};
        if (oc->oformat->flags & AVFMT_GLOBALHEADER) actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(actx, ac, nullptr) < 0) { fprintf(stderr, "aenc open\n"); return 1; }
        avcodec_parameters_from_context(as->codecpar, actx);
        as->time_base = AVRational{1, actx->sample_rate};

        toneSwr = swr_alloc();
        av_opt_set_chlayout(toneSwr, "in_chlayout", &actx->ch_layout, 0);
        av_opt_set_chlayout(toneSwr, "out_chlayout", &actx->ch_layout, 0);
        av_opt_set_int(toneSwr, "in_sample_rate", 48000, 0);
        av_opt_set_int(toneSwr, "out_sample_rate", 48000, 0);
        av_opt_set_sample_fmt(toneSwr, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
        av_opt_set_sample_fmt(toneSwr, "out_sample_fmt", actx->sample_fmt, 0);
        swr_init(toneSwr);
    }

    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&oc->pb, o.out.toUtf8().constData(), AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "avio open\n"); return 1;
        }
    }
    if (avformat_write_header(oc, nullptr) < 0) { fprintf(stderr, "header\n"); return 1; }

    SwsContext* sws = sws_getContext(o.width, o.height, AV_PIX_FMT_RGB24,
                                     o.width, o.height, AV_PIX_FMT_YUV420P,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    AVFrame* vf = av_frame_alloc();
    vf->format = AV_PIX_FMT_YUV420P;
    vf->width = o.width; vf->height = o.height;
    av_frame_get_buffer(vf, 0);
    AVPacket* vpkt = av_packet_alloc();

    int64_t pts = 0;
    for (int n = 0; n < o.frames; ++n) {
        QImage img(o.width, o.height, QImage::Format_RGB888);
        paintFrame(img, n);
        av_frame_make_writable(vf);
        const uint8_t* data[4] = { img.constBits(), nullptr, nullptr, nullptr };
        int stride[4] = { static_cast<int>(img.bytesPerLine()), 0, 0, 0 };
        sws_scale(sws, data, stride, 0, o.height, vf->data, vf->linesize);
        vf->pts = pts;
        pts += o.patternUs[n % o.patternUs.size()]; // explicit non-uniform PTS

        avcodec_send_frame(vctx, vf);
        while (avcodec_receive_packet(vctx, vpkt) == 0) {
            av_packet_rescale_ts(vpkt, vctx->time_base, vs->time_base);
            vpkt->stream_index = vs->index;
            av_interleaved_write_frame(oc, vpkt);
        }
    }
    avcodec_send_frame(vctx, nullptr);
    while (avcodec_receive_packet(vctx, vpkt) == 0) {
        av_packet_rescale_ts(vpkt, vctx->time_base, vs->time_base);
        vpkt->stream_index = vs->index;
        av_interleaved_write_frame(oc, vpkt);
    }

    if (o.audio) {
        // 440 Hz tone covering roughly the video duration; 1024-sample chunks.
        const int totalSamples = static_cast<int>(pts / 1'000'000.0 * 48000);
        int made = 0;
        int16_t s16[2048];
        AVPacket* ap = av_packet_alloc();
        while (made < totalSamples) {
            int chunk = qMin(1024, totalSamples - made);
            AVFrame* afr = av_frame_alloc();
            afr->format = AV_SAMPLE_FMT_S16;
            afr->nb_samples = chunk;
            av_channel_layout_copy(&afr->ch_layout, &actx->ch_layout);
            afr->sample_rate = 48000;
            av_frame_get_buffer(afr, 0);
            for (int i = 0; i < chunk; ++i) {
                int16_t v = static_cast<int16_t>(
                    12000.0 * std::sin(2.0 * M_PI * 440.0 * (made + i) / 48000.0));
                s16[i * 2] = v; s16[i * 2 + 1] = v;
            }
            AVFrame* conv = av_frame_alloc();
            conv->format = actx->sample_fmt;
            conv->nb_samples = chunk;
            av_channel_layout_copy(&conv->ch_layout, &actx->ch_layout);
            conv->sample_rate = 48000;
            av_frame_get_buffer(conv, 0);
            const uint8_t* in[1] = { reinterpret_cast<const uint8_t*>(s16) };
            swr_convert(toneSwr, conv->data, chunk, in, chunk);
            conv->pts = made;
            made += chunk;
            if (avcodec_send_frame(actx, conv) >= 0) {
                while (avcodec_receive_packet(actx, ap) == 0) {
                    av_packet_rescale_ts(ap, actx->time_base, as->time_base);
                    ap->stream_index = as->index;
                    av_interleaved_write_frame(oc, ap);
                }
            }
            av_frame_free(&conv);
            av_frame_free(&afr);
        }
        avcodec_send_frame(actx, nullptr);
        while (avcodec_receive_packet(actx, ap) == 0) {
            av_packet_rescale_ts(ap, actx->time_base, as->time_base);
            ap->stream_index = as->index;
            av_interleaved_write_frame(oc, ap);
        }
        av_packet_free(&ap);
    }

    av_write_trailer(oc);
    sws_freeContext(sws);
    if (toneSwr) swr_free(&toneSwr);
    av_frame_free(&vf);
    av_packet_free(&vpkt);
    avcodec_free_context(&vctx);
    if (actx) avcodec_free_context(&actx);
    if (!(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb);
    avformat_free_context(oc);

    if (o.corruptTail) {
        // Simulate a broken/truncated recording: chop off the last 18% of the
        // file bytes, leaving undecodable tail data without a clean trailer.
        QFile f(o.out);
        if (f.open(QIODevice::ReadOnly)) {
            const qint64 sz = f.size();
            const qint64 cut = sz - sz * 18 / 100;
            QByteArray all = f.read(cut + 1);
            f.close();
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(all.constData(), cut);
                // Append garbage to mimic damaged trailing bytes.
                f.write(QByteArray(64, '\x77'));
                f.close();
            }
        }
    }
    printf("wrote %s (%d frames, VFR, audio=%d, corruptTail=%d)\n",
           qPrintable(o.out), o.frames, o.audio, o.corruptTail);
    return 0;
}
