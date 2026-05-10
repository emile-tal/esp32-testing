import wave

# -------- PARAMETERS (match your ESP config) --------
SAMPLE_RATE = 16000
CHANNELS = 1
BITS_PER_SAMPLE = 16

INPUT_PCM = "recording.pcm"
OUTPUT_WAV = "recording.wav"

# -----------------------------------------------------

with open(INPUT_PCM, "rb") as f:
    pcm_data = f.read()

print(f"Read {len(pcm_data)} bytes from {INPUT_PCM}")

with wave.open(OUTPUT_WAV, "wb") as wf:
    wf.setnchannels(CHANNELS)
    wf.setsampwidth(BITS_PER_SAMPLE // 8)
    wf.setframerate(SAMPLE_RATE)
    wf.writeframes(pcm_data)

print(f"Wrote {OUTPUT_WAV}")
