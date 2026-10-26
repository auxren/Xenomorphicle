#ifndef SCOPEFFT_H_
#define SCOPEFFT_H_
// ---------------------------------------------------------------------------
// The Scope's spectrum view: a 1024-point Hann-windowed FFT, a peak readout
// and the 128 log-frequency columns the OLED draws. Portable C++ (no CMSIS)
// so the host tests run the very code the module runs; at 1024 points it is
// a fraction of a millisecond on the M7, run at redraw rate in loop context.
// Input is normalised: +-1.0 is full scale, and a full-scale sine reads 0 dB.
// ---------------------------------------------------------------------------

#include <stdint.h>

namespace ScopeFft {

constexpr int kN = 1024;
constexpr float kFloorDb = -120.f;
constexpr float kRowTopDb = 0.f;     // top of the plot
constexpr float kRowBottomDb = -90.f;

// Work buffers (~16 KB) the caller owns, so the Scope can hold them on the heap
// only while it is open. The twiddle and window tables fill on first use.
enum class Window : uint8_t { HANN = 0, BLACKMAN_HARRIS, FLATTOP, RECT };

struct Work {
  float re[kN], im[kN], cos_t[kN / 2], sin_t[kN / 2], win[kN];
  float win_scale = 0.f;        // 2 / sum(win): a sine of amplitude A reads A
  uint8_t win_kind = 255;       // which window `win` holds (255: none yet)
  bool tables = false;
};

// The mean comes off first, for every source; then the window; then the FFT.
void MagnitudeDb(const float *in /*kN*/, float *db_out /*kN/2+1*/, Work &w, Window win = Window::HANN);
// The same transform, as power (magnitude squared, same normalisation), for
// averaging: average in power, then PowerToDb.
void PowerSpectrum(const float *in /*kN*/, float *pow_out /*kN/2+1*/, Work &w, Window win = Window::HANN);
void PowerToDb(const float *pow /*kN/2+1*/, float *db_out /*kN/2+1*/);
// Strongest bin in [min_hz, max_hz], parabolic-interpolated; its level in *peak_db.
float PeakHz(const float *db, float rate_hz, float *peak_db = nullptr, float min_hz = 20.f,
             float max_hz = 1e9f);
// 20 Hz .. min(20 kHz, rate/2), log10-spaced over 128 columns; each column
// is the loudest bin it covers; where it covers none (the low end, columns narrower
// than a bin) the level is interpolated linearly in dB between the two bins around
// it, so the low end is a slope and not a row of flat blocks.
void Columns(const float *db, float rate_hz, int plot_h, uint8_t rows_out[128]);
int ColumnForHz(float hz, float rate_hz);
// A zoomed view: a LINEAR axis from 0 to span_hz over the 128 columns (the data is
// the decimated stream's spectrum at rate_hz). Same rules as Columns().
void ColumnsLinear(const float *db, float rate_hz, float span_hz, int plot_h, uint8_t rows_out[128]);
int ColumnForHzLinear(float hz, float span_hz);
// Below this the strongest bin is the noise floor or filter leakage, not a signal: the header
// says "pk --" rather than a frequency (a -1 dBFS tone far above a zoomed span leaks at -75 dB).
constexpr float kPeakMinDb = -70.f;
// The shown span of a zoomed view: 0.8 of the decimated Nyquist (0.4 of the decimated rate),
// so nothing from the filter's transition band is drawn; it depends on the SOURCE's rate.
inline double ZoomSpanHz(double decimated_rate_hz) { return decimated_rate_hz * 4.0 / 10.0; }
// Where a zoomed view's peak search starts: 20 Hz, or three bins where the bins are finer.
inline float PeakFloorHz(float bin_hz) { return 3.f * bin_hz < 20.f ? 3.f * bin_hz : 20.f; }
// Averaging advances per fresh half frame of samples, not per redraw: true once `count` is
// at least kAvgFreshSamples past `last` (wrap-safe).
constexpr uint32_t kAvgFreshSamples = kN / 2;
inline bool AvgDue(uint32_t count, uint32_t last) { return (uint32_t)(count - last) >= kAvgFreshSamples; }

}  // namespace ScopeFft

#endif  // SCOPEFFT_H_
