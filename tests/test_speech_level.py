from pathlib import Path
import sys
import unittest
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'server/main/xiaozhi-server'))
from core.utils.speech_level import normalize_speech


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
