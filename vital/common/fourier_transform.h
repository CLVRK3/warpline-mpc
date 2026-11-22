/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vital.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "JuceHeader.h"

#if !INTEL_IPP && !JUCE_MODULE_AVAILABLE_juce_dsp && !__APPLE__
  // MPC edition: kissfft is included here, outside namespace vital.
  #include "kissfft/kissfft.h"
  // MPC edition: for the fast real inverse transform (FastRealInverse).
  #include <cmath>
  #include <cstring>
  #include <map>
  #include <memory>
  #include <mutex>
  #include <vector>
  #if defined(__ARM_NEON__) || defined(__ARM_NEON)
    #include <arm_neon.h>
  #elif defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    #include <xmmintrin.h>
    #include <emmintrin.h>
  #endif
#endif

namespace vital {
  #if INTEL_IPP

  #include "ipps.h"

  class FourierTransform {
    public:
      FourierTransform(int bits) : size_(1 << bits) {
        int spec_size = 0;
        int spec_buffer_size = 0;
        int buffer_size = 0;
        ippsFFTGetSize_R_32f(bits, IPP_FFT_DIV_INV_BY_N, ippAlgHintNone, &spec_size, &spec_buffer_size, &buffer_size);

        spec_ = std::make_unique<Ipp8u[]>(spec_size);
        spec_buffer_ = std::make_unique<Ipp8u[]>(spec_buffer_size);
        buffer_ = std::make_unique<Ipp8u[]>(buffer_size);

        ippsFFTInit_R_32f(&ipp_specs_, bits, IPP_FFT_DIV_INV_BY_N, ippAlgHintNone, spec_.get(), spec_buffer_.get());
      }

      void transformRealForward(float* data) {
        data[size_] = 0.0f;
        ippsFFTFwd_RToPerm_32f_I((Ipp32f*)data, ipp_specs_, buffer_.get());
        data[size_] = data[1];
        data[size_ + 1] = 0.0f;
        data[1] = 0.0f;
      }

      void transformRealInverse(float* data) {
        data[1] = data[size_];
        ippsFFTInv_PermToR_32f_I((Ipp32f*)data, ipp_specs_, buffer_.get());
        memset(data + size_, 0, size_ * sizeof(float));
      }

    private:
      int size_;
      IppsFFTSpec_R_32f *ipp_specs_;
      std::unique_ptr<Ipp8u[]> spec_;
      std::unique_ptr<Ipp8u[]> spec_buffer_;
      std::unique_ptr<Ipp8u[]> buffer_;

      JUCE_LEAK_DETECTOR(FourierTransform)
  };

  #elif JUCE_MODULE_AVAILABLE_juce_dsp

  class FourierTransform {
    public:
      FourierTransform(int bits) : fft_(bits) { }

      void transformRealForward(float* data) { fft_.performRealOnlyForwardTransform(data, true); }
      void transformRealInverse(float* data) { fft_.performRealOnlyInverseTransform(data); }

    private:
      dsp::FFT fft_;

      JUCE_LEAK_DETECTOR(FourierTransform)
  };

  #elif __APPLE__
  #define VIMAGE_H
  #include <Accelerate/Accelerate.h>

  class FourierTransform {
    public:
      FourierTransform(vDSP_Length bits) : setup_(vDSP_create_fftsetup(bits, 2)), bits_(bits), size_(1 << bits) { }
      ~FourierTransform() {
        vDSP_destroy_fftsetup(setup_);
      }

      void transformRealForward(float* data) {
        static const float kMult = 0.5f;
        data[size_] = 0.0f;
        DSPSplitComplex split = { data, data + 1 };
        vDSP_fft_zrip(setup_, &split, 2, bits_, kFFTDirection_Forward);
        vDSP_vsmul(data, 1, &kMult, data, 1, size_);

        data[size_] = data[1];
        data[size_ + 1] = 0.0f;
        data[1] = 0.0f;
      }

