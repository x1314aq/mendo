# Diagram viewer — Ctrl+wheel zoom, and PNG preview — design

## Part 1 — Ctrl+wheel zoom (`src/mermaid/diagram_view.cpp`)

Today `WM_MOUSEWHEEL` always scrolls (Shift = horizontal), and zoom only comes from the
three bar buttons, `+`/`-`/`0`, and the scrollbar. Add:

- `MK_CONTROL` in `GET_KEYSTATE_WPARAM(wparam)` → zoom instead of scroll.
- Wheel up (`+delta`) = zoom in. One notch = one entry of the existing stop list, via
  `diagram_view::NextZoom`, so the result can never land between stops and the fit zoom
  stays one of them (no "37.4%" label).
- Sub-notch deltas (precision touchpads send ±15/±30) accumulate in a counter; a step is
  taken every `WHEEL_DELTA` and the remainder is dropped when the direction flips.
- The point under the cursor stays put, like every other image viewer. `SetZoom` currently
  anchors on the window centre; it becomes a thin wrapper over a new
  `SetZoomAt(float zoom, D2D1_POINT_2F anchor)`, so the buttons and keys behave exactly as
  before while the wheel anchors on the pointer.
- Nothing changes when Ctrl is not held: wheel keeps scrolling (Shift keeps being
  horizontal), and the button/key paths are untouched.

Wheel-with-Ctrl over the *document* window already zooms the document; the viewer is a
separate HWND with its own WndProc, so the two never see each other's messages.

## Part 2 — Preview a PNG in the same window

The viewer holds either a diagram or an image. `HasContent()` accepts both;
`HasDrawing()` remains specific to diagrams for the render-request lifecycle.

- `Diagram` stays as today: recorded `ID2D1CommandList` + `DiagramDrawing` (needed to
  rebuild the command list after a device loss).
- `Image` holds a compressed file snapshot and an `ID2D1ImageSourceFromWic` created on
  the viewer's D2D device. WIC frames, converters and scalers are temporary application
  references; D2D retains the source chain for its cache. No CPU bitmap or separate WIC
  fallback is retained by the viewer. The image source is released before its snapshot.

Everything that is not "how pixels are produced" is shared: title, rounded bar, ± / fit
buttons, zoom label, scrollbars, drag-to-pan, arrow/page/home/end keys, `Esc`, DPI change,
device-loss recovery. Layout, scrolling and the fit zoom only need a natural size:

- diagram → `drawing->width/height` (CSS px, as today),
- image → pixel size ÷ DPI scale, i.e. the same DIP size the document draws it at, so 100%
  in the viewer matches 100% in the page.

`Origin()`, `ScrollTo()`, `UpdateScrollBars()`, `Layout()` and `NextZoom`/`FitZoom` use
`ContentSize()`. Images are drawn with `DrawImage` and
`D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC`; the local bounds are mapped to the same
display size regardless of the image's embedded DPI.

### Decoding rules
- Local files only — `path_util`/`ResolveLocalPath` already reject UNC and device paths
  (a preview must not stall on SMB).
- Decode through the same WIC format allowlist as document images; PNG is the target.
- Caps: 256 MB file (same as `ImageLoader::MAX_IMAGE_FILE_SIZE`), and a long edge of 8192 px
  via `IWICBitmapScaler`. Nothing is drawn from a partially decoded file.
- Downscaling happens before the PBGRA conversion for frames that carry no alpha (JPEG and
  friends), so the decoder's own scaled decode (`IWICBitmapSourceTransform`) is used and the
  full-resolution surface is never materialized. Alpha frames scale after conversion, since
  interpolating straight alpha bleeds the edges. This mirrors `wic_util::DecodeToWicBitmap`.
- The image source uses `CACHE_ON_DEMAND`. Creation also calls `EnsureCached(nullptr)`
  to settle the cache before accepting an image. This catches structural damage (a file
  truncated before or inside the pixel data, so the decoder cannot produce a frame at
  all) and populates the entire capped image cache up front — it does not promise GPU
  memory proportional to the visible region. It does **not** validate pixel contents:
  WIC's PNG/JPEG decoders accept e.g. a corrupted-but-CRC-consistent `IDAT` and return
  all-zero pixels with `S_OK`, and D2D cannot see through to that, so such an image still
  shows as blank rather than raising the failure path.
