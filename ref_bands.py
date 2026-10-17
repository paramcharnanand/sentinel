"""Python reference band features for the first frames of a WAV file.

    python ref_bands.py data/normal.wav data/ref_bands.txt [n_frames]

Writes one line of NB values per 512-sample frame, computed exactly as the
Sentinel spec states, for test_host to compare against feat_bands().
"""
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import stft

FRAME, NB = 512, 32


def bands(x):
    f, t, Z = stft(x, fs=16000, nperseg=512, noverlap=0)
    P = np.log(np.abs(Z) ** 2 + 1e-10)
    return np.array([g.mean() for g in np.array_split(P, NB, axis=0)])


def main():
    src, dst = sys.argv[1], sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 5
    fs, x = wavfile.read(src)
    if x.ndim > 1:
        x = x[:, 0]
    if x.dtype != np.int16 or fs != 16000:
        sys.exit(f"{src}: expected 16 kHz int16, got {fs} Hz {x.dtype}")
    x = x.astype(np.float64)  # raw int16 values, as feat_bands() sees them
    with open(dst, "w") as out:
        for i in range(n):
            out.write(" ".join(f"{v:.9g}" for v in bands(x[i * FRAME:(i + 1) * FRAME])) + "\n")
    print(f"wrote {n} reference frames to {dst}")


if __name__ == "__main__":
    main()
