#include "core/Exporter.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <cstring>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace sr {

static QString avErr(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

QString Exporter::audioModeName(AudioMode m)
{
    switch (m) {
    case AudioMode::Mute:   return QStringLiteral("mute (no audio track)");
    case AudioMode::Drop:   return QStringLiteral("resample (pitch shifts, not preserved)");
    case AudioMode::Atempo: return QStringLiteral("atempo filter (pitch preserved)");
    }
    return {};
}

Exporter::Exporter(VideoDecoder& decoder) : m_dec(decoder) {}

namespace {

// Per-segment pitch-preserving tempo chain. atempo accepts [0.5, 100]; slower
// factors are stacked as chained 0.5 stages like the legacy constant path did.
QString atempoChain(double speed)
{
    QString chain;
    double remaining = speed;
    while (remaining < 0.5 - 1e-9) {
        chain += QStringLiteral("atempo=0.5,");
        remaining /= 0.5;
    }
    chain += QStringLiteral("atempo=%1").arg(remaining, 0, 'f', 6);
    return chain;
}

struct AtempoStage {
    AVFilterGraph* graph = nullptr;
    AVFilterContext* src = nullptr;
    AVFilterContext* sink = nullptr;
};

struct AtempoParams {
    AVRational timeBase;
    int sampleRate = 0;
};

// Build one filter graph per output segment. A fresh graph per segment means
// its algorithmic delay is fully drained (EOF flush) before the next segment
// starts, so no filter tail can bleed across a speed change or a freeze.
bool buildAtempo(const AtempoParams& params, double speed, AtempoStage& out,
                 QString* err)
{
    out.graph = avfilter_graph_alloc();
    if (!out.graph) { *err = QStringLiteral("filter graph alloc failed"); return false; }
    char args[512];
    snprintf(args, sizeof(args),
             "time_base=%d/%d:sample_rate=%d:sample_fmt=fltp:channel_layout=stereo",
             params.timeBase.num, params.timeBase.den, params.sampleRate);
    if (avfilter_graph_create_filter(&out.src, avfilter_get_by_name("abuffer"),
                                     "in", args, nullptr, out.graph) < 0) {
        *err = QStringLiteral("abuffer create failed"); return false;
    }
    if (avfilter_graph_create_filter(&out.sink, avfilter_get_by_name("abuffersink"),
                                     "out", nullptr, nullptr, out.graph) < 0) {
        *err = QStringLiteral("abuffersink create failed"); return false;
    }
    // Lock the sink to the exact FLTP/stereo/rate format the encoder expects.
    const enum AVSampleFormat sinkFmts[] = {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_NONE};
    av_opt_set_int_list(out.sink, "sample_fmts", sinkFmts,
                        AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    av_opt_set(out.sink, "ch_layouts", "stereo", AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(out.sink, "sample_rate", params.sampleRate,
                   AV_OPT_SEARCH_CHILDREN);
    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = out.src;
    outputs->pad_idx = 0;
    outputs->next = nullptr;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = out.sink;
    inputs->pad_idx = 0;
    inputs->next = nullptr;
    const QByteArray chain = atempoChain(speed).toUtf8();
    int rc = avfilter_graph_parse_ptr(out.graph, chain.constData(),
                                      &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (rc < 0 || avfilter_graph_config(out.graph, nullptr) < 0) {
        *err = QStringLiteral("atempo graph config failed for speed %1").arg(speed);
        return false;
    }
    return true;
}

} // namespace

bool Exporter::run(const ExportSettings& s, ExportReport& report,
                   std::function<void(int, int)> progress)
{
    report = ExportReport{};
    report.startFrame = s.startFrame;
    report.endFrame = s.endFrame;
    report.audioMode = s.audioMode;

    const auto& idx = m_dec.index();
    if (s.startFrame < 0 || s.endFrame >= idx.size() || s.startFrame > s.endFrame) {
        report.error = QStringLiteral("invalid frame range [%1,%2], count=%3")
                           .arg(s.startFrame).arg(s.endFrame).arg(idx.size());
        return false;
    }
    if (!s.hasCurve && !(s.speed > 0.0 && s.speed <= 1.0)) {
        report.error = QStringLiteral("speed must be in (0,1] for slow motion");
        return false;
    }

    const int64_t spanStartUs = idx[s.startFrame].ptsUs;
    const int64_t sourceEndUs = idx[s.endFrame].ptsUs + idx[s.endFrame].durationUs;
    const int64_t sourceSpanUs = sourceEndUs - spanStartUs;

    // ---- The single source of truth for every output frame ---------------
    // Both the interactive preview and this render call FramePlanner::plan on
    // the same SpeedCurve; there is no second approximation in the exporter.
    const SpeedCurve curve = s.effectiveCurve(sourceSpanUs);
    QString planError;
    FramePlan plan = FramePlanner::plan(idx, s.startFrame, s.endFrame, curve,
                                        s.outputFrameRate, s.slowmoMode, &planError);
    if (!plan.ok) {
        report.error = planError;
        return false;
    }
    const int fps = plan.outputFrameRate;
    const int64_t outStepUs = plan.outputStepUs;
    const int64_t expectedDurationUs = plan.outputDurationUs;
    const int totalOut = plan.frames.size();

    // ---- Load the exact source frames of the selected interval ------------
    QVector<QImage> src;
    src.reserve(s.endFrame - s.startFrame + 1);
    QString err;
    if (!m_dec.seekToFrame(s.startFrame, &err)) {
        report.error = QStringLiteral("seek failed: %1").arg(err);
        return false;
    }
    DecodedFrame f;
    int expected = s.startFrame;
    while (m_dec.readNextFrame(f, &err)) {
        if (f.index < s.startFrame) continue;
        if (f.index != expected) {
            report.error = QStringLiteral("frame sequence gap: got %1 expected %2")
                               .arg(f.index).arg(expected);
            return false;
        }
        src.push_back(f.image.copy());
        if (f.index == s.endFrame) break;
        ++expected;
    }
    if (src.size() != s.endFrame - s.startFrame + 1) {
        report.error = QStringLiteral("could not decode full interval (%1/%2 frames)")
                           .arg(src.size()).arg(s.endFrame - s.startFrame + 1);
        return false;
    }
    FramePlanner::materialise(plan, idx, s.startFrame, src, s.slowmoMode);

    // ---- Mux + encode ------------------------------------------------------
    const int w = m_dec.summary().width;
    const int h = m_dec.summary().height;
    AVFormatContext* oc = nullptr;
    AVCodecContext* vc = nullptr;
    AVCodecContext* ac = nullptr;
    AVStream* vst = nullptr;
    AVStream* ast = nullptr;
    bool ioOpened = false;
    auto failOut = [&](const QString& msg) -> bool {
        report.error = msg;
        if (vc) avcodec_free_context(&vc);
        if (ac) avcodec_free_context(&ac);
        if (oc) {
            if (ioOpened && oc->pb) avio_closep(&oc->pb);
            avformat_free_context(oc);
        }
        return false;
    };

    int rc = avformat_alloc_output_context2(&oc, nullptr, nullptr,
                                            s.outputPath.toUtf8().constData());
    if (rc < 0 || !oc) return failOut(QStringLiteral("output context: %1").arg(avErr(rc)));

    // Container PTS precision. MP4/MOV accept an arbitrary timescale, so we
    // request microseconds; Matroska's timestamp precision is fixed at 1 ms by
    // specification, NUT keeps fine precision. The effective value is recorded
    // in the sidecar so the verifier applies the right tolerance.
    int64_t containerTickUs = 1;
    const char* ofName = oc->oformat->name ? oc->oformat->name : "";
    const bool isMp4Family = strstr(ofName, "mp4") || strstr(ofName, "mov")
                             || strstr(ofName, "3gp") || strstr(ofName, "ipod")
                             || strstr(ofName, "psp");
    if (isMp4Family) {
        av_opt_set_int(oc->priv_data, "video_track_timescale", 1'000'000, 0);
        containerTickUs = 1;
    } else if (strstr(ofName, "matroska") || strstr(ofName, "webm")) {
        containerTickUs = 1000;
    }

    const AVCodec* venc = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!venc) venc = avcodec_find_encoder(oc->oformat->video_codec);
    if (!venc) return failOut(QStringLiteral("no usable video encoder"));
    vst = avformat_new_stream(oc, nullptr);
    vc = avcodec_alloc_context3(venc);
    vc->width = w;
    vc->height = h;
    vc->pix_fmt = AV_PIX_FMT_YUV420P;
    vc->time_base = AVRational{1, 1'000'000};
    vc->framerate = AVRational{fps, 1};
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) vc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_opt_set(vc->priv_data, "preset", "veryfast", 0);
    av_opt_set(vc->priv_data, "crf", "20", 0);
    rc = avcodec_open2(vc, venc, nullptr);
    if (rc < 0) return failOut(QStringLiteral("video encoder open: %1").arg(avErr(rc)));
    avcodec_parameters_from_context(vst->codecpar, vc);
    // Keep microsecond precision in the container so VFR output PTS is not
    // quantised to the muxer's default millisecond timebase.
    vst->time_base = AVRational{1, 1'000'000};

    const bool wantAudio = s.audioMode != AudioMode::Mute
                           && m_dec.summary().audioStreamIndex >= 0;
    if (wantAudio) {
        const AVCodec* aenc = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!aenc) return failOut(QStringLiteral("no AAC encoder"));
        ast = avformat_new_stream(oc, nullptr);
        ac = avcodec_alloc_context3(aenc);
        ac->sample_rate = 48000;
        ac->sample_fmt = aenc->sample_fmts ? aenc->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&ac->ch_layout, 2);
        ac->bit_rate = 128000;
        ac->time_base = AVRational{1, ac->sample_rate};
        if (oc->oformat->flags & AVFMT_GLOBALHEADER) ac->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        rc = avcodec_open2(ac, aenc, nullptr);
        if (rc < 0) return failOut(QStringLiteral("audio encoder open: %1").arg(avErr(rc)));
        avcodec_parameters_from_context(ast->codecpar, ac);
        ast->time_base = AVRational{1, ac->sample_rate};
    }

    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&oc->pb, s.outputPath.toUtf8().constData(), AVIO_FLAG_WRITE);
        if (rc < 0) return failOut(QStringLiteral("avio_open: %1").arg(avErr(rc)));
        ioOpened = true;
    }
    rc = avformat_write_header(oc, nullptr);
    if (rc < 0) return failOut(QStringLiteral("write_header: %1").arg(avErr(rc)));

    // Video encode loop. Images come from the shared planner output.
    {
        SwsContext* sws = sws_getContext(w, h, AV_PIX_FMT_RGB24,
                                         w, h, AV_PIX_FMT_YUV420P,
                                         SWS_BILINEAR, nullptr, nullptr, nullptr);
        AVFrame* vf = av_frame_alloc();
        vf->format = AV_PIX_FMT_YUV420P;
        vf->width = w; vf->height = h;
        av_frame_get_buffer(vf, 0);
        AVPacket* vpkt = av_packet_alloc();

        bool encError = false;
        for (int i = 0; i < plan.frames.size(); ++i) {
            const auto& pl = plan.frames[i];
            if (pl.image.isNull()) {
                failOut(QStringLiteral("planned frame %1 has no materialised image").arg(i));
                sws_freeContext(sws);
                av_frame_free(&vf);
                av_packet_free(&vpkt);
                return false;
            }
            av_frame_make_writable(vf);
            const uint8_t* srcData[4] = { pl.image.constBits(), nullptr, nullptr, nullptr };
            int srcStride[4] = { static_cast<int>(pl.image.bytesPerLine()), 0, 0, 0 };
            sws_scale(sws, srcData, srcStride, 0, h, vf->data, vf->linesize);
            vf->pts = pl.prov.outputPtsUs;
            rc = avcodec_send_frame(vc, vf);
            if (rc < 0) { report.error = QStringLiteral("video send: %1").arg(avErr(rc)); encError = true; break; }
            while (avcodec_receive_packet(vc, vpkt) == 0) {
                av_packet_rescale_ts(vpkt, vc->time_base, vst->time_base);
                vpkt->stream_index = vst->index;
                av_interleaved_write_frame(oc, vpkt);
            }
            if (progress) progress(i + 1, totalOut);
        }
        if (!encError) {
            avcodec_send_frame(vc, nullptr);
            while (avcodec_receive_packet(vc, vpkt) == 0) {
                av_packet_rescale_ts(vpkt, vc->time_base, vst->time_base);
                vpkt->stream_index = vst->index;
                av_interleaved_write_frame(oc, vpkt);
            }
        }
        sws_freeContext(sws);
        av_frame_free(&vf);
        av_packet_free(&vpkt);
        if (encError) return failOut(report.error);
    }

    // ---- Audio path --------------------------------------------------------
    // The audio timeline follows the exact same inverse segments as the
    // video: every non-freeze segment is rendered with its own atempo graph
    // (pitch preserved) or resampler (pitch shifted), every freeze segment is
    // filled per the explicit freeze policy, and every graph is fully flushed
    // so filter algorithmic delay never crosses a speed boundary.
    if (wantAudio) {
        AVFormatContext* ifmt = m_dec.formatCtx();
        const int aIdx = m_dec.summary().audioStreamIndex;
        AVCodecContext* inAc = avcodec_alloc_context3(nullptr);
        avcodec_parameters_to_context(inAc, ifmt->streams[aIdx]->codecpar);
        const AVCodec* inDec = avcodec_find_decoder(inAc->codec_id);
        if (!inDec || avcodec_open2(inAc, inDec, nullptr) < 0) {
            avcodec_free_context(&inAc);
            return failOut(QStringLiteral("source audio decoder unavailable"));
        }
        inAc->pkt_timebase = ifmt->streams[aIdx]->time_base;

        const int64_t startTs = av_rescale_q(spanStartUs, AVRational{1, 1'000'000},
                                             ifmt->streams[aIdx]->time_base);
        const int64_t endTs = av_rescale_q(sourceEndUs, AVRational{1, 1'000'000},
                                           ifmt->streams[aIdx]->time_base);
        av_seek_frame(ifmt, aIdx, startTs, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(inAc);

        // Stage 1: normalise every decoded frame to FLTP / stereo / its own rate.
        SwrContext* normSwr = swr_alloc();
        av_opt_set_chlayout(normSwr, "in_chlayout", &inAc->ch_layout, 0);
        AVChannelLayout stereoLayout;
        av_channel_layout_default(&stereoLayout, 2);
        av_opt_set_chlayout(normSwr, "out_chlayout", &stereoLayout, 0);
        av_opt_set_int(normSwr, "in_sample_rate", inAc->sample_rate, 0);
        av_opt_set_int(normSwr, "out_sample_rate", inAc->sample_rate, 0);
        av_opt_set_sample_fmt(normSwr, "in_sample_fmt",
                              static_cast<AVSampleFormat>(inAc->sample_fmt), 0);
        av_opt_set_sample_fmt(normSwr, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
        if (swr_init(normSwr) < 0) {
            swr_free(&normSwr);
            avcodec_free_context(&inAc);
            return failOut(QStringLiteral("audio normaliser init failed"));
        }

        auto makeEncoderSwr = [&](int inputRate) {
            SwrContext* c = swr_alloc();
            av_opt_set_chlayout(c, "in_chlayout", &stereoLayout, 0);
            av_opt_set_chlayout(c, "out_chlayout", &ac->ch_layout, 0);
            av_opt_set_int(c, "in_sample_rate", inputRate, 0);
            av_opt_set_int(c, "out_sample_rate", ac->sample_rate, 0);
            av_opt_set_sample_fmt(c, "in_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
            av_opt_set_sample_fmt(c, "out_sample_fmt",
                                  static_cast<AVSampleFormat>(ac->sample_fmt), 0);
            swr_init(c);
            return c;
        };
        SwrContext* toEnc = makeEncoderSwr(inAc->sample_rate);

        // ATEMPO parameters: every per-segment graph locks the same input and
        // sink sample format so the encoder pipeline never sees a surprise.
        AtempoParams atempoParams;
        if (s.audioMode == AudioMode::Atempo) {
            atempoParams.timeBase = inAc->pkt_timebase;
            atempoParams.sampleRate = inAc->sample_rate;
        }

        const int encFrameSize = ac->frame_size > 0 ? ac->frame_size : 1024;
        const int encChannels = ac->ch_layout.nb_channels;
        const int encBps = av_get_bytes_per_sample(ac->sample_fmt);
        const bool encPlanar = av_sample_fmt_is_planar(ac->sample_fmt);
        QVector<QByteArray> fifo(encChannels);
        int64_t encSamplesTotal = 0;

        auto drainFifo = [&]() {
            int64_t available = fifo.isEmpty() ? 0 : fifo[0].size() / encBps;
            while (available >= encFrameSize) {
                AVFrame* out = av_frame_alloc();
                out->format = ac->sample_fmt;
                av_channel_layout_copy(&out->ch_layout, &ac->ch_layout);
                out->sample_rate = ac->sample_rate;
                out->nb_samples = encFrameSize;
                av_frame_get_buffer(out, 0);
                if (encPlanar) {
                    for (int ch = 0; ch < encChannels; ++ch) {
                        memcpy(out->data[ch], fifo[ch].constData(),
                               encFrameSize * encBps);
                        fifo[ch].remove(0, encFrameSize * encBps);
                    }
                } else {
                    const int bytes = encFrameSize * encChannels * encBps;
                    memcpy(out->data[0], fifo[0].constData(), bytes);
                    fifo[0].remove(0, bytes);
                }
                out->pts = av_rescale_q(encSamplesTotal,
                                        AVRational{1, ac->sample_rate}, ac->time_base);
                encSamplesTotal += encFrameSize;
                if (avcodec_send_frame(ac, out) >= 0) {
                    AVPacket* ap = av_packet_alloc();
                    while (avcodec_receive_packet(ac, ap) == 0) {
                        av_packet_rescale_ts(ap, ac->time_base, ast->time_base);
                        ap->stream_index = ast->index;
                        av_interleaved_write_frame(oc, ap);
                    }
                    av_packet_free(&ap);
                }
                av_frame_free(&out);
                available = fifo.isEmpty() ? 0 : fifo[0].size() / encBps;
            }
        };

        // Push a FLTP stereo frame at its declared rate into the encoder FIFO.
        auto pushSamples = [&](const AVFrame* fltp, SwrContext* fixedConv) {
            SwrContext* conv = fixedConv;
            const auto swrDeleter = [](SwrContext* c) { SwrContext* p = c; swr_free(&p); };
            std::unique_ptr<SwrContext, decltype(swrDeleter)> adHoc(nullptr, swrDeleter);
            if (!fixedConv || (fixedConv == toEnc && fltp->sample_rate != inAc->sample_rate)) {
                SwrContext* c = makeEncoderSwr(fltp->sample_rate);
                conv = c;
                adHoc.reset(c);
            }
            int cap = swr_get_out_samples(conv, fltp->nb_samples);
            if (cap <= 0) cap = fltp->nb_samples + 256;
            AVFrame* tmp = av_frame_alloc();
            tmp->format = ac->sample_fmt;
            av_channel_layout_copy(&tmp->ch_layout, &ac->ch_layout);
            tmp->sample_rate = ac->sample_rate;
            tmp->nb_samples = cap;
            if (av_frame_get_buffer(tmp, 0) < 0) { av_frame_free(&tmp); return; }
            const int got = swr_convert(conv, tmp->data, cap,
                                        const_cast<const uint8_t**>(fltp->data),
                                        fltp->nb_samples);
            if (got <= 0 || got > cap) { av_frame_free(&tmp); return; }
            if (encPlanar) {
                for (int ch = 0; ch < encChannels && ch < AV_NUM_DATA_POINTERS; ++ch)
                    fifo[ch].append(reinterpret_cast<const char*>(tmp->data[ch]),
                                    got * encBps);
            } else {
                fifo[0].append(reinterpret_cast<const char*>(tmp->data[0]),
                               got * encChannels * encBps);
            }
            av_frame_free(&tmp);
            drainFifo();
        };

        // Append `samples` FLTP frames' worth of audio for a freeze segment.
        auto appendFreeze = [&](int samples, const QVector<float>& lastSample) {
            if (samples <= 0) return;
            AVFrame* fr = av_frame_alloc();
            fr->format = AV_SAMPLE_FMT_FLTP;
            av_channel_layout_copy(&fr->ch_layout, &stereoLayout);
            fr->sample_rate = inAc->sample_rate;
            fr->nb_samples = samples;
            av_frame_get_buffer(fr, 0);
            for (int ch = 0; ch < qMin(2, encChannels); ++ch) {
                const float v = s.audioFreezePolicy == AudioFreezePolicy::HoldLast
                                    ? lastSample.value(ch, 0.0f) : 0.0f;
                float* dst = reinterpret_cast<float*>(fr->data[ch]);
                for (int i = 0; i < samples; ++i) dst[i] = v;
            }
            pushSamples(fr, toEnc);
            av_frame_free(&fr);
        };

        // Decode the whole selected source range once into normalised FLTP
        // frames. Segmentation afterwards is pure timeline bookkeeping, so the
        // mapping used for audio matches the video mapping exactly.
        struct NormFrame { QByteArray ch[2]; int64_t ptsUs = 0; int samples = 0; };
        QVector<NormFrame> normFrames;
        QVector<float> lastSample{0.0f, 0.0f};
        {
            AVPacket* pkt = av_packet_alloc();
            AVFrame* decFrame = av_frame_alloc();
            AVFrame* normFrame = av_frame_alloc();
            bool audioDone = false;
            while (!audioDone) {
                int r = av_read_frame(ifmt, pkt);
                if (r == AVERROR_EOF) {
                    avcodec_send_packet(inAc, nullptr);
                } else if (r < 0) {
                    audioDone = true;
                } else {
                    if (pkt->stream_index != aIdx) { av_packet_unref(pkt); continue; }
                    if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < startTs) {
                        av_packet_unref(pkt);
                        continue;
                    }
                    avcodec_send_packet(inAc, pkt);
                    av_packet_unref(pkt);
                }
                while (true) {
                    r = avcodec_receive_frame(inAc, decFrame);
                    if (r == AVERROR(EAGAIN)) break;
                    if (r == AVERROR_EOF) { audioDone = true; break; }
                    if (r < 0) { audioDone = true; break; }
                    if (decFrame->pts == AV_NOPTS_VALUE) { av_frame_unref(decFrame); continue; }
                    if (decFrame->pts > endTs) { audioDone = true; break; }

                    normFrame->format = AV_SAMPLE_FMT_FLTP;
                    av_channel_layout_copy(&normFrame->ch_layout, &stereoLayout);
                    normFrame->sample_rate = inAc->sample_rate;
                    int cap = swr_get_out_samples(normSwr, decFrame->nb_samples);
                    normFrame->nb_samples = cap;
                    av_frame_get_buffer(normFrame, 0);
                    const int got = swr_convert(normSwr, normFrame->data, cap,
                                                const_cast<const uint8_t**>(decFrame->data),
                                                decFrame->nb_samples);
                    if (got <= 0) { av_frame_unref(decFrame); continue; }
                    normFrame->nb_samples = got;
                    NormFrame nf;
                    nf.samples = got;
                    nf.ptsUs = av_rescale_q(decFrame->pts,
                                            ifmt->streams[aIdx]->time_base,
                                            AVRational{1, 1'000'000}) - spanStartUs;
                    for (int ch = 0; ch < 2; ++ch)
                        nf.ch[ch] = QByteArray(reinterpret_cast<const char*>(normFrame->data[ch]),
                                               got * int(sizeof(float)));
                    lastSample[0] = reinterpret_cast<const float*>(nf.ch[0].constData())[got - 1];
                    lastSample[1] = reinterpret_cast<const float*>(nf.ch[1].constData())[got - 1];
                    normFrames.push_back(std::move(nf));
                    av_frame_unref(normFrame);
                    av_frame_unref(decFrame);
                }
                if (r == AVERROR_EOF) break;
            }
            av_frame_free(&normFrame);
            av_frame_free(&decFrame);
            av_packet_free(&pkt);
        }

        // Slice normalised frames belonging to source interval [a,b) (rel us).
        auto sliceSource = [&](int64_t a, int64_t b) -> AVFrame* {
            AVFrame* part = av_frame_alloc();
            part->format = AV_SAMPLE_FMT_FLTP;
            av_channel_layout_copy(&part->ch_layout, &stereoLayout);
            part->sample_rate = inAc->sample_rate;
            QVector<QByteArray> buf(2);
            int total = 0;
            for (const auto& nf : normFrames) {
                const int64_t fStart = nf.ptsUs;
                const int64_t fEnd = nf.ptsUs
                    + av_rescale_q(nf.samples, AVRational{1, inAc->sample_rate},
                                   AVRational{1, 1'000'000});
                if (fEnd <= a || fStart >= b) continue;
                // Convert us bounds inside this frame to sample indices.
                const int64_t sampleDurUs = 1'000'000 / int64_t(inAc->sample_rate);
                int i0 = qBound(0, int((a - fStart) / qMax<int64_t>(1, sampleDurUs)),
                                nf.samples);
                int i1 = qBound(0, int((b - fStart) / qMax<int64_t>(1, sampleDurUs)),
                                nf.samples);
                if (i1 <= i0) continue;
                for (int ch = 0; ch < 2; ++ch)
                    buf[ch].append(nf.ch[ch].constData() + i0 * int(sizeof(float)),
                                   (i1 - i0) * int(sizeof(float)));
                total += i1 - i0;
            }
            part->nb_samples = total;
            if (total > 0) av_frame_get_buffer(part, 0);
            for (int ch = 0; ch < 2; ++ch)
                memcpy(part->data[ch], buf[ch].constData(), total * int(sizeof(float)));
            return part;
        };

        // Render one moving (non-freeze) segment through the real component
        // that implements the requested audio semantics.
        auto renderMoving = [&](const SpeedCurve::Segment& seg) {
            AVFrame* part = sliceSource(seg.srcStartUs, seg.srcEndUs);
            if (part->nb_samples <= 0) { av_frame_free(&part); return; }
            if (s.audioMode == AudioMode::Atempo) {
                AtempoStage st;
                QString gerr;
                if (!buildAtempo(atempoParams, seg.speed, st, &gerr)) {
                    report.error = gerr;
                    av_frame_free(&part);
                    return;
                }
                part->pts = av_rescale_q(seg.srcStartUs,
                                         AVRational{1, 1'000'000},
                                         AVRational{1, inAc->sample_rate});
                if (av_buffersrc_add_frame_flags(st.src, part, 0) < 0) {
                    report.error = QStringLiteral("atempo segment input failed");
                    av_frame_free(&part);
                    avfilter_graph_free(&st.graph);
                    return;
                }
                av_frame_free(&part);
                // EOF flush: atempo buffering/algorithmic delay is drained in
                // full before the next segment is built.
                rc = av_buffersrc_add_frame_flags(st.src, nullptr, 0);
                (void)rc;
                AVFrame* filt = av_frame_alloc();
                while (av_buffersink_get_frame(st.sink, filt) >= 0) {
                    pushSamples(filt, toEnc);
                    av_frame_unref(filt);
                }
                av_frame_free(&filt);
                avfilter_graph_free(&st.graph);
            } else {
                // DROP: resample to speed*rate, replay at the encoder rate.
                const int stretchedRate = qMax(1, qRound(inAc->sample_rate * seg.speed));
                SwrContext* stretchSwr = swr_alloc();
                av_opt_set_chlayout(stretchSwr, "in_chlayout", &stereoLayout, 0);
                av_opt_set_chlayout(stretchSwr, "out_chlayout", &stereoLayout, 0);
                av_opt_set_int(stretchSwr, "in_sample_rate", inAc->sample_rate, 0);
                av_opt_set_int(stretchSwr, "out_sample_rate", stretchedRate, 0);
                av_opt_set_sample_fmt(stretchSwr, "in_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
                av_opt_set_sample_fmt(stretchSwr, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
                swr_init(stretchSwr);
                SwrContext* conv = makeEncoderSwr(stretchedRate);
                int cap = swr_get_out_samples(stretchSwr, part->nb_samples);
                AVFrame* stretched = av_frame_alloc();
                stretched->format = AV_SAMPLE_FMT_FLTP;
                av_channel_layout_copy(&stretched->ch_layout, &stereoLayout);
                stretched->sample_rate = stretchedRate;
                stretched->nb_samples = cap;
                av_frame_get_buffer(stretched, 0);
                const int got = swr_convert(stretchSwr, stretched->data, cap,
                                            const_cast<const uint8_t**>(part->data),
                                            part->nb_samples);
                if (got > 0) {
                    stretched->nb_samples = got;
                    pushSamples(stretched, conv);
                }
                av_frame_free(&stretched);
                av_frame_free(&part);
                swr_free(&conv);
                swr_free(&stretchSwr);
            }
        };

        const auto segs = curve.inverseSegments();
        for (const auto& seg : segs) {
            if (seg.speed == 0.0) {
                const int samples = qMax(0, int(av_rescale_q(
                    seg.outEndUs - seg.outStartUs,
                    AVRational{1, 1'000'000},
                    AVRational{1, inAc->sample_rate})));
                appendFreeze(samples, lastSample);
            } else {
                renderMoving(seg);
                if (!report.error.isEmpty()) {
                    swr_free(&toEnc);
                    swr_free(&normSwr);
                    av_channel_layout_uninit(&stereoLayout);
                    avcodec_free_context(&inAc);
                    return failOut(report.error);
                }
            }
        }

        // Pad the remaining samples to a full encoder frame.
        if (!fifo.isEmpty() && !fifo[0].isEmpty()) {
            const int have = encPlanar ? fifo[0].size() / encBps
                                       : fifo[0].size() / (encChannels * encBps);
            const int pad = encFrameSize - have;
            if (pad > 0 && pad < encFrameSize) {
                if (encPlanar)
                    for (int ch = 0; ch < encChannels; ++ch)
                        fifo[ch].append(QByteArray(pad * encBps, '\0'));
                else
                    fifo[0].append(QByteArray(pad * encChannels * encBps, '\0'));
                drainFifo();
            }
        }
        avcodec_send_frame(ac, nullptr);
        AVPacket* ap = av_packet_alloc();
        while (avcodec_receive_packet(ac, ap) == 0) {
            av_packet_rescale_ts(ap, ac->time_base, ast->time_base);
            ap->stream_index = ast->index;
            av_interleaved_write_frame(oc, ap);
        }
        av_packet_free(&ap);
        swr_free(&toEnc);
        swr_free(&normSwr);
        
        av_channel_layout_uninit(&stereoLayout);
        avcodec_free_context(&inAc);
        report.hasAudio = true;
    }

    av_write_trailer(oc);
    if (ioOpened) avio_closep(&oc->pb);

    report.ok = true;
    report.curve = curve;
    report.outputPath = s.outputPath;
    report.outputFrameCount = plan.frames.size();
    report.sourceSpanUs = sourceSpanUs;
    report.expectedDurationUs = expectedDurationUs;
    report.outputDurationUs = plan.frames.last().prov.outputPtsUs + outStepUs;
    for (const auto& pf : plan.frames) {
        const auto& p = pf.prov;
        report.frames.push_back(p);
        if (p.kind == QLatin1String("freeze")) ++report.freezeFrameCount;
        if (p.generated) ++report.generatedFrameCount;
        else ++report.originalFrameCount;
    }
    report.sidecarPath = s.outputPath + QStringLiteral(".frames.json");
    {
        QJsonObject meta;
        meta["tool"] = QStringLiteral("sportreview");
        meta["slowmoMode"] = slowmoModeName(s.slowmoMode);
        meta["audioMode"] = audioModeName(s.audioMode);
        meta["audioFreezePolicy"] = audioFreezePolicyName(s.audioFreezePolicy);
        meta["hasCurve"] = s.hasCurve;
        meta["legacySpeed"] = s.speed;
        meta["speedCurve"] = curve.toJson();
        meta["outputFrameRate"] = fps;
        meta["startFrame"] = s.startFrame;
        meta["endFrame"] = s.endFrame;
        meta["sourceSpanUs"] = static_cast<qint64>(sourceSpanUs);
        meta["outputDurationUs"] = static_cast<qint64>(report.outputDurationUs);
        meta["containerTickUs"] = static_cast<qint64>(containerTickUs);
        meta["originalFrames"] = report.originalFrameCount;
        meta["generatedFrames"] = report.generatedFrameCount;
        meta["freezeFrames"] = report.freezeFrameCount;
        QJsonArray farr;
        for (const auto& p : report.frames) {
            QJsonObject po;
            po["outputIndex"] = p.outputIndex;
            po["outputPtsUs"] = static_cast<qint64>(p.outputPtsUs);
            po["kind"] = p.kind;
            po["generated"] = p.generated;
            po["sourceFrameA"] = p.sourceFrameA;
            po["sourceFrameB"] = p.sourceFrameB;
            po["blendWeight"] = p.blendWeight;
            po["sourceTimeUs"] = static_cast<qint64>(p.sourceTimeUs);
            po["segmentIndex"] = p.segmentIndex;
            po["localSpeed"] = p.localSpeed;
            farr.append(po);
        }
        meta["frames"] = farr;
        QFile sf(report.sidecarPath);
        if (sf.open(QIODevice::WriteOnly | QIODevice::Truncate))
            sf.write(QJsonDocument(meta).toJson(QJsonDocument::Indented));
    }
    avcodec_free_context(&vc);
    avcodec_free_context(&ac);
    avformat_free_context(oc);
    return true;
}

} // namespace sr
