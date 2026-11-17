// JuceHeader.h - minimal stand-in for the parts of JUCE that Vital's sound engine touches.
//
// Part of Warpline (GPLv3, see LICENSE). The MPC edition does not use JUCE: the engine only needs
// a few macros plus small String / Base64 / MemoryOutputStream helpers used when wavetables stored
// inside .vital patches are decoded. Everything here is plain C++17.

#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <memory>
#include <string>
#include <vector>

#define JUCE_LEAK_DETECTOR(Class)
#define JUCE_DECLARE_NON_COPYABLE(Class) \
  Class(const Class&) = delete;          \
  Class& operator=(const Class&) = delete;
#define JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Class) JUCE_DECLARE_NON_COPYABLE(Class)

#ifndef jassert
  #define jassert(x) ((void)0)
#endif
#ifndef jassertfalse
  #define jassertfalse ((void)0)
#endif

namespace ProjectInfo {
  constexpr const char* versionString = "1.5.5";
}

// A tiny subset of juce::String.
class String {
  public:
    String() = default;
    String(const char* s) : s_(s ? s : "") { }
    String(const std::string& s) : s_(s) { }

    const std::string& toStdString() const { return s_; }
    bool isEmpty() const { return s_.empty(); }
    int length() const { return static_cast<int>(s_.size()); }

    void trim() {
      size_t a = s_.find_first_not_of(" \t\r\n");
      size_t b = s_.find_last_not_of(" \t\r\n");
      s_ = (a == std::string::npos) ? std::string() : s_.substr(a, b - a + 1);
    }

    String upToFirstOccurrenceOf(const char* sub, bool include, bool) const {
      size_t p = s_.find(sub);
      if (p == std::string::npos) return *this;
      return String(s_.substr(0, include ? p + std::strlen(sub) : p));
    }

    String upToLastOccurrenceOf(const char* sub, bool include, bool) const {
      size_t p = s_.rfind(sub);
      if (p == std::string::npos) return *this;
      return String(s_.substr(0, include ? p + std::strlen(sub) : p));
    }

    String fromFirstOccurrenceOf(const char* sub, bool include, bool) const {
      size_t p = s_.find(sub);
      if (p == std::string::npos) return String();
      return String(s_.substr(include ? p : p + std::strlen(sub)));
    }

    bool containsOnly(const char* chars) const { return s_.find_first_not_of(chars) == std::string::npos; }
    int getIntValue() const { return s_.empty() ? 0 : std::atoi(s_.c_str()); }

    bool operator==(const String& o) const { return s_ == o.s_; }
    bool operator!=(const String& o) const { return s_ != o.s_; }
    String operator+(const String& o) const { return String(s_ + o.s_); }

  private:
    std::string s_;
};

// A growable byte buffer, enough for Base64 decoding.
class MemoryOutputStream {
  public:
    MemoryOutputStream() = default;
    explicit MemoryOutputStream(size_t initial) { data_.reserve(initial); }

    void write(const void* d, size_t n) {
      const uint8_t* b = static_cast<const uint8_t*>(d);
      data_.insert(data_.end(), b, b + n);
    }
    void writeByte(uint8_t b) { data_.push_back(b); }
    const void* getData() const { return data_.empty() ? nullptr : data_.data(); }
    size_t getDataSize() const { return data_.size(); }

  private:
    std::vector<uint8_t> data_;
};

// Standard Base64 (RFC 4648), as written by JUCE's Base64::toBase64.
struct Base64 {
  static String toBase64(const void* source, size_t size) {
    static const char* kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t* in = static_cast<const uint8_t*>(source);
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < size; i += 3) {
      uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
      out += kChars[(v >> 18) & 63]; out += kChars[(v >> 12) & 63];
      out += kChars[(v >> 6) & 63];  out += kChars[v & 63];
    }
    if (i < size) {
      uint32_t v = uint32_t(in[i]) << 16;
      if (i + 1 < size) v |= uint32_t(in[i + 1]) << 8;
      out += kChars[(v >> 18) & 63]; out += kChars[(v >> 12) & 63];
      out += (i + 1 < size) ? kChars[(v >> 6) & 63] : '=';
      out += '=';
    }
    return String(out);
  }

  static bool convertFromBase64(MemoryOutputStream& out, const std::string& text) {
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
      int v;
      if (c >= 'A' && c <= 'Z') v = c - 'A';
      else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
      else if (c >= '0' && c <= '9') v = c - '0' + 52;
      else if (c == '+') v = 62;
      else if (c == '/') v = 63;
      else if (c == '=') break;
      else continue;
      acc = (acc << 6) | uint32_t(v);
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out.writeByte(uint8_t((acc >> bits) & 0xFF));
      }
    }
    return true;
  }
  static bool convertFromBase64(MemoryOutputStream& out, const String& text) {
    return convertFromBase64(out, text.toStdString());
  }
};

// The MPC SDK framework already flushes denormals around every process() call.
struct FloatVectorOperations {
  static void disableDenormalisedNumberSupport() { }
};
