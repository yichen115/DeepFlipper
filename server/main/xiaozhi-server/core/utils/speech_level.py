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


class StreamLeveler:
    """Even out streamed cloud speech without hard clipping.

    Streamed TTS arrives in arbitrary chunks, so whole-utterance normalization is not
    possible. Instead the gain follows the recent speech loudness (silence is ignored)
    towards the same -20 dBFS RMS that local MeloTTS is normalized to, moving slowly so
    single syllables are not pumped. Peaks pass a soft knee instead of being clipped,
    which is what made a fixed x1.5 gain sound sudden and harsh.

    One instance lives per TTS provider (per device connection), so the gain carries
    over between sentences and turns instead of restarting at a guess every time.
    """

    def __init__(self, rate=24000, target_dbfs=-20.0, max_gain_db=12.0, min_gain_db=-12.0,
                 initial_gain_db=3.5, gate_dbfs=-42.0, window_s=0.6, attack_s=0.08, release_s=0.9,
                 knee_dbfs=-6.0, ceiling_dbfs=-1.0):
        self.block = max(1, int(rate * 0.02))
        self.target = 10 ** (target_dbfs / 20)
        self.max_gain = 10 ** (max_gain_db / 20)
        self.min_gain = 10 ** (min_gain_db / 20)
        self.gate = 10 ** (gate_dbfs / 20)
        self.knee = 10 ** (knee_dbfs / 20)
        self.ceiling = 10 ** (ceiling_dbfs / 20)
        block_s = self.block / rate
        self.energy_alpha = 1 - np.exp(-block_s / window_s)
        self.attack = 1 - np.exp(-block_s / attack_s)
        self.release = 1 - np.exp(-block_s / release_s)
        self.gain = 10 ** (initial_gain_db / 20)
        self.energy = None          # running mean square of speech blocks (full scale = 1)

    def _limit(self, x):
        """Soft knee: linear below the knee, smoothly approaches the ceiling above it."""
        magnitude = np.abs(x)
        over = magnitude > self.knee
        if over.any():
            room = self.ceiling - self.knee
            excess = (magnitude[over] - self.knee) / room
            x = x.copy()
            x[over] = np.sign(x[over]) * (self.knee + room * np.tanh(excess))
        return x

    def process(self, samples):
        """int16 PCM in, int16 PCM out (same length)."""
        pcm = np.asarray(samples, dtype=np.int16)
        if not pcm.size:
            return pcm
        x = pcm.astype(np.float32) / 32768.0
        out = np.empty_like(x)
        for start in range(0, x.size, self.block):
            chunk = x[start:start + self.block]
            mean_square = float(np.mean(chunk.astype(np.float64) ** 2))
            if mean_square > self.gate ** 2:
                if self.energy is None:
                    self.energy = mean_square
                else:
                    self.energy += self.energy_alpha * (mean_square - self.energy)
            old = self.gain
            if self.energy:
                wanted = min(self.max_gain, max(self.min_gain, self.target / np.sqrt(self.energy)))
                rate = self.attack if wanted < self.gain else self.release
                self.gain += rate * (wanted - self.gain)
            # Ramp across the block so gain changes never step audibly.
            ramp = np.linspace(old, self.gain, chunk.size, endpoint=False, dtype=np.float32)
            out[start:start + chunk.size] = self._limit(chunk * ramp)
        return np.round(out * 32767.0).astype(np.int16)
