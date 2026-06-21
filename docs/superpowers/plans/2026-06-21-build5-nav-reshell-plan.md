# Build 5 — Navigation Reshell Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Collapse the three ML pipeline tabs (Train / Convert / Deploy & Run) under a top-bar **ML ▾** dropdown and present the board/dev tabs (Files, Tasks, Hardware, Edit, Examples) as direct top-level entries. **Presentation only** — no behavior or state change. Spec: `docs/superpowers/specs/2026-06-21-nav-reshell-design.md`.

**Architecture:** A focused rewrite of two functions in one file (`crates/kbdk-ui/src/app.rs`): `top_bar()` (group the ML tabs under an egui `menu_button`, dev tabs flat) and the `--tab` startup parser in `KbdkApp::new()` (widen to accept every current tab name). The `Tab` enum, `ui()` routing, all tab modules, workers, `Msg` variants, `pump()`, persistence, and every other test hook are untouched. No new modules, no new dependencies.

**Tech Stack:** Rust, eframe/egui 0.34 (`menu::menu_button`), `theme::` color constants. This is the **last** board-IDE build — done after Edit (Build 3) and Examples (Build 4) so the tab set is final.

## Global Constraints

- **Presentation only.** Do NOT add/remove/rename `Tab` variants, change their `serde` names, touch `ui()` routing, or alter any tab module, worker, `Msg`, `pump()` arm, or the `Fields` persistence schema. Persisted `f.tab` from older builds MUST still load and select the same tab.
- **Single file.** All edits land in `crates/kbdk-ui/src/app.rs`. No new files, no `main.rs`/`lib.rs`/`Cargo.toml` changes, **no new dependencies**.
- **egui 0.34 idioms (match the codebase):** `App::ui` + `Panel::top(...).show_inside` shell stays; use `ui.menu_button("ML ▾", |ui| …)` / `egui::menu::menu_button` for the dropdown; ML items stay `ui.selectable_value(&mut self.f.tab, Tab::X, "…")`; colors via `theme::` constants only (never literal `Color32`). Do not touch zoom (`set_zoom_factor` stays as-is).
- **Test hooks preserved.** `--screenshot`, `KBDK_SHOT_DELAY`, `KBDK_FIELDS`, `KBDK_AUTOTRAIN`, `KBDK_AUTOCONVERT`, `KBDK_POLL`, `KBDK_AUTOCAPTURE` keep working unchanged. ONLY `--tab` changes, and only by *accepting more names* (unknown still → `Train`).
- **No board / hardware access during the build.** This build has no board I/O. The live on-board screenshot gate is run by the **human afterward** — do NOT attach to hardware for UI verification.

---

## File Structure

**Created:** none.

**Modified:**
- `crates/kbdk-ui/src/app.rs` — `top_bar()` (ML menu + dev tabs) and the `--tab` match in `KbdkApp::new()`.

---

## Task 1: Confirm the final tab set and the seams to touch

**Files:** read-only inspection of `crates/kbdk-ui/src/app.rs`.

This build runs after Edit/Examples, so first establish what the enum actually contains at this point in history (Examples may be its own `Tab::Examples` or folded into the Edit tab — the spec says the reshell adapts, adds/removes nothing).

- [ ] **Step 1: Read the current `Tab` enum** in `app.rs`. Record the exact variant list. Expected after Builds 3–4:
  ```rust
  pub enum Tab { Train, Convert, Deploy, Files, Tasks, Hardware, Edit, Examples }
  ```
  If `Examples` is NOT a variant (Build 4 put it inside the Edit tab), simply omit `Examples` from the top-level row in Task 2 and from the `--tab` match in Task 3 — do not invent it.

- [ ] **Step 2: Read the current `top_bar()`** (the `ui.selectable_value(…)` block). Confirm it still has the heading + subtitle + `separator()` first and the right-aligned device-badge block (`with_layout(egui::Layout::right_to_left(…))`) last. Those bracket the tab row and must be preserved verbatim.

- [ ] **Step 3: Read the current `--tab` parser** in `KbdkApp::new()` (the `skip_while(|a| a != "--tab").nth(1)` match). Confirm whether Build 3 already added `"edit" => Tab::Edit` (and Build 4 `"examples"`). If so, Task 3 only folds in the remaining names; do not duplicate the parser.

**Verification:** you can state the exact `Tab` variant list and the exact lines that Task 2 and Task 3 will replace. No code change yet.

---

## Task 2: Rewrite `top_bar()` — ML menu + dev tabs

**Files:** Modify `crates/kbdk-ui/src/app.rs` (`top_bar` only).

**Interface:** signature unchanged — `fn top_bar(&mut self, ui: &mut egui::Ui)`.

Replace ONLY the six flat ML/dev `selectable_value` lines (between the `separator()` and the `with_layout(right_to_left …)` device-badge block). Keep everything else in the function byte-for-byte.

- [ ] **Step 1: ML group as a `menu_button`.** Where the flat `Train`/`Convert`/`Deploy` lines were, add:
  ```rust
  let ml_active = matches!(self.f.tab, Tab::Train | Tab::Convert | Tab::Deploy);
  let ml_label = if ml_active {
      // current ML tab name so the bar shows where you are
      let name = match self.f.tab {
          Tab::Convert => "Convert",
          Tab::Deploy => "Deploy & Run",
          _ => "Train",
      };
      format!("ML  ·  {name}  ▾")
  } else {
      "ML  ▾".to_string()
  };
  let ml_text = if ml_active {
      egui::RichText::new(ml_label).color(theme::MAUVE).strong()
  } else {
      egui::RichText::new(ml_label)
  };
  ui.menu_button(ml_text, |ui| {
      ui.selectable_value(&mut self.f.tab, Tab::Train, "Train");
      ui.selectable_value(&mut self.f.tab, Tab::Convert, "Convert");
      ui.selectable_value(&mut self.f.tab, Tab::Deploy, "Deploy & Run");
  });
  ```
  (Exact label format / color is a presentation detail; the requirement is the button visibly indicates when an ML tab is active, and the menu items use `selectable_value` so the active one is checked when open. If `menu_button` rejects `RichText`, fall back to a plain `&str` label and color via the active check, or use `egui::menu::menu_button(ui, …)`.)