- After a device loss, a temporary WIC source chain and a new D2D image source are created
  from the retained compressed bytes. Original pixel dimensions are retained even when
  decoding downsizes. Recovery does not reopen the file or reuse a retained WIC fallback.
- Reading, decoding and cache initialization run on the UI thread when opening or rebuilding
  the image. File-size and output-resolution caps bound each operation.

### Lifecycle
- One viewer window, as today: opening an image replaces a shown diagram and vice versa
  (`ShowLoading()` → new content).
- Opening/switching documents closes the viewer (existing `OpenDocument` path). Editing the
  file in place (`ReloadDocument`) keeps the image snapshot; Ctrl+click reads the image
  again. Images skip the reload-follow logic, since it tracks diagram node indexes.
- Dark mode repaints the background; the image is never re-encoded or tinted.
- Failed decode (missing file, unsupported codec, over the cap) shows the existing error
  path: the window closes and the usual "cannot be shown" toast is raised, so a broken
  reference never leaves an empty window behind.
- Permanent failures while rebuilding or drawing an image use the same close-and-notify
  path. Device loss releases the device resources so the normal device recovery can retry.

### Entry point — Ctrl+click an image
- `Ctrl+click` on an image in the document opens it in the viewer, matching the existing
  "Ctrl+click = open elsewhere" convention (file pane entries, links).
- The image hit test already returns the `src` (`FindImageHitAtPosition`), and
  `ResolveLocalPath(src, doc.GetDirectory())` turns that into the absolute path; a link
  wrapping the image keeps opening the link, as today.
- A decode that fails (missing file, unsupported codec, over a cap) still opens nothing and
  raises the existing failure toast, so a stale reference never leaves an empty window.

### Window title
- `Image Viewer` — a fixed English title, not localized (decided). No new i18n fields, so
  the five-language string tables and `test_locale.cpp` are untouched by part 2.
- The diagram title (`i18n::S().viewer_title`) stays as it is, and is re-applied when the
  window switches back to a diagram.

### Strings and docs
- The image window title is the fixed English `Image Viewer` — no i18n field for it.
- No new failure string either: the image path reuses `toast_diagram_view_failed` as-is
  (decided). Nothing in `i18n::Strings` changes, so the five tables and `kStringFields` in
  `tests/test_locale.cpp` are untouched.
- `res/help_*.md` (5 languages): the Diagram Viewer bullet gains "Ctrl+wheel to zoom", plus
  a line that `Ctrl+click` on an image previews it. The `README` feature list is unchanged —
  no new button appears in the chrome.

### Naming
`DiagramView` keeps its name even though it now shows images too (decided) — renaming would
touch ~40 references for no behaviour change. The class comment gains one line saying it
shows both a diagram and a PNG.

### Tests
- Helpers and WIC decoding get unit tests in `tests/test_diagram_view.cpp` (including the
  `FitZoom` / `NextZoom` cases moved from `test_command_executor.cpp`):
  wheel delta → step direction and accumulation, including partial notches and direction
  reversal;
  image natural size = pixels ÷ DPI scale;
  long-edge / file-size caps;
  rejecting network and device paths;
  pixel ownership after closing the source file, and PNG transparency after scaling;
  the opaque-frame scaler landing inside the format converter, and alpha frames scaling
  after it.
- D2D integration tests use a WARP device without an HWND. They cover drawing with high
  quality cubic interpolation after releasing application WIC references, recreating the
  image source on a new device from the snapshot, and rejecting a truncated file whose
  dimensions are still readable. Pixel-level corruption is not testable: WIC reports
  success there, so no case pins that down.
- `tests/test_app_controller.cpp` covers image hit testing and linked-image targets.
- The window itself stays untested, as today (no HWND in tests).
