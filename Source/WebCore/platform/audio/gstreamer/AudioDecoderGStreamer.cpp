/*
 * Copyright (C) 2023 Igalia S.L
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * aint with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "AudioDecoderGStreamer.h"

#if ENABLE(WEB_CODECS) && USE(GSTREAMER)

#include "GStreamerCommon.h"
#include "GStreamerElementHarness.h"
#include "GStreamerRegistryScanner.h"
#include "PlatformRawAudioDataGStreamer.h"
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/WorkQueue.h>
#include <wtf/text/MakeString.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(GStreamerAudioDecoder);

GST_DEBUG_CATEGORY(webkit_audio_decoder_debug);
#define GST_CAT_DEFAULT webkit_audio_decoder_debug

static WorkQueue& gstDecoderWorkQueue()
{
    static std::once_flag onceKey;
    static LazyNeverDestroyed<Ref<WorkQueue>> queue;
    std::call_once(onceKey, [] {
        queue.construct(WorkQueue::create("GStreamer AudioDecoder queue"_s));
    });
    return queue.get();
}

class GStreamerInternalAudioDecoder : public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<GStreamerInternalAudioDecoder> {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(GStreamerInternalAudioDecoder);

public:
    static Ref<GStreamerInternalAudioDecoder> create(const String& codecName, const AudioDecoder::Config& config, AudioDecoder::OutputCallback&& outputCallback, GRefPtr<GstElement>&& element)
    {
        return adoptRef(*new GStreamerInternalAudioDecoder(codecName, config, WTFMove(outputCallback), WTFMove(element)));
    }
    ~GStreamerInternalAudioDecoder() = default;

    Ref<AudioDecoder::DecodePromise> decode(std::span<const uint8_t>, bool isKeyFrame, int64_t timestamp, std::optional<uint64_t> duration);
    void flush();
    void close() { m_isClosed = true; }
    bool isConfigured() const { return !!m_inputCaps; }

    GstElement* harnessedElement() const { return m_harness->element(); }

private:
    GStreamerInternalAudioDecoder(const String& codecName, const AudioDecoder::Config&, AudioDecoder::OutputCallback&&, GRefPtr<GstElement>&&);

    AudioDecoder::OutputCallback m_outputCallback;

    RefPtr<GStreamerElementHarness> m_harness;
    GRefPtr<GstCaps> m_inputCaps;
    GRefPtr<GstBuffer> m_header;
    bool m_isClosed { false };
};

void GStreamerAudioDecoder::create(const String& codecName, const Config& config, CreateCallback&& callback, OutputCallback&& outputCallback)
{
    static std::once_flag debugRegisteredFlag;
    std::call_once(debugRegisteredFlag, [] {
        GST_DEBUG_CATEGORY_INIT(webkit_audio_decoder_debug, "webkitaudiodecoder", 0, "WebKit WebCodecs Audio Decoder");
    });

    GRefPtr<GstElement> element;
    if (codecName.startsWith("pcm-"_s)) {
        auto components = codecName.split('-');
        if (components.size() != 2) {
            GST_WARNING("Invalid LPCM codec string: %s", codecName.utf8().data());
            callback(makeUnexpected(makeString("Invalid LPCM codec string: "_s, codecName)));
            return;
        }
    } else {
        auto& scanner = GStreamerRegistryScanner::singleton();
        auto lookupResult = scanner.isCodecSupported(GStreamerRegistryScanner::Configuration::Decoding, codecName);
        if (!lookupResult) {
            GST_WARNING("No decoder found for codec %s", codecName.utf8().data());
            callback(makeUnexpected(makeString("No decoder found for codec "_s, codecName)));
            return;
        }
        element = gst_element_factory_create(lookupResult.factory.get(), nullptr);
    }

    Ref decoder = adoptRef(*new GStreamerAudioDecoder(codecName, config, WTFMove(outputCallback), WTFMove(element)));
    Ref internalDecoder = decoder->m_internalDecoder;
    if (!internalDecoder->isConfigured()) {
        GST_WARNING("Internal audio decoder failed to configure for codec %s", codecName.utf8().data());
        callback(makeUnexpected(makeString("Internal audio decoder failed to configure for codec "_s, codecName)));
        return;
    }

    gstDecoderWorkQueue().dispatch([callback = WTFMove(callback), decoder = WTFMove(decoder)]() mutable {
        auto internalDecoder = decoder->m_internalDecoder;
        GST_DEBUG_OBJECT(decoder->m_internalDecoder->harnessedElement(), "Audio decoder created");
        callback(Ref<AudioDecoder> { WTFMove(decoder) });
    });
}

GStreamerAudioDecoder::GStreamerAudioDecoder(const String& codecName, const Config& config, OutputCallback&& outputCallback, GRefPtr<GstElement>&& element)
    : m_internalDecoder(GStreamerInternalAudioDecoder::create(codecName, config, WTFMove(outputCallback), WTFMove(element)))
{
}

GStreamerAudioDecoder::~GStreamerAudioDecoder()
{
    GST_DEBUG_OBJECT(m_internalDecoder->harnessedElement(), "Disposing");
    close();
}

Ref<AudioDecoder::DecodePromise> GStreamerAudioDecoder::decode(EncodedData&& data)
{
    return invokeAsync(gstDecoderWorkQueue(), [value = Vector<uint8_t> { data.data }, isKeyFrame = data.isKeyFrame, timestamp = data.timestamp, duration = data.duration, decoder = m_internalDecoder] {
        return decoder->decode({ value.data(), value.size() }, isKeyFrame, timestamp, duration);
    });
}

Ref<GenericPromise> GStreamerAudioDecoder::flush()
{
    return invokeAsync(gstDecoderWorkQueue(), [decoder = m_internalDecoder] {
        decoder->flush();
        return GenericPromise::createAndResolve();
    });
}

void GStreamerAudioDecoder::reset()
{
    m_internalDecoder->close();
}

void GStreamerAudioDecoder::close()
{
    m_internalDecoder->close();
}

GStreamerInternalAudioDecoder::GStreamerInternalAudioDecoder(const String& codecName, const AudioDecoder::Config& config, AudioDecoder::OutputCallback&& outputCallback, GRefPtr<GstElement>&& element)
    : m_outputCallback(WTFMove(outputCallback))
{
    GST_DEBUG_OBJECT(element.get(), "Configuring decoder for codec %s", codecName.ascii().data());

    const char* parser = nullptr;
    if (codecName.startsWith("mp4a"_s)) {
        m_inputCaps = adoptGRef(gst_caps_new_simple("audio/mpeg", "mpegversion", G_TYPE_INT, 4, "channels", G_TYPE_INT, config.numberOfChannels, nullptr));
        auto codecData = wrapSpanData(config.description);
        if (codecData)
            gst_caps_set_simple(m_inputCaps.get(), "codec_data", GST_TYPE_BUFFER, codecData.get(), "stream-format", G_TYPE_STRING, "raw", nullptr);
        else
            gst_caps_set_simple(m_inputCaps.get(), "stream-format", G_TYPE_STRING, "adts", nullptr);
    } else if (codecName == "mp3"_s) {
        m_inputCaps = adoptGRef(gst_caps_new_simple("audio/mpeg", "mpegversion", G_TYPE_INT, 1, "layer", G_TYPE_INT, 3, "rate", G_TYPE_INT, config.sampleRate, "channels", G_TYPE_INT, config.numberOfChannels, "parsed", G_TYPE_BOOLEAN, TRUE, nullptr));
    } else if (codecName == "opus"_s) {
        int channelMappingFamily = config.numberOfChannels <= 2 ? 0 : 1;
        m_inputCaps = adoptGRef(gst_caps_new_simple("audio/x-opus", "channel-mapping-family", G_TYPE_INT, channelMappingFamily, nullptr));
        m_header = wrapSpanData(config.description);
        if (m_header)
            parser = "opusparse";
    } else if (codecName == "alaw"_s)
        m_inputCaps = adoptGRef(gst_caps_new_simple("audio/x-alaw", "rate", G_TYPE_INT, config.sampleRate, "channels", G_TYPE_INT, config.numberOfChannels, nullptr));
    else if (codecName == "ulaw"_s)
        m_inputCaps = adoptGRef(gst_caps_new_simple("audio/x-mulaw", "rate", G_TYPE_INT, config.sampleRate, "channels", G_TYPE_INT, config.numberOfChannels, nullptr));
    else if (codecName == "flac"_s) {
        m_header = wrapSpanData(config.description);
        if (!m_header) {
            GST_WARNING("Decoder config description for flac codec is mandatory");
            return;
        }
        parser = "flacparse";
        m_inputCaps = adoptGRef(gst_caps_new_empty_simple("audio/x-flac"));
    } else if (codecName == "vorbis"_s) {
        m_header = wrapSpanData(config.description);
        if (!m_header) {
            GST_WARNING("Decoder config description for vorbis codec is mandatory");
            return;
        }
        parser = "oggparse";
        m_inputCaps = adoptGRef(gst_caps_new_empty_simple("application/ogg"));
    } else if (codecName.startsWith("pcm-"_s)) {
        auto components = codecName.split('-');
        auto pcmFormat = components[1].convertToASCIILowercase();
        GstAudioFormat gstPcmFormat = GST_AUDIO_FORMAT_UNKNOWN;
        if (pcmFormat == "u8"_s)
            gstPcmFormat = GST_AUDIO_FORMAT_U8;
        else if (pcmFormat == "s16"_s)
            gstPcmFormat = GST_AUDIO_FORMAT_S16;
        else if (pcmFormat == "s24"_s)
            gstPcmFormat = GST_AUDIO_FORMAT_S24;
        else if (pcmFormat == "s32"_s)
            gstPcmFormat = GST_AUDIO_FORMAT_S32;
        else if (pcmFormat == "f32"_s)
            gstPcmFormat = GST_AUDIO_FORMAT_F32;
        else {
            GST_WARNING("Invalid LPCM codec format: %s", pcmFormat.ascii().data());
            return;
        }
        m_inputCaps = adoptGRef(gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, gst_audio_format_to_string(gstPcmFormat),
            "rate", G_TYPE_INT, config.sampleRate, "channels", G_TYPE_INT, config.numberOfChannels,
            "layout", G_TYPE_STRING, "interleaved", nullptr));
        parser = "rawaudioparse";
    } else
        return;

    configureAudioDecoderForHarnessing(element);

    GRefPtr<GstElement> harnessedElement;
    bool isParserRequired = false;
    if (element) {
        auto* factory = gst_element_get_factory(element.get());
        isParserRequired = !gst_element_factory_can_sink_all_caps(factory, m_inputCaps.get());
    }
    if (!g_strcmp0(parser, "rawaudioparse")) {
        harnessedElement = makeGStreamerElement(parser, nullptr);
        if (!harnessedElement) {
            GST_WARNING_OBJECT(element.get(), "Required parser %s not found", parser);
            m_inputCaps.clear();
            return;
        }
    } else if (parser && isParserRequired) {
        // The decoder won't accept the input caps, so put a parser in front.
        auto* parserElement = makeGStreamerElement(parser, nullptr);
        if (!parserElement) {
            GST_WARNING_OBJECT(element.get(), "Required parser %s not found, decoding will fail", parser);
            m_inputCaps.clear();
            return;
        }
        harnessedElement = gst_bin_new(nullptr);
        gst_bin_add_many(GST_BIN_CAST(harnessedElement.get()), parserElement, element.get(), nullptr);
        gst_element_link(parserElement, element.get());
        auto sinkPad = adoptGRef(gst_element_get_static_pad(parserElement, "sink"));
        gst_element_add_pad(harnessedElement.get(), gst_ghost_pad_new("sink", sinkPad.get()));
        auto srcPad = adoptGRef(gst_element_get_static_pad(element.get(), "src"));
        gst_element_add_pad(harnessedElement.get(), gst_ghost_pad_new("src", srcPad.get()));
    } else
        harnessedElement = WTFMove(element);

    m_harness = GStreamerElementHarness::create(WTFMove(harnessedElement), [weakThis = ThreadSafeWeakPtr { *this }, this](auto&, GRefPtr<GstSample>&& outputSample) {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis)
            return;
        if (m_isClosed)
            return;

        auto outputBuffer = gst_sample_get_buffer(outputSample.get());
        if (GST_BUFFER_FLAG_IS_SET(outputBuffer, GST_BUFFER_FLAG_DECODE_ONLY))
            return;

        if (!gst_buffer_n_memory(outputBuffer))
            return;

        static std::once_flag onceFlag;
        std::call_once(onceFlag, [this] {
            m_harness->dumpGraph("audio-decoder"_s);
        });

        GST_TRACE_OBJECT(m_harness->element(), "Got frame with PTS: %" GST_TIME_FORMAT, GST_TIME_ARGS(GST_BUFFER_PTS(outputBuffer)));

        auto data = PlatformRawAudioDataGStreamer::create(WTFMove(outputSample));
        m_outputCallback(AudioDecoder::DecodedData { WTFMove(data) });
    });
}

Ref<AudioDecoder::DecodePromise> GStreamerInternalAudioDecoder::decode(std::span<const uint8_t> frameData, [[maybe_unused]] bool isKeyFrame, int64_t timestamp, std::optional<uint64_t> duration)
{
    GST_DEBUG_OBJECT(m_harness->element(), "Decoding%s frame", isKeyFrame ? " key" : "");

    auto encodedData = wrapSpanData(frameData);
    if (!encodedData)
        return AudioDecoder::DecodePromise::createAndReject("Empty frame"_s);

    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    if (timestamp < 0)
        segment.rate = -1.0;

    if (m_header) {
        GST_DEBUG_OBJECT(m_harness->element(), "Pushing initial header");
        m_harness->start(GRefPtr<GstCaps>(m_inputCaps), &segment);
        m_harness->pushBuffer(WTFMove(m_header));
    }

    GST_BUFFER_PTS(encodedData.get()) = abs(timestamp) * 1000;
    if (duration)
        GST_BUFFER_DURATION(encodedData.get()) = *duration;

    auto result = m_harness->pushSample(adoptGRef(gst_sample_new(encodedData.get(), m_inputCaps.get(), &segment, nullptr)));
    if (!result)
        return AudioDecoder::DecodePromise::createAndReject("Decode error"_s);

    m_harness->processOutputSamples();
    return AudioDecoder::DecodePromise::createAndResolve();
}

void GStreamerInternalAudioDecoder::flush()
{
    if (m_isClosed) {
        GST_DEBUG_OBJECT(m_harness->element(), "Decoder closed, nothing to flush");
        return;
    }

    auto buffer = adoptGRef(gst_buffer_new());
    GST_BUFFER_FLAG_SET(buffer.get(), GST_BUFFER_FLAG_DISCONT);
    m_harness->pushBuffer(WTFMove(buffer));

    m_harness->flushBuffers();
}

#undef GST_CAT_DEFAULT

} // namespace WebCore

#endif // ENABLE(WEB_CODECS) && USE(GSTREAMER)
