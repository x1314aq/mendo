# Recently opened files — design

## Goal
Give mendo a window listing the Markdown files opened most recently, from which the
user can reopen one, drop one entry, or clear the list.

## Data model — `RecentFiles` (`src/io/recent_files.h/.cpp`)
- Newest first, at most `kMaxEntries = 10` entries (the dialog shows all rows without
  scrolling, so the cap is what fits on screen).
- `Add(path)`: ignores empty paths, embedded virtual paths (`mendo://help`), non-local
  paths (UNC / device paths — touching them can trigger SMB auth or a multi-second
  stall) and relative paths; moves an existing entry to the front and re-spells it the
  way it was just opened (case-insensitive comparison, NTFS semantics); trims the tail
  at the cap.
- `RemoveAt`, `Clear`. `Normalize` is the single place that applies the rules.
- `MakeRecentFileLabel(path)` splits a row into file name + parent directory.
- No I/O, so the rules are unit-testable in isolation.

## No persistence (deliberate)
- The list lives only in the process: **it starts empty every time mendo starts** and
  nothing is written to `settings.ini`. There is no `[RecentFiles]` section, no
  `session::SaveRecentFiles / LoadRecentFiles`, and `RecentFiles` has no
  `SetEntries` — there is no code path that could restore a list.
- Consequence to keep in mind: opening a file in a second window does not reach the
  first window's list, and killing the process loses the list (there is nothing to
  lose on disk either).
- Restoring across runs would mean adding a section + a load path again; the model
  itself is persistence-agnostic, so that stays a small, local change.

## Entry points
- Title bar button `TitleBarHitZone::RecentFiles` right of *Open File* (clock/history
  glyph), with a tooltip in the current UI language.
- `Ctrl+Shift+O` (`Ctrl+O` stays the open dialog).
- Both go through one path: `ShowRecentFilesAction` → reducer → `effect::ShowRecentFiles`
  → `App::ShowRecentFilesDialog`, so the title bar and the shortcut cannot drift apart.

## Dialog — `RecentFilesDialog : PopupWindow<RecentFilesDialog>`
- Same self-drawn popup shell as the settings dialog and context menu: created per
  open, rounded frame, theme colors, DPI aware, owner-modal, wheel forwarded to the
  document. `PopupWindow` gained a `requires { derived().OnKeyDown(wParam); }` hook so
  the base keeps handling Esc / Enter while the list gets the arrow keys first.
- Width comes from the longest file name / parent folder, clamped to
  `MIN_WIDTH..MAX_WIDTH` so a deep path cannot cover the window.
- Rows: file name (primary) over the parent directory (secondary). Both use DWrite
  ellipsis; the directory keeps the text after the last `\` so the nearest folder
  stays readable. Hover and keyboard selection share the `PaneItemHover` highlight.
- Per-row `×` (drawn only for the hovered row or for the keyboard selection) calls
  `on_remove(index)`; the footer button calls `on_clear()`. Both rebuild the layout
  and re-centre the window at its current centre. Neither closes the dialog.
- `↑`/`↓`/`Home`/`End` move the selection, `Delete` removes it, `Enter` opens it,
  `Esc` closes. The empty list shows a centered "no recent files" line and no footer
  button.
- Opening a row closes the dialog first (the callback starts a load), then calls
  `on_open(path)`; App dispatches the existing `OpenPathAction`, so history, file pane
  follow, live reload and scroll restoration behave exactly as for any other open.

## Wiring
- `App` owns `RecentFiles recent_files_`; it starts empty, is appended in `FinishLoad`
  via `RememberRecentFile()` (documents without a backing file — help, licenses — are
  skipped), and is never written to the config.
- Tests: `test_recent_files.cpp` (the model rules),
  `test_recent_files_dialog.cpp` (layout / hit test / keys / every language), plus
  updates to the title bar, controller and locale field lists.
- Docs: `res/help_*.md` gained a "how to reopen" bullet, a shortcut row and a tip;
  the five `README`s list the feature.
