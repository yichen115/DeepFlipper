"""Normalize generated speech with bounded gain and explicit peak headroom."""
import numpy as np


def normalize_speech(samples, target_dbfs=-20.0, max_gain_db=16.0, peak_dbfs=-1.0):
    pcm = np.asarray(samples, dtype=np.float32)
    if not pcm.size or not np.isfinite(pcm).all():
        raise ValueError('Invalid generated speech samples')
    peak = float(np.max(np.abs(pcm)))
    rms = float(np.sqrt(np.mean(pcm.astype(np.float64) ** 2)))
    # Do not amplify numerical residue or a silent utterance into noise.
    if rms < 1e-5 or peak < 1e-5:
        return np.zeros_like(pcm)
    gain = min(10 ** (target_dbfs / 20) / rms, 10 ** (max_gain_db / 20),
               10 ** (peak_dbfs / 20) / peak)
    return (pcm * gain).astype(np.float32)
