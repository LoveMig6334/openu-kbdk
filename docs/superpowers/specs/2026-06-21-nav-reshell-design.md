# Build 5 — Navigation reshell (collapse ML under a top-bar menu)

> **Status:** design settled 2026-06-21. This is the spec for **Build 5** (the final
> build) of the board-IDE roadmap
> (`docs/superpowers/plans/2026-06-21-board-ide-remaining-features.md`). Next step
> after review: `superpowers:writing-plans` → the bite-sized implementation plan
> (`docs/superpowers/plans/2026-06-21-build5-nav-reshell-plan.md`) → subagent-driven
> build → review → merge, same flow as builds 1–4.

## Goal

Realize the agreed "full board IDE" navigation. `kbdk-ui` started as an ML pipeline
(Train / Convert / Deploy & Run) and grew board-IDE tabs (Files, Tasks, Hardware,
Edit, Examples). With eight tabs the flat top bar is overloaded and buries the
IDE-first story. Build 5 **regroups the chrome only**: collapse the three ML pipeline
tabs under a top-bar **ML ▾** dropdown, and present the board/dev areas as direct
top-level entries.

This is the last build, done **after** Edit (Build 3) and Examples (Build 4), so the
final tab set is known and the nav is reshaped exactly once.

## This is presentation only

The single most important constraint: **no behavior or state change.** This build
touches navigation chrome and one test hook, nothing else.

- The `Tab` enum is **unchanged** (same variants, same `serde` (de)serialization, so
  persisted `f.tab` from any prior build still loads and selects the same tab).
- The `ui()` routing `match self.f.tab { … }` is **unchanged** (every variant still
  maps to the same `*_tab::show(self, ui)`).
- All per-tab modules, state on `KbdkApp`, workers, `Msg` variants, and `pump()` arms
  are **untouched**.
- Every test hook keeps working: `--screenshot`, `KBDK_SHOT_DELAY`, `KBDK_FIELDS`,
  `KBDK_AUTOTRAIN`, `KBDK_AUTOCONVERT`, `KBDK_POLL`, `KBDK_AUTOCAPTURE`. The **only**
  hook that changes is `--tab`, and only to *accept more names* (it currently silently
  collapses unknown names to `Train`).

The diff is confined to two functions in one file: `top_bar()` (rewrite the tab row)
and the `--tab` startup parser in `KbdkApp::new()` (extend the match). No new modules,
no new dependencies.

## Settled design decisions

These resolve the "Open design decisions" the roadmap left for Build 5:

| Decision | Choice | Notes |
| --- | --- | --- |
| Top-bar menu vs left sidebar rail | **Top-bar menu** (egui `menu_button`) | Lowest-risk; keeps the existing `Panel::top` shell and device badge layout. A left rail would reflow every tab's `show_inside` panels — out of scope for a presentation-only build. |
| ML grouping | **`ML ▾` dropdown** holding **Train / Convert / Deploy & Run** | The three pipeline stages live together behind one menu. |
| Dev/board tabs | **Direct top-level entries:** Files, Tasks, Hardware, Edit, Examples | The IDE-first surface. |
| Top-level order | `ML ▾` first, then **Edit · Examples · Files · Tasks · Hardware** | ML kept leftmost (preserves muscle memory from the flat bar where Train was first); editor-centric tabs lead the board group. |
| `--tab` hook | **Accept every current tab name**, default `Train` for unknown | Was `convert`/`deploy`/`_=>Train`; extend to `train`/`convert`/`deploy`/`files`/`tasks`/`hardware`/`edit`/`examples`. |
| Menu-label ↔ selected-tab feedback | The **`ML ▾` button shows it is the active group** when the selected tab is one of Train/Convert/Deploy (e.g. label renders as `ML ▾ · Train` or is colored), and the menu items use `selectable_value` so the current ML tab is checked inside the open menu | Keeps the "where am I" affordance the flat `selectable_value` row gave for free. |

## Final tab set (the dependency on Builds 3 & 4)

Build 5 assumes the `Tab` enum at merge time is the full board-IDE set. After Builds
3 (Edit) and 4 (Examples):

```rust
pub enum Tab { Train, Convert, Deploy, Files, Tasks, Hardware, Edit, Examples }
```

> **Build-order note.** If Build 4 lands Examples *inside* the Edit tab rather than as
> its own `Tab::Examples` variant (the roadmap leaves that open), then `Examples` is
> simply absent from the top-level row — the reshell adapts to whatever variants the
> enum actually has. The reshell **adds no variant** and **removes none**; it only
> regroups the variants that exist. The plan's Task 1 begins by reading the current
> enum and adjusting the top-level list to match.

## Architecture

### Before (flat `selectable_value` row in `top_bar`)

```
[kbdk] [KidBright µAI dev-kit] | Train  Convert  Deploy & Run  Files  Tasks  Hardware …   ● adb …
```

Each tab is a `ui.selectable_value(&mut self.f.tab, Tab::X, "X")`.

### After (ML grouped under a menu)

```
[kbdk] [KidBright µAI dev-kit] | [ML ▾]  Edit  Examples  Files  Tasks  Hardware       ● adb …
                                    └─ (menu) Train · Convert · Deploy & Run
```

- **`ML ▾`** is an `egui::menu::menu_button` (the egui 0.34 idiom). Inside its
  closure, three `ui.selectable_value(&mut self.f.tab, Tab::Train|Convert|Deploy, "…")`
  calls — same widget, same binding target, just nested in the dropdown. Selecting one
  sets `self.f.tab` exactly as the flat row did; egui closes the menu on click.
- The dev tabs stay as flat `selectable_value` calls in the horizontal row.
- The right-aligned device badge block (`with_layout(right_to_left …)`) is **unchanged**.

