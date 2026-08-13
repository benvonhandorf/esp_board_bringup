# System

[← Command reference](README.md) · [Project README](../README.md)

## `info`

Displays system information including chip model, RAM, and flash size, plus the
silicon revision, station MAC, firmware and ESP-IDF versions, the reason for the
last reset, and uptime.

## `reset`

Performs a soft reset of the device.

## `lfxtal`

Attempt to configure the low frequency crystal oscillator.  Report the status of the oscillator.

The 32 kHz oscillator is enabled, given time to start, and then *measured* by
calibrating it against the main crystal. A dead oscillator makes that
calibration time out, which is what distinguishes "no crystal fitted" from a
working one — `rtc_clk_slow_freq_get_hz()` cannot be used here, as it returns a
nominal 32768 for whichever source is selected regardless of reality. Only if
the measurement is close to 32.768 kHz is the RTC slow clock actually switched
over to it; otherwise the previous clock source is left alone and the failure is
reported.
