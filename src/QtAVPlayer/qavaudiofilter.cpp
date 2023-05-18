/*********************************************************
 * Copyright (C) 2021, Val Doroshchuk <valbok@gmail.com> *
 *                                                       *
 * This file is part of QtAVPlayer.                      *
 * Free Qt Media Player based on FFmpeg.                 *
 *********************************************************/

#include "qavaudiofilter_p.h"
#include "qavfilter_p_p.h"
#include "qavcodec_p.h"
#include "qavstream.h"
#include <QDebug>

extern "C" {
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/avassert.h>
#include <libavutil/bprint.h>
#include <libavformat/avformat.h>
}

QT_BEGIN_NAMESPACE

class QAVAudioFilterPrivate : public QAVFilterPrivate
{
public:
    QAVAudioFilterPrivate(QAVFilter *q, QMutex &mutex) : QAVFilterPrivate(q, mutex) { }

    QList<QAVAudioInputFilter> inputs;
    QList<QAVAudioOutputFilter> outputs;
    int64_t filter_in_rescale_delta_last = AV_NOPTS_VALUE;
};

QAVAudioFilter::QAVAudioFilter(
    const QAVStream &stream,
    const QString &name,
    const QList<QAVAudioInputFilter> &inputs,
    const QList<QAVAudioOutputFilter> &outputs,
    QMutex &mutex,
    QObject *parent)
    : QAVFilter(
        stream,
        name,
        *new QAVAudioFilterPrivate(this, mutex),
        parent)
{
    Q_D(QAVAudioFilter);
    d->inputs = inputs;
    d->outputs = outputs;
}

int QAVAudioFilter::write(const QAVFrame &frame)
{
    Q_D(QAVAudioFilter);
    if (!frame || frame.stream().stream()->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        qWarning() << "Frame is not audio";
        return AVERROR(EINVAL);
    }
    if (!d->isEmpty)
        return AVERROR(EAGAIN);

    d->sourceFrame = frame;
    AVFrame *decoded_frame = frame.frame();
    AVRational decoded_frame_tb = frame.stream().stream()->time_base;
    // TODO: clear filter_in_rescale_delta_last
    if (decoded_frame->pts != AV_NOPTS_VALUE) {
        decoded_frame->pts = av_rescale_delta(decoded_frame_tb, decoded_frame->pts,
                                              AVRational{1, decoded_frame->sample_rate},
                                              decoded_frame->nb_samples,
                                              &d->filter_in_rescale_delta_last,
                                              AVRational{1, decoded_frame->sample_rate});
    }

    for (auto &filter : d->inputs) {
        QAVFrame ref = frame;
        QMutexLocker locker(&d->graphMutex);
        int ret = av_buffersrc_add_frame_flags(filter.ctx(), ref.frame(), AV_BUFFERSRC_FLAG_PUSH);
        if (ret < 0)
            return ret;
    }
    d->isEmpty = false;
    return 0;
}

void QAVAudioFilter::read(QAVFrame &frame)
{
    Q_D(QAVAudioFilter);
    if (d->outputs.isEmpty() || d->isEmpty) {
        frame = d->sourceFrame;
        d->sourceFrame = {};
        d->isEmpty = true;
        return;
    }

    int ret = 0;
    if (d->outputFrames.isEmpty()) {
        for (int i = 0; i < d->outputs.size(); ++i) {
            const auto &filter = d->outputs[i];
            while (true) {
                QAVFrame out = d->sourceFrame;
                // av_buffersink_get_frame_flags allocates frame's data
                av_frame_unref(out.frame());
                {
                    QMutexLocker locker(&d->graphMutex);
                    ret = av_buffersink_get_frame_flags(filter.ctx(), out.frame(), 0);
                }
                if (ret < 0)
                    break;

                if (!out.frame()->pkt_duration)
                    out.frame()->pkt_duration = d->sourceFrame.frame()->pkt_duration;
                frame.setTimeBase(av_buffersink_get_time_base(filter.ctx()));
                out.setFilterName(
                    !filter.name().isEmpty()
                    ? filter.name()
                    : QString(QLatin1String("%1:%2")).arg(d->name).arg(QString::number(i)));
                if (!out.stream())
                    out.setStream(d->stream);
                d->outputFrames.push_back(out);
            }
        }
    }

    if (!d->outputFrames.isEmpty())
        frame = d->outputFrames.takeFirst();
    if (d->outputFrames.isEmpty()) {
        d->sourceFrame = {};
        d->isEmpty = true;
    }
}

void QAVAudioFilter::flush()
{
    Q_D(QAVAudioFilter);
    for (const auto &filter : d->inputs) {
        int ret = av_buffersrc_add_frame(filter.ctx(), nullptr);
        if (ret < 0)
            qWarning() << "Could not flush:" << ret;
    }
    d->isEmpty = false;
}

QT_END_NAMESPACE