### Active-group feedback for `ML ▾`

Because a `menu_button`'s own button is not a `selectable_value`, it does not light up
when one of its hidden items is active. To preserve the "current tab is highlighted"
cue:

- Compute `let ml_active = matches!(self.f.tab, Tab::Train | Tab::Convert | Tab::Deploy);`
- Render the button label so the active group is obvious — preferred: keep the short
  trigger text `ML ▾` but **append the active ML tab name** when `ml_active`
  (`format!("ML ▾  ·  {}", name)`), and/or color it with `theme::MAUVE`/the selected
  accent. Final visual detail is a plan-level choice; the requirement is that a glance
  at the bar tells the user when they are on an ML tab. The `selectable_value`s inside
  the menu already show a check on the active item when the menu is open.

### `--tab` startup hook (the one behavioral surface that changes — and only widens)

Current parser in `KbdkApp::new()`:

```rust
if let Some(tab) = std::env::args().skip_while(|a| a != "--tab").nth(1) {
    app.f.tab = match tab.as_str() {
        "convert" => Tab::Convert,
        "deploy"  => Tab::Deploy,
        _         => Tab::Train,
    };
}
```

New parser accepts every current tab name (still defaulting unknown → `Train`, so old
invocations are unaffected):

```rust
app.f.tab = match tab.as_str() {
    "convert"  => Tab::Convert,
    "deploy"   => Tab::Deploy,
    "files"    => Tab::Files,
    "tasks"    => Tab::Tasks,
    "hardware" => Tab::Hardware,
    "edit"     => Tab::Edit,
    "examples" => Tab::Examples,
    _          => Tab::Train,           // includes "train"
};
```

This is what lets the on-board screenshot gate target each tab by name (e.g.
`--screenshot shot.png --tab hardware`), the same mechanism Builds 1–4 used.

## Components / files

**Modify (only):**

- `crates/kbdk-ui/src/app.rs`
  - `top_bar()` — replace the six flat `selectable_value` lines with: an `ML ▾`
    `menu_button` (Train/Convert/Deploy inside) + active-group feedback, then the dev
    tabs (Files/Tasks/Hardware/Edit/Examples) as flat `selectable_value`s, in the
    settled order. The heading, subtitle, `separator`, and device-badge block stay.
  - `KbdkApp::new()` — widen the `--tab` match to all current tab names.

**Create:** none. **New dependencies:** none. **New `Msg`/worker/state:** none.

> If Build 3 already added a string→`Tab` mapping for `--tab edit`, Build 5 simply
> folds the remaining names into that single match (don't duplicate the parser).

## egui 0.34 notes (kept consistent with the rest of kbdk-ui)

- Use `egui::menu::menu_button(ui, "ML ▾", |ui| { … })` (or `ui.menu_button("ML ▾",
  |ui| …)`), the 0.34 menu idiom. The `App::ui` + `Panel::top(...).show_inside`
  shell is **unchanged** (per the codebase's `App::ui`/`show_inside`, never
  `update`/`show`).
- The ML items remain `selectable_value(&mut self.f.tab, Tab::X, "X")` so they show
  a checkmark for the active item and write `self.f.tab` directly.
- Colors via `theme::` constants only (`MAUVE`, `SUBTEXT`, the accent), never literal
  `Color32` — matches the existing top bar.
- Zoom/scaling untouched (`set_zoom_factor` already in place; this build adds no
  scaling code).

## Error handling

None new. There is no new I/O, no worker, and no fallible path: the build moves
existing widgets and widens a pure string match. The `--tab` parser already has a
total fallback (`_ => Tab::Train`), preserved.

## Testing & verification

- **Build:** `cargo build` (workspace) is clean; `cargo test -p kbdk-core` passes
  (unchanged — this build adds no kbdk-core code, but the workspace must still build
  and test green).
- **No new unit tests required** (presentation-only; nothing pure to test). The
  `--tab` widening is exercised by the screenshot gate below rather than a unit test,
  consistent with how the other tabs are verified.
- **Reachability check (manual, quick):** every tab is still selectable — the three ML
  tabs from the `ML ▾` menu, the five dev tabs from the top-level row.
- **On-board screenshot gate (mandatory, human-run afterward — do NOT attach to
  hardware during the build):** launch `kbdk-ui --screenshot PATH` for at least:
  - default launch (verify the new bar renders: `ML ▾` + dev tabs + device badge,
    no overflow/clipping at the 1.25× default zoom),
  - `--tab hardware`, `--tab edit`, `--tab examples`, and one ML tab via
    `--tab convert` (confirms the widened `--tab` names resolve and each tab still
    renders its content).

  Builds 1–4 each shipped a layout bug that only the live screenshot caught; this gate
  is non-negotiable even for a chrome-only change (menu width / bar overflow are
  exactly the kind of thing it catches).

## Non-goals

- Any change to tab **contents**, state, workers, messages, or persistence schema
  (the `Fields` struct and `Tab` enum are byte-compatible with prior builds).
- A left sidebar rail (rejected above — would reflow every tab).
- Adding/removing tabs, renaming the `Tab` variants, or changing `serde` names.
- Keyboard shortcuts / command palette for tab switching (possible future polish, not
  in scope).
- Reordering or restyling anything outside the tab row (heading, badge, theme).

## Why last, and why low-risk

- **Last:** doing the reshell before Edit/Examples would reshape the nav twice; after
  them, the eight-tab set is final.
- **Low-risk:** confined to two functions in one file, no new code paths, every test
  hook preserved (one widened). The summary-table risk rating in the roadmap is **Low**
  — this spec keeps it there by forbidding any behavior change.
</content>
</invoke>
