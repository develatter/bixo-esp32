"""Minimal ctypes binding for libopus (encode/decode of mono 16-bit PCM)."""

import ctypes
import ctypes.util

import numpy as np

_APPLICATION_VOIP = 2048
_APPLICATION_AUDIO = 2049
_MAX_PACKET = 4000

_CANDIDATES = (
    "/opt/homebrew/lib/libopus.dylib",
    "/usr/local/lib/libopus.dylib",
    "libopus.so.0",
)


def _load() -> ctypes.CDLL:
    found = ctypes.util.find_library("opus")
    for name in ([found] if found else []) + list(_CANDIDATES):
        try:
            return ctypes.CDLL(name)
        except OSError:
            continue
    raise OSError("libopus not found (brew install opus / apt install libopus0)")


_lib = _load()
_lib.opus_encoder_create.restype = ctypes.c_void_p
_lib.opus_encoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
_lib.opus_encode.restype = ctypes.c_int32
_lib.opus_encode.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int32]
_lib.opus_encoder_destroy.argtypes = [ctypes.c_void_p]
_lib.opus_decoder_create.restype = ctypes.c_void_p
_lib.opus_decoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
_lib.opus_decode.restype = ctypes.c_int
_lib.opus_decode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int32, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
_lib.opus_decoder_destroy.argtypes = [ctypes.c_void_p]
_lib.opus_strerror.restype = ctypes.c_char_p


def _check(code: int) -> int:
    if code < 0:
        raise RuntimeError(f"opus: {_lib.opus_strerror(code).decode()}")
    return code


class Encoder:
    def __init__(self, sample_rate: int, frame_ms: int = 60, voice: bool = False) -> None:
        err = ctypes.c_int()
        app = _APPLICATION_VOIP if voice else _APPLICATION_AUDIO
        self._st = _lib.opus_encoder_create(sample_rate, 1, app, ctypes.byref(err))
        _check(err.value)
        self.frame_samples = sample_rate * frame_ms // 1000
        self._out = ctypes.create_string_buffer(_MAX_PACKET)

    def encode(self, pcm: np.ndarray) -> bytes:
        """Encode exactly one frame of int16 samples."""
        frame = np.ascontiguousarray(pcm, dtype=np.int16)
        n = _check(_lib.opus_encode(self._st, frame.ctypes.data, self.frame_samples, self._out, _MAX_PACKET))
        return self._out.raw[:n]

    def __del__(self) -> None:
        if getattr(self, "_st", None):
            _lib.opus_encoder_destroy(self._st)


class Decoder:
    def __init__(self, sample_rate: int) -> None:
        err = ctypes.c_int()
        self._st = _lib.opus_decoder_create(sample_rate, 1, ctypes.byref(err))
        _check(err.value)
        # 120 ms is the longest Opus frame.
        self._pcm = np.zeros(sample_rate * 120 // 1000, dtype=np.int16)

    def decode(self, packet: bytes) -> np.ndarray:
        n = _check(_lib.opus_decode(self._st, packet, len(packet), self._pcm.ctypes.data, len(self._pcm), 0))
        return self._pcm[:n].copy()

    def __del__(self) -> None:
        if getattr(self, "_st", None):
            _lib.opus_decoder_destroy(self._st)