      void transformRealInverse(float* data) {
        float multiplier = 1.0f / size_;
        DSPSplitComplex split = { data, data + 1 };
        data[1] = data[size_];

        vDSP_fft_zrip(setup_, &split, 2, bits_, kFFTDirection_Inverse);
        vDSP_vsmul(data, 1, &multiplier, data, 1, size_ * 2);
        memset(data + size_, 0, size_ * sizeof(float));
      }

    private:
      FFTSetup setup_;
      vDSP_Length bits_;
      vDSP_Length size_;

      JUCE_LEAK_DETECTOR(FourierTransform)
  };

  #else

  // MPC edition: the real inverse transform is the oscillators' hot path (every voice rebuilds its
  // waveform with it every 7 ms), so it gets its own SIMD implementation below. kissfft stays for the
  // forward transform (wavetable creation, not on the audio path) and for sizes the fast path doesn't cover.
  namespace fft_simd {
  #if defined(__ARM_NEON__) || defined(__ARM_NEON)
    typedef float32x4_t v4;
    static inline v4 load(const float* p) { return vld1q_f32(p); }
    static inline void store(float* p, v4 v) { vst1q_f32(p, v); }
    static inline v4 add(v4 a, v4 b) { return vaddq_f32(a, b); }
    static inline v4 sub(v4 a, v4 b) { return vsubq_f32(a, b); }
    static inline v4 mul(v4 a, v4 b) { return vmulq_f32(a, b); }
    static inline v4 dup(float x) { return vdupq_n_f32(x); }
    // Lanes in reverse order.
    static inline v4 rev(v4 v) { v4 r = vrev64q_f32(v); return vcombine_f32(vget_high_f32(r), vget_low_f32(r)); }
    // Four interleaved complex numbers (re, im, re, im ...) into a real and an imaginary vector, and back.
    static inline void load2(const float* p, v4& re, v4& im) { float32x4x2_t t = vld2q_f32(p); re = t.val[0]; im = t.val[1]; }
    static inline void store2(float* p, v4 re, v4 im) { float32x4x2_t t; t.val[0] = re; t.val[1] = im; vst2q_f32(p, t); }
    // p[4 * i + r] = lane i of vector r.
    static inline void store4(float* p, v4 a, v4 b, v4 c, v4 d) {
      float32x4x4_t t; t.val[0] = a; t.val[1] = b; t.val[2] = c; t.val[3] = d; vst4q_f32(p, t);
    }
  #elif defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    typedef __m128 v4;
    static inline v4 load(const float* p) { return _mm_loadu_ps(p); }
    static inline void store(float* p, v4 v) { _mm_storeu_ps(p, v); }
    static inline v4 add(v4 a, v4 b) { return _mm_add_ps(a, b); }
    static inline v4 sub(v4 a, v4 b) { return _mm_sub_ps(a, b); }
    static inline v4 mul(v4 a, v4 b) { return _mm_mul_ps(a, b); }
    static inline v4 dup(float x) { return _mm_set1_ps(x); }
    static inline v4 rev(v4 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 1, 2, 3)); }
    static inline void load2(const float* p, v4& re, v4& im) {
      v4 a = _mm_loadu_ps(p), b = _mm_loadu_ps(p + 4);
      re = _mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0));
      im = _mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1));
    }
    static inline void store2(float* p, v4 re, v4 im) {
      _mm_storeu_ps(p, _mm_unpacklo_ps(re, im));
      _mm_storeu_ps(p + 4, _mm_unpackhi_ps(re, im));
    }
    static inline void store4(float* p, v4 a, v4 b, v4 c, v4 d) {
      _MM_TRANSPOSE4_PS(a, b, c, d);
      _mm_storeu_ps(p, a); _mm_storeu_ps(p + 4, b); _mm_storeu_ps(p + 8, c); _mm_storeu_ps(p + 12, d);
    }
  #else
    struct v4 { float f[4]; };
    static inline v4 load(const float* p) { v4 r; for (int i = 0; i < 4; ++i) r.f[i] = p[i]; return r; }
    static inline void store(float* p, v4 v) { for (int i = 0; i < 4; ++i) p[i] = v.f[i]; }
    static inline v4 add(v4 a, v4 b) { for (int i = 0; i < 4; ++i) a.f[i] += b.f[i]; return a; }
    static inline v4 sub(v4 a, v4 b) { for (int i = 0; i < 4; ++i) a.f[i] -= b.f[i]; return a; }
    static inline v4 mul(v4 a, v4 b) { for (int i = 0; i < 4; ++i) a.f[i] *= b.f[i]; return a; }
    static inline v4 dup(float x) { v4 r; for (int i = 0; i < 4; ++i) r.f[i] = x; return r; }
    static inline v4 rev(v4 v) { v4 r; for (int i = 0; i < 4; ++i) r.f[i] = v.f[3 - i]; return r; }
    static inline void load2(const float* p, v4& re, v4& im) { for (int i = 0; i < 4; ++i) { re.f[i] = p[2 * i]; im.f[i] = p[2 * i + 1]; } }
    static inline void store2(float* p, v4 re, v4 im) { for (int i = 0; i < 4; ++i) { p[2 * i] = re.f[i]; p[2 * i + 1] = im.f[i]; } }
    static inline void store4(float* p, v4 a, v4 b, v4 c, v4 d) {
      for (int i = 0; i < 4; ++i) { p[4 * i] = a.f[i]; p[4 * i + 1] = b.f[i]; p[4 * i + 2] = c.f[i]; p[4 * i + 3] = d.f[i]; }
    }
  #endif
  } // namespace fft_simd

  // The real inverse transform of desktop Vital (its JUCE, IPP and vDSP backends), for sizes N = 2 * 4^k
  // (N = 2048 for the oscillators). `data` holds bins 0..N/2-1 as (re, im) pairs, with the Nyquist bin's real
  // part at data[N]:
  //   out[n] = (1 / N) * (X[0] + X[N/2] (-1)^n + 2 Re(sum over k = 1..N/2-1 of X[k] e^(+2 pi i k n / N))),
  // with X[0] = data[0], X[N/2] = data[N] and X[k] = (data[2k], data[2k+1]). Nothing else in the buffer is read.
  // (The kissfft code below, used before, also read the upper half of the buffer as bins N/2+1..N-1, which
  // desktop Vital ignores; morphs that keep data there, such as Inharmonic Scale, sounded different because
  // of it. -DWARPLINE_KISSFFT_INVERSE=1 still builds that old path.)
  // The spectrum Z (Z[0] = X[0], Z[N/2] = X[N/2], Z[k] = X[k], Hermitian) is folded into N/2 complex points,
  //   C[k] = E[k] + i O[k],  E[k] = Z[k] + conj(Z[N/2 - k]),  O[k] = (Z[k] - conj(Z[N/2 - k])) e^(+2 pi i k / N),
  // whose inverse transform c gives out[2m] = Re(c[m]) / N and out[2m + 1] = Im(c[m]) / N.
  // The N/2-point transform is a radix-4 Stockham transform on split real/imaginary arrays, four
  // butterflies per SIMD operation. No allocation after construction; the scratch arrays (4 x N/2 floats)
  // belong to the instance, the twiddle tables are shared.
  class FastRealInverse {
    public:
      static bool supports(size_t bits) { return bits >= 5 && ((bits - 1) % 2) == 0; }

      explicit FastRealInverse(size_t bits) : n_(static_cast<int>(1) << bits), h_(n_ / 2) {
        tables_ = &tablesFor(bits);
        for (int i = 0; i < 4; ++i)
          scratch_[i] = std::make_unique<float[]>(h_);
      }

      void transform(float* data) {
        using namespace fft_simd;
        const int n = n_, h = h_;
        float* cre = scratch_[0].get();
        float* cim = scratch_[1].get();
        const float* wkr = tables_->wk_re.data();
        const float* wki = tables_->wk_im.data();

        // k = 0: E and D are real (DC and Nyquist).
        {
          float dc = data[0], nyquist = data[n];
          cre[0] = dc + nyquist;
          cim[0] = dc - nyquist;
        }
        // k = 1..3 one at a time.
        for (int k = 1; k < 4; ++k) {
          float zkr = data[2 * k], zki = data[2 * k + 1];               // Z[k]
          float zhr = data[2 * (h - k)], zhi = data[2 * (h - k) + 1];   // Z[H-k]
          float er = zkr + zhr, ei = zki - zhi;                          // E = Z[k] + conj(Z[H-k])
          float dr = zkr - zhr, di = zki + zhi;                          // D = Z[k] - conj(Z[H-k])
          float orr = dr * wkr[k] - di * wki[k], oi = dr * wki[k] + di * wkr[k];
          cre[k] = er - oi;
          cim[k] = ei + orr;
        }
        for (int k = 4; k < h; k += 4) {
          v4 zkr, zki, zhr, zhi;
          load2(data + 2 * k, zkr, zki);
          load2(data + 2 * (h - k - 3), zhr, zhi); zhr = rev(zhr); zhi = rev(zhi);
          v4 er = add(zkr, zhr), ei = sub(zki, zhi);   // E = Z[k] + conj(Z[H-k])
          v4 dr = sub(zkr, zhr), di = add(zki, zhi);   // D = Z[k] - conj(Z[H-k])
          v4 wr = load(wkr + k), wi = load(wki + k);
          v4 orr = sub(mul(dr, wr), mul(di, wi)), oi = add(mul(dr, wi), mul(di, wr));
          store(cre + k, sub(er, oi));
          store(cim + k, add(ei, orr));
        }

        const float* rre = nullptr;
        const float* rim = nullptr;
        inverseComplex(cre, cim, scratch_[2].get(), scratch_[3].get(), rre, rim);

        const v4 scale = dup(1.0f / n);
        for (int m = 0; m < h; m += 4)
          store2(data + 2 * m, mul(load(rre + m), scale), mul(load(rim + m), scale));
        memset(data + n, 0, n * sizeof(float));
      }

    private:
      struct Stage { int n, s; std::vector<float> w1r, w1i, w2r, w2i, w3r, w3i; };
      struct Tables { std::vector<Stage> stages; std::vector<float> wk_re, wk_im; };

      static Tables makeTables(size_t bits) {
        Tables t;
        const int n = static_cast<int>(1) << bits, h = n / 2;
        const double two_pi = 6.283185307179586476925286766559;
        t.wk_re.resize(h);
        t.wk_im.resize(h);
        for (int k = 0; k < h; ++k) {
          t.wk_re[k] = static_cast<float>(std::cos(two_pi * k / n));
          t.wk_im[k] = static_cast<float>(std::sin(two_pi * k / n));
        }
        for (int len = h, s = 1; len >= 4; len /= 4, s *= 4) {
          Stage st;
          st.n = len; st.s = s;
          const int m = len / 4;
          for (int p = 0; p < m; ++p) {
            double a = two_pi * p / len;
            st.w1r.push_back(static_cast<float>(std::cos(a)));     st.w1i.push_back(static_cast<float>(std::sin(a)));
            st.w2r.push_back(static_cast<float>(std::cos(2 * a))); st.w2i.push_back(static_cast<float>(std::sin(2 * a)));
            st.w3r.push_back(static_cast<float>(std::cos(3 * a))); st.w3i.push_back(static_cast<float>(std::sin(3 * a)));
          }
          t.stages.push_back(std::move(st));
        }
        return t;
      }

      static const Tables& tablesFor(size_t bits) {
        // Only the oscillator size is used in practice; other supported sizes get their own table once.
        static const Tables t11 = makeTables(11);
        if (bits == 11)
          return t11;
        static std::mutex lock;
        static std::map<size_t, std::unique_ptr<Tables>> others;
        std::lock_guard<std::mutex> guard(lock);
        auto& entry = others[bits];
        if (!entry)
          entry = std::make_unique<Tables>(makeTables(bits));
        return *entry;
      }

      // Unnormalised inverse transform of h_ complex points (split arrays). x is overwritten; the result
      // pointers end up at whichever pair of arrays holds it.
      void inverseComplex(float* xr, float* xi, float* yr, float* yi, const float*& out_r, const float*& out_i) {
        using namespace fft_simd;
        for (const Stage& st : tables_->stages) {
          const int m = st.n / 4, s = st.s;
          if (s == 1) {
            // First stage: four values of p at a time, outputs interleaved by four.
            for (int p = 0; p < m; p += 4) {
              v4 ar = load(xr + p), ai = load(xi + p);
              v4 br = load(xr + p + m), bi = load(xi + p + m);
              v4 cr = load(xr + p + 2 * m), ci = load(xi + p + 2 * m);
              v4 dr = load(xr + p + 3 * m), di = load(xi + p + 3 * m);
              v4 y0r, y0i, y1r, y1i, y2r, y2i, y3r, y3i;
              butterfly(ar, ai, br, bi, cr, ci, dr, di,
                        load(st.w1r.data() + p), load(st.w1i.data() + p), load(st.w2r.data() + p),
                        load(st.w2i.data() + p), load(st.w3r.data() + p), load(st.w3i.data() + p),
                        y0r, y0i, y1r, y1i, y2r, y2i, y3r, y3i);
              store4(yr + 4 * p, y0r, y1r, y2r, y3r);
              store4(yi + 4 * p, y0i, y1i, y2i, y3i);
            }
          }
          else {
            for (int p = 0; p < m; ++p) {
              v4 w1r = dup(st.w1r[p]), w1i = dup(st.w1i[p]), w2r = dup(st.w2r[p]), w2i = dup(st.w2i[p]);
              v4 w3r = dup(st.w3r[p]), w3i = dup(st.w3i[p]);
              const int i0 = s * p, i1 = s * (p + m), i2 = s * (p + 2 * m), i3 = s * (p + 3 * m);
              const int o0 = s * 4 * p, o1 = o0 + s, o2 = o0 + 2 * s, o3 = o0 + 3 * s;
              for (int q = 0; q < s; q += 4) {
                v4 y0r, y0i, y1r, y1i, y2r, y2i, y3r, y3i;
                butterfly(load(xr + i0 + q), load(xi + i0 + q), load(xr + i1 + q), load(xi + i1 + q),
                          load(xr + i2 + q), load(xi + i2 + q), load(xr + i3 + q), load(xi + i3 + q),
                          w1r, w1i, w2r, w2i, w3r, w3i, y0r, y0i, y1r, y1i, y2r, y2i, y3r, y3i);
                store(yr + o0 + q, y0r); store(yi + o0 + q, y0i);
                store(yr + o1 + q, y1r); store(yi + o1 + q, y1i);
                store(yr + o2 + q, y2r); store(yi + o2 + q, y2i);
                store(yr + o3 + q, y3r); store(yi + o3 + q, y3i);
              }
            }
          }
          std::swap(xr, yr);
          std::swap(xi, yi);
        }
        out_r = xr;
        out_i = xi;
      }

      // Radix-4 decimation-in-frequency butterfly of the inverse (e^+i) transform.
      static inline void butterfly(fft_simd::v4 ar, fft_simd::v4 ai, fft_simd::v4 br, fft_simd::v4 bi,
                                   fft_simd::v4 cr, fft_simd::v4 ci, fft_simd::v4 dr, fft_simd::v4 di,
                                   fft_simd::v4 w1r, fft_simd::v4 w1i, fft_simd::v4 w2r, fft_simd::v4 w2i,
                                   fft_simd::v4 w3r, fft_simd::v4 w3i,
                                   fft_simd::v4& y0r, fft_simd::v4& y0i, fft_simd::v4& y1r, fft_simd::v4& y1i,
                                   fft_simd::v4& y2r, fft_simd::v4& y2i, fft_simd::v4& y3r, fft_simd::v4& y3i) {
        using namespace fft_simd;
        v4 apcr = add(ar, cr), apci = add(ai, ci), amcr = sub(ar, cr), amci = sub(ai, ci);
        v4 bpdr = add(br, dr), bpdi = add(bi, di), bmdr = sub(br, dr), bmdi = sub(bi, di);
        y0r = add(apcr, bpdr);
        y0i = add(apci, bpdi);
        // t1 = (a - c) + i (b - d),  t3 = (a - c) - i (b - d),  t2 = (a + c) - (b + d)
        v4 t1r = sub(amcr, bmdi), t1i = add(amci, bmdr);
        v4 t3r = add(amcr, bmdi), t3i = sub(amci, bmdr);
        v4 t2r = sub(apcr, bpdr), t2i = sub(apci, bpdi);
        y1r = sub(mul(t1r, w1r), mul(t1i, w1i)); y1i = add(mul(t1r, w1i), mul(t1i, w1r));
        y2r = sub(mul(t2r, w2r), mul(t2i, w2i)); y2i = add(mul(t2r, w2i), mul(t2i, w2r));
        y3r = sub(mul(t3r, w3r), mul(t3i, w3i)); y3i = add(mul(t3r, w3i), mul(t3i, w3r));
      }

      int n_, h_;
      const Tables* tables_;
      std::unique_ptr<float[]> scratch_[4];
  };

  class FourierTransform {
    public:
      FourierTransform(size_t bits) : bits_(bits), size_(1 << bits), forward_(size_, false), inverse_(size_, true) {
        buffer_ = std::make_unique<std::complex<float>[]>(size_);
        if (FastRealInverse::supports(bits))
          fast_inverse_ = std::make_unique<FastRealInverse>(bits);
      }

      ~FourierTransform() { }

      void transformRealForward(float* data) {
        for (int i = size_ - 1; i >= 0; --i) {
          data[2 * i] = data[i];
          data[2 * i + 1] = 0.0f;
        }

        forward_.transform((std::complex<float>*)data, buffer_.get());

        int num_floats = size_ * 2;
        memcpy(data, buffer_.get(), num_floats * sizeof(float));
        // MPC edition: leave the result in desktop Vital's layout (its IPP / Accelerate code): bins 0..N/2-1 as
        // (re, im) pairs, the Nyquist bin's real part at data[N], zero imaginary parts for DC and Nyquist, and
        // nothing above. Bin N/2 already sits at data[N] here; the upstream fallback overwrote it with data[1]
        // (DC's imaginary part, about 0), which dropped every wavetable frame's top harmonic.
        data[size_ + 1] = 0.0f;
        data[1] = 0.0f;
        memset(data + size_ + 2, 0, (size_ - 2) * sizeof(float));
      }

      void transformRealInverse(float* data) {
#if !WARPLINE_KISSFFT_INVERSE   // build with -DWARPLINE_KISSFFT_INVERSE=1 to compare against the old path
        if (fast_inverse_) {
          fast_inverse_->transform(data);
          return;
        }
#endif
        transformRealInverseKiss(data);
      }

      // The kissfft inverse Warpline used before (reads the whole buffer; see FastRealInverse).
      void transformRealInverseKiss(float* data) {
        data[0] *= 0.5f;
        data[1] = data[size_];
        inverse_.transform((std::complex<float>*)data, buffer_.get());
        int num_floats = size_ * 2;

        float multiplier = 2.0f / size_;
        for (int i = 0; i < size_; ++i)
          data[i] = buffer_[i].real() * multiplier;

        memset(data + size_, 0, size_ * sizeof(float));
      }

    private:
      size_t bits_;
      size_t size_;
      std::unique_ptr<std::complex<float>[]> buffer_;
      kissfft<float> forward_;
      kissfft<float> inverse_;
      std::unique_ptr<FastRealInverse> fast_inverse_;

      JUCE_LEAK_DETECTOR(FourierTransform)
  };

  #endif

  template <size_t bits>
  class FFT {
    public:
      static FourierTransform* transform() {
        static FFT<bits> instance;
        return &instance.fourier_transform_;
      }

    private:
      FFT() : fourier_transform_(bits) { }

      FourierTransform fourier_transform_;
  };

} // namespace vital