- [ ] **Step 2: Dev tabs flat, in the settled order** (`Edit · Examples · Files · Tasks · Hardware`):
  ```rust
  ui.selectable_value(&mut self.f.tab, Tab::Edit, "Edit");
  ui.selectable_value(&mut self.f.tab, Tab::Examples, "Examples"); // omit if no Examples variant
  ui.selectable_value(&mut self.f.tab, Tab::Files, "Files");
  ui.selectable_value(&mut self.f.tab, Tab::Tasks, "Tasks");
  ui.selectable_value(&mut self.f.tab, Tab::Hardware, "Hardware");
  ```

- [ ] **Step 3: Leave the rest of `top_bar` untouched** — the heading, `KidBright µAI dev-kit` subtitle, `separator()`, and the right-to-left device-badge block stay exactly as they are.

**Verification:** `cargo build -p kbdk-ui` compiles. The `ui()` `match self.f.tab` arm count is unchanged (every variant still routes). Visually confirmed at the screenshot gate (Task 4).

---

## Task 3: Widen the `--tab` startup hook

**Files:** Modify `crates/kbdk-ui/src/app.rs` (the `--tab` match in `KbdkApp::new()`).

- [ ] **Step 1: Extend the match** to accept every current tab name, keeping the total fallback so old invocations are unaffected:
  ```rust
  app.f.tab = match tab.as_str() {
      "convert"  => Tab::Convert,
      "deploy"   => Tab::Deploy,
      "files"    => Tab::Files,
      "tasks"    => Tab::Tasks,
      "hardware" => Tab::Hardware,
      "edit"     => Tab::Edit,
      "examples" => Tab::Examples,   // omit if no Examples variant
      _          => Tab::Train,      // includes "train" and any unknown
  };
  ```
  If Build 3/4 already added some of these arms, just add the missing ones — keep a single match, no duplication.

**Verification:** `cargo build -p kbdk-ui` clean. Each new name resolves (exercised at the screenshot gate, Task 4); unknown names still land on `Train`.

---

## Task 4: Build, test, and the on-board screenshot gate

**Files:** none (verification only).

- [ ] **Step 1: Workspace build is clean:**
  ```sh
  cargo build
  ```
- [ ] **Step 2: kbdk-core tests still pass** (unchanged by this build, but the gate is mandatory):
  ```sh
  cargo test -p kbdk-core
  ```
- [ ] **Step 3: Reachability sanity (local, optional, no board needed):** run `cargo run -p kbdk-ui`, click `ML ▾` and confirm Train/Convert/Deploy & Run each select; click each dev tab (Edit/Examples/Files/Tasks/Hardware) and confirm it selects and shows content. Confirm the device badge still sits at the right edge with no clipping at the 1.25× default zoom.
- [ ] **Step 4: On-board screenshot gate — RUN BY THE HUMAN AFTERWARD (do NOT attach to hardware in the build session).** Capture at least:
  - default launch: `cargo run -p kbdk-ui -- --screenshot /tmp/nav_default.png` — verify the new bar (`ML ▾` + dev tabs + device badge) renders without overflow/clipping.
  - `--tab convert` (an ML tab via the widened hook), `--tab edit`, `--tab examples`, `--tab hardware` — confirm each name resolves and the tab renders its content. Example:
    ```sh
    cargo run -p kbdk-ui -- --screenshot /tmp/nav_edit.png --tab edit
    ```
  Builds 1–4 each shipped a layout bug only the live screenshot caught (menu width / bar overflow are exactly this category) — this gate is non-negotiable.

**Verification:** `cargo build` and `cargo test -p kbdk-core` green; the human-run screenshots show the regrouped bar and confirm every tab is reachable and renders.

---

## Task 5: Docs

**Files:** Modify `CLAUDE.md` (the kbdk-ui paragraph).

- [ ] **Step 1: Update the kbdk-ui tab description in `CLAUDE.md`** to reflect the regrouped navigation: ML pipeline (Train/Convert/Deploy & Run) lives under a top-bar **ML ▾** dropdown; Edit/Examples/Files/Tasks/Hardware are direct top-level tabs. Keep it to a sentence or two folded into the existing kbdk-ui notes (this is chrome, not a new capability). Note the `--tab` hook now accepts all tab names.

**Verification:** the kbdk-ui section in `CLAUDE.md` describes the new nav; no other docs need changing (this build added no kbdk-core API, no new module, no new dependency).

---

## Done criteria

- `cargo build` (workspace) and `cargo test -p kbdk-core` pass.
- `top_bar()` shows `ML ▾` (Train/Convert/Deploy & Run inside) + Edit/Examples/Files/Tasks/Hardware top-level; device badge unchanged; active ML group is visually indicated.
- `--tab` accepts `train|convert|deploy|files|tasks|hardware|edit|examples`, unknown → `Train`.
- `Tab` enum, `ui()` routing, all tab modules, workers, `Msg`, `pump()`, and the `Fields` persistence schema are unchanged — old persisted `f.tab` still loads.
- Human-run on-board screenshots confirm the bar renders and every tab is reachable.
</content>
