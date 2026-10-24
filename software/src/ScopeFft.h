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

void MagnitudeDb(const float *in /*kN*/, float *db_out /*kN/2+1*/);
// Strongest bin above 20 Hz, parabolic-interpolated; its level in *peak_db.
float PeakHz(const float *db, float rate_hz, float *peak_db = nullptr);
// 20 Hz .. min(20 kHz, rate/2), log10-spaced over 128 columns; each column
// is the loudest bin it covers (or the nearest bin where it covers none).
void Columns(const float *db, float rate_hz, int plot_h, uint8_t rows_out[128]);
int ColumnForHz(float hz, float rate_hz);

}  // namespace ScopeFft

#endif  // SCOPEFFT_H_
