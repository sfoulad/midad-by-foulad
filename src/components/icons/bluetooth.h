#pragma once
#include <cstdint>

// Lucide "bluetooth" glyph (24x24 viewBox: m7 7 10 10-5 5V2l5 5L7 17), rasterized
// at 20x20 with a 10x supersample + 2.6px stroke -- sized to match the header's
// battery indicator (16x12) rather than a smaller attempt that read as a blur
// through a phone camera on real e-ink. 1bpp, MSB-first, bit 0 = ink -- matches
// GfxRenderer::drawIcon's format.
//
// Pre-rotated 90 deg CCW before packing to cancel out GfxRenderer::drawIcon's own
// internal (size-1-row, col) mapping (screen = rotate(bitmap, 90 CW); see its
// comment in GfxRenderer.cpp) -- without this the glyph renders sideways. Pixel
// data carried forward from the BLE-R2 audit's stale-branch source (proven
// correct via multiple rounds of real-device photo verification, including this
// exact rotation fix); only this header's own commentary is new.
// size: 20x20
static const uint8_t BluetoothIcon[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xF9, 0xF9, 0xFF, 0xF0, 0xF0, 0xFF, 0xE0, 0x60, 0x7F, 0xC6, 0x06, 0x3F, 0x80, 0x00, 0x1F,
    0x80, 0x00, 0x1F, 0xFE, 0x07, 0xFF, 0xFC, 0x63, 0xFF, 0xF8, 0xF1, 0xFF, 0xF9, 0xF9, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
