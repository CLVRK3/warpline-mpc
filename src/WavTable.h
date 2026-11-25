// WavTable.h - reads a .wav file as a wavetable (Warpline 1.4: your own tables in <drive>/Warpline/Wavetables).
//
// Serum-style wavetables are WAV files of back-to-back single cycles, 2048 samples each, often with a "clm " chunk
// ("<!>2048 ...") that names the cycle length. Without one: a file that divides into 2048-sample cycles is read as
// those, and a short file (up to 4096 samples) as one cycle. PCM 8/16/24/32-bit and 32-bit float, any channel count
// (the first channel is used). Part of Warpline. GPLv3 (see LICENSE).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace wavtable {

  struct Table {
    std::vector<float> samples;   // frames * frame_size, normalised to a peak of 1
    int frame_size = 0;
    int frames() const { return frame_size > 0 ? static_cast<int>(samples.size()) / frame_size : 0; }
  };

  constexpr int kMaxFrames = 256;
  constexpr size_t kMaxFileBytes = 16u * 1024u * 1024u;

  inline uint32_t le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
  inline uint16_t le16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

  // Parses WAV bytes. Returns false (and an empty table) if they are not a usable wavetable.
  inline bool parse(const std::vector<unsigned char>& b, Table& out) {
    out = Table();
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0)
      return false;
    int format = 0, channels = 0, bits = 0, clm = 0;
    const unsigned char* data = nullptr;
    size_t data_size = 0;
    size_t pos = 12;
    while (pos + 8 <= b.size()) {
      const unsigned char* c = b.data() + pos;
      size_t size = le32(c + 4);
      size_t body = pos + 8;
      size_t avail = std::min(size, b.size() - body);
      if (std::memcmp(c, "fmt ", 4) == 0 && avail >= 16) {
        format = le16(c + 8);
        channels = le16(c + 10);
        bits = le16(c + 22);
        if (format == 0xFFFE && avail >= 26)   // WAVE_FORMAT_EXTENSIBLE: the sub-format's first two bytes
          format = le16(c + 8 + 24);
      }
      else if (std::memcmp(c, "clm ", 4) == 0 && avail >= 4) {
        std::string s(reinterpret_cast<const char*>(c + 8), std::min<size_t>(avail, 32));
        size_t k = s.find("<!>");
        if (k != std::string::npos)
          clm = std::atoi(s.c_str() + k + 3);
      }
      else if (std::memcmp(c, "data", 4) == 0) {
        data = c + 8;
        data_size = avail;
      }
      pos = body + size + (size & 1);
    }
    if (!data || channels < 1 || (format != 1 && format != 3))
      return false;
    int bytes = bits / 8;
    if ((format == 1 && (bits != 8 && bits != 16 && bits != 24 && bits != 32)) || (format == 3 && bits != 32))
      return false;
    size_t count = data_size / (static_cast<size_t>(bytes) * channels);
    if (count < 2)
      return false;

    std::vector<float> mono(count);
    for (size_t i = 0; i < count; ++i) {
      const unsigned char* p = data + i * bytes * channels;
      float v = 0.0f;
      if (format == 3) { float f; std::memcpy(&f, p, 4); v = f; }
      else if (bits == 8) v = (p[0] - 128) / 128.0f;
      else if (bits == 16) v = static_cast<int16_t>(le16(p)) / 32768.0f;
      else if (bits == 24) v = static_cast<int32_t>((p[0] << 8) | (p[1] << 16) | (static_cast<uint32_t>(p[2]) << 24)) / 2147483648.0f;
      else v = static_cast<int32_t>(le32(p)) / 2147483648.0f;
      mono[i] = std::isfinite(v) ? v : 0.0f;
    }

    int frame = 0;
    if (clm >= 16 && clm <= 65536 && count >= static_cast<size_t>(clm))
      frame = clm;
    else if (count % 2048 == 0)
      frame = 2048;
    else if (count <= 4096)
      frame = static_cast<int>(count);
    else
      return false;   // not a wavetable layout we can tell
    size_t frames = std::min<size_t>(count / frame, kMaxFrames);
    mono.resize(frames * frame);

    float peak = 0.0f;
    for (float v : mono) peak = std::max(peak, std::fabs(v));
    if (peak < 1e-6f)
      return false;
    for (float& v : mono) v /= peak;
    out.samples = std::move(mono);
    out.frame_size = frame;
    return true;
  }

  inline bool load(const std::string& path, Table& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
      return false;
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size <= 0 || static_cast<size_t>(size) > kMaxFileBytes)
      return false;
    in.seekg(0);
    std::vector<unsigned char> b(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(b.data()), size);
    return in && parse(b, out);
  }

} // namespace wavtable
