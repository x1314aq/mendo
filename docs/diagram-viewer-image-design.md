# Diagram viewer — Ctrl+wheel zoom — design

The diagram and image viewer (`src/ui/zoom_viewer.*`) is upstream's: the window opens
from the document's top-right buttons (diagrams) or by clicking an image, decodes images
on a worker, re-reads on device loss and on file change, and pre-scales a high-quality
copy for zoomed-out views.

This document covers the one thing this fork adds to that window: **`Ctrl+wheel` zoom**.

## Why

Upstream binds the wheel to scrolling only — zoom is the top-right buttons and the
keyboard. That is fine for browsing, but when you are reading a detail at 400% and want
to nudge in or out, reaching for a button costs a pointer round-trip. Every other viewer
on this machine (browsers, image viewers, PDF readers) uses `Ctrl+wheel`, so the reflex is
already there.

## Behaviour

- `Ctrl` held + vertical wheel zooms one notch per `WHEEL_DELTA`.
- **The point under the pointer stays put.** Zooming pivots on the cursor, not the window
  centre — that is what makes wheel zoom feel like it is magnifying what you are looking at.
- Plain wheel still scrolls; `Shift+wheel` still scrolls horizontally. Nothing changes for
  users who never press `Ctrl`.
- Horizontal wheel (`WM_MOUSEHWHEEL`) is never zoom, even with `Ctrl`.
- The zoom steps are the same ladder as the buttons and keyboard (`zoom_viewer::NextZoom`),
  so wheel zoom stops at the same limits and passes through the same fit-zoom notch.
- Zoom does nothing while the window is loading (no content yet).

## Fractional deltas

High-resolution wheels and touchpads report deltas smaller than one notch (e.g. 40 of 120).
Rounding each message on its own loses the remainder and the view barely moves. So
`zoom_viewer::WheelZoom` carries the remainder to the next message:

```cpp
struct WheelZoom {
    int remainder = 0;
    int Accumulate(int delta) noexcept;   // returns whole notches, keeps the rest
};
```

Reversing direction discards the remainder, so a leftover from a scroll-up cannot make the
first scroll-down tick. The remainder is also dropped whenever something else changes the
zoom (`Fit`, `Step`, plain-wheel scroll), so `Ctrl+wheel` always starts counting from a
whole notch.

## Where it lives

- `ZoomWindow::SetZoomAt(zoom, anchor)` — the pivot-preserving zoom. `SetZoom` is now a
  thin wrapper that pivots on the window centre, so buttons and keyboard are unchanged.
- `ZoomWindow::ZoomByWheel(delta, anchor)` — accumulates deltas, then walks the zoom ladder
  one notch at a time, re-pivoting on `anchor` each step.
- `ZoomWindow::HandleMessage`, `WM_MOUSEWHEEL` — reads `MK_CONTROL` from the message's key
  state and routes to `ZoomByWheel` with the pointer position in DIPs.

`ZoomWindow` rebuilds its target and fit-zoom on every layout, so no new state had to be
threaded through window resizing or DPI changes.

## Testing

`tests/test_mermaid_renderer.cpp` covers `WheelZoom::Accumulate` (fractional accumulation,
direction reversal, exact notches, zero) alongside the existing `FitZoom` / `NextZoom`
tests. `SetZoomAt` and the message routing are exercised through the window at runtime —
the wheel path needs a real `HWND` and capture, which the fake `IZoomWindow` cannot drive.
