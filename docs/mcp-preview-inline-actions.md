# Quick preview: in-preview file switching and inline file actions

Date: 2026-09-21. Scope: `src/ui/quick_preview_window.{h,cpp}` plus the owner
wiring in `src/app`. Built with `cmake --build build --target pulse` only.

## What changed

### Switching files without leaving the preview

- Title bar now carries three client-area buttons left of the close button:
  previous (`E72B`), next (`E72A`) and more (`E712`), each a 40 px slot with a
  hover pill. Prev/next post the existing `WM_QUICK_PREVIEW_NAVIGATE` (+/-1),
  so they share `NavigateQuickPreview` with the arrow keys; double-clicks keep
  stepping instead of toggling maximize. `WM_NCHITTEST` returns `HTCLIENT` for
  that strip and `HTCAPTION` elsewhere, so dragging the title still works.
- The title text is clipped before the button strip.

### File actions on the previewed entry

- New `ui::QuickPreviewAction` (Open, Cut, Copy, CopyPath, ToggleStar, Rename,
  Delete, Properties) and a third owner message `WM_QUICK_PREVIEW_COMMAND`
  (`WM_APP + 65`; wParam = action, lParam bit 0 = Shift). `Initialize` gained
  a defaulted `command_message` parameter, so other callers are unaffected.
- Right-click / Shift+F10 / the "more" button open one Fluent menu for every
  preview kind. Text and hex keep their Copy / Select all / Find rows first;
  the file verbs follow (Open, Copy for non-text kinds, Cut, Copy path, Star
  or Unstar, Rename, Delete, Properties). Cut / Rename / Delete are disabled
  on recycle-bin and read-only network views (`QuickPreviewItem::read_only`).
- Keyboard inside the preview mirrors the main list: `Ctrl+Shift+C` copy path,
  `Ctrl+C` copy file (text/hex kinds keep text copy), `Ctrl+X` cut, `Delete`
  (Shift = permanent), `F2` rename, `Alt+Enter` properties. All are ignored
  while the find bar owns the keyboard.
- `HandleQuickPreviewCommand` (app_commands.cpp) re-focuses the previewed row
  first, then reuses the existing entry points: `OpenSelected`,
  `CollectToTray`, `CopySelectedPath`, `ToggleStarred`, `ShowRenameOverlay`,
  `DeleteSelected`, `DispatchMenuCommand(CmdProperties)`. Rename closes the
  preview before showing the main-window rename editor.

### Keeping the preview in step with the listing

- `SyncQuickPreview` runs after `ApplyWorkerResult` and `ApplyNotifyToVisible`
  (both UI thread). It locates the previewed entry by name + full path:
  - still present and mtime/size/attrs changed -> `Update` (reload);
  - still present, unchanged -> refresh the star flag only;
  - gone after an in-preview Delete -> re-anchor to the nearest remaining file
    at the recorded view row (`AppState::quickPreviewAnchorView`) so the user
    can keep triaging; otherwise `Close`.
- `QuickPreviewItem` gained `starred` and `read_only`; `SelectedQuickPreviewItem`
  now fills them from `PlacesCatalog` and the tab flags.

## Files

| File | Change |
| --- | --- |
| `src/ui/quick_preview_window.h` | `QuickPreviewAction`, item flags, `item()`, `SetStarred`, chrome button members |
| `src/ui/quick_preview_window.cpp` | button layout/draw/hit-test, unified `ShowContextMenu`, key/syskey handling, `PostAction` |
| `src/app/app_state.h` | `WM_QUICK_PREVIEW_COMMAND`, `quickPreviewAnchorView` |
| `src/app/app_commands.h/.cpp` | `HandleQuickPreviewCommand`, `SyncQuickPreview`, shared `QuickPreviewItemAt` / `QuickPreviewEntryIndex` |
| `src/app/app_main.cpp` | passes the command message to `Initialize`; dispatches it |
| `src/app/app_navigation.cpp` | calls `SyncQuickPreview` after snapshot / notify application |

## Verification

- `cmake --build build --target pulse` (via `scripts\vcvars.bat`): exit 0,
  26 steps, no warnings or errors under `/W4 /permissive- /utf-8`.
  Log: `build/preview-actions-check/build.log`.
- `get_diagnostics` scoped to `src/ui/quick_preview_window.cpp` and `src/app`:
  0 errors, 0 warnings.
- `git diff --check` on the seven touched files: clean. Patched files keep
  their existing CRLF endings.
- `build/pulse.exe` sha256 `abf81dd0a3f5b2b3227ae1683b4035b64242e850e964d04bad158a709598e21b`.
- Not run (per AGENTS.md scope rule): full selftest, `build_release.bat`,
  installer packaging, unrelated test targets. Manual UI pass still advised:
  prev/next buttons, right-click menu on an image and on a text file, Delete
  stepping to the neighbour, F2 handing off to the list rename editor.

## Notes / follow-ups

### Follow-up: system preview handlers (mp4, PDF, Office)

First user test on an `.mp4` showed almost none of the new controls. Those
kinds are not drawn by the preview window; `PreviewHandlerHost` places the
shell handler in a separate `WS_POPUP | HWND_TOPMOST` overlay (owned by a
dedicated STA thread) covering the whole content area, so:

- right-clicks land in the provider's HWND and never reach the preview;
- the Fluent menu (`HWND_TOP`) opened underneath the topmost overlay;
- clicks on the overlay do not enter our thread's queue, so the menu's modal
  loop could not dismiss on them.

Fixes (same build target, `build/pulse.exe` sha256
`8ef5c2ff561d1b2282e7feac70633b0d1fb2e8e0c4d3ef0ca08d7f90e65a262e`):

- `preview_handler_host.cpp`: overlay `WM_CONTEXTMENU` is posted to the owner
  (`PostMessageW`, never synchronous - the STA must not wait on a modal menu).
- `FluentMenu::SetTopmost`: popup and flyout use `HWND_TOPMOST` when the owner
  asks for it, and the modal loop polls for a pressed button over a window of
  another thread and dismisses. The quick preview sets it whenever the current
  kind is handler-hosted (`NativeKind::None`).
- Chrome buttons re-take focus before acting so the keyboard verbs work after
  interacting with the provider.

Title-bar buttons and keyboard shortcuts were already unaffected (title bar is
outside the overlay); right-click / "more" menu now works over video too.

- Star toggling reuses `ToggleStarred`, which also refreshes starred views.
- Delete inside a `pulse:starred` / search view removes the row via the
  normal refresh path; if no neighbour remains the preview closes.
- Menu strings reuse existing ids (`Open`, `Cut`, `Copy`, `CopyPath`, `Star`,
  `Unstar`, `Rename`, `Delete`, `Properties`); no new resources were added.
