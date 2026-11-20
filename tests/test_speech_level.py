from pathlib import Path
import sys
import unittest
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'server/main/xiaozhi-server'))
from core.utils.speech_level import normalize_speech, StreamLeveler

RATE = 24000


def tone(seconds, level, freq=220):
    t = np.arange(int(RATE * seconds)) / RATE
    return (np.sin(2 * np.pi * freq * t) * level * 32767).astype(np.int16)


def rms_db(x):
    x = np.asarray(x, dtype=np.float64) / 32768
    return 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-12)


class StreamLevelerTests(unittest.TestCase):
    def feed(self, leveler, pcm, chunk=1234):
        return np.concatenate([leveler.process(pcm[i:i + chunk]) for i in range(0, len(pcm), chunk)])

    def test_quiet_and_loud_sentences_end_up_close_without_clipping(self):
        leveler = StreamLeveler(rate=RATE)
        quiet, loud = tone(2.0, 0.06), tone(2.0, 0.5)
        out_quiet = self.feed(leveler, quiet)
        out_loud = self.feed(leveler, loud)
        # Raw difference is ~18 dB; after settling (last second) it is within a couple of dB.
        settled = abs(rms_db(out_quiet[RATE:]) - rms_db(out_loud[RATE:]))
        self.assertLess(settled, 2.5)
        self.assertLess(abs(rms_db(out_loud[RATE:]) - (-20)), 3.5)
        # Soft limiter: nothing ever reaches full scale, even at the loud onset.
        self.assertLess(int(np.max(np.abs(out_loud))), int(32767 * 10 ** (-1 / 20)) + 2)

    def test_old_fixed_gain_clipped_where_the_leveler_does_not(self):
        burst = tone(0.5, 0.95)
        clipped = np.clip(burst.astype(np.float32) * 1.5, -32768, 32767)
        self.assertGreater(int(np.sum(np.abs(clipped) >= 32767)), 1000)   # the old harsh sound
        out = StreamLeveler(rate=RATE).process(burst)
        self.assertEqual(int(np.sum(np.abs(out) >= 32767)), 0)

    def test_silence_does_not_pump_the_gain_up(self):
        leveler = StreamLeveler(rate=RATE)
        self.feed(leveler, tone(1.0, 0.3))
        energy = leveler.energy
        out = self.feed(leveler, (np.random.default_rng(1).standard_normal(RATE * 2) * 20).astype(np.int16))
        self.assertEqual(leveler.energy, energy)                    # breath noise is not measured
        self.assertLess(leveler.gain, 1.0)                          # so the gain is not pumped up
        self.assertLess(int(np.max(np.abs(out))), 400)

    def test_chunking_does_not_change_the_output(self):
        pcm = np.concatenate([tone(0.7, 0.05), tone(0.7, 0.6, 330)])
        whole = StreamLeveler(rate=RATE).process(pcm)
        pieces = self.feed(StreamLeveler(rate=RATE), pcm, chunk=480)
        self.assertTrue(np.array_equal(whole, pieces))
        self.assertEqual(len(StreamLeveler(rate=RATE).process(np.zeros(0, np.int16))), 0)


class SpeechLevelTests(unittest.TestCase):
    def test_quiet_speech_becomes_audible_with_peak_headroom(self):
        pcm=(np.sin(np.arange(24000)*2*np.pi*440/24000)*0.04).astype(np.float32)
        adjusted=normalize_speech(pcm)
        self.assertAlmostEqual(float(np.sqrt(np.mean(adjusted**2))),0.1,places=4)
        self.assertLess(float(np.max(np.abs(adjusted))),0.9)

    def test_transient_peaks_and_silence_do_not_clip_or_amplify_noise(self):
        pcm=np.ones(1000,dtype=np.float32)*0.01;pcm[0]=1
        adjusted=normalize_speech(pcm)
        self.assertLessEqual(float(np.max(np.abs(adjusted))),0.892)
        self.assertTrue(np.all(normalize_speech(np.full(100,1e-8))==0))
        with self.assertRaises(ValueError):normalize_speech(np.array([float('nan')]))

    def test_gain_limit_keeps_the_wave_shape(self):
        pcm=np.array([-0.001,0,0.001],dtype=np.float32)
        adjusted=normalize_speech(pcm)
        self.assertLessEqual(float(adjusted[-1]/pcm[-1]),10**(16/20)+1e-5)
        self.assertAlmostEqual(float(adjusted[0]),-float(adjusted[-1]))
