"""A local reference roundtrip through the same Opus settings as TTS delivery."""
import io
import wave


def prepare_opus_audio(audio, file_type, sample_rate=24000):
    from pydub import AudioSegment
    from core.utils.opus_encoder_utils import OpusEncoderUtils
    source = AudioSegment.from_file(io.BytesIO(audio), format=file_type)
    source = source.set_channels(1).set_frame_rate(sample_rate).set_sample_width(2)
    if not 0 < len(source) <= 30000:
        raise ValueError('测试音频须在 30 秒以内')
    encoder = OpusEncoderUtils(sample_rate, 1, 60)
    packets = []
    try:
        encoder.encode_pcm_to_opus_stream(source.raw_data, True, packets.append)
    finally:
        encoder.close()
    output = io.BytesIO()
    source.export(output, format='wav')
    return output.getvalue(), packets


def opus_preview(audio, file_type, sample_rate=24000):
    _, packets = prepare_opus_audio(audio, file_type, sample_rate)
    return decode_opus_packets(packets, sample_rate)


def decode_opus_packets(packets, sample_rate=24000):
    import opuslib_next
    decoder = opuslib_next.Decoder(sample_rate, 1)
    pcm = b''.join(decoder.decode(packet, sample_rate * 60 // 1000) for packet in packets)
    output = io.BytesIO()
    with wave.open(output, 'wb') as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm)
    return output.getvalue()
