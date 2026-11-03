# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

This is a personal fork of the 4coder editor (frozen upstream in 2022, MIT licensed) being turned into a full time editor with a vim style modal layer. The checkout lives at `4ed/code`; builds go to the sibling `4ed/build`. Upstream also expects a sibling `4coder-non-source` repo (fonts, icons, Windows freetype libs) — it is **not present** here, so anything that reads from `../4coder-non-source` is missing and has local fallbacks (system freetype via `pkg-config`, system Liberation Mono font).

## Commands

```sh
bin/build-linux.sh                 # dev build (-g, FRED_INTERNAL) of everything into ../build
bin/build-linux.sh -DOPT_BUILD     # optimized build
bin/install-linux.sh               # optimized build + install to ~/.local/share/4coder, symlink ~/.local/bin/4coder
bin/test-vim-linux.sh [filter]     # drives a real 4coder window with xdotool and checks vim behavior
```

- The build driver does **not** stop on compile errors; it keeps going and prints "copy ship_files/themes" at the end regardless. Always grep the output for `error`.
- Rebuild only the custom layer (fast) from the build dir: `cd ../build && ../code/custom/bin/buildsuper_x64-linux.sh ../code/custom/4coder_vim_bindings.cpp`.
- Running from `../build` needs `fonts/liberation-mono.ttf`, `config.4coder` and `bindings.4coder` next to the binary; the build never creates them (the install script does). User settings go in `~/.4coder/`, which is searched before the binary's folder.
- There is no unit test suite. `bin/test-vim-linux.sh` needs the X display (`DISPLAY=:0`) and only sends keys while the 4coder window it launched is focused. Add a `check` line there for every vim behavior you add or fix. Code files are re-indented on save (`automatically_indent_text_on_save`), which affects expected output for `.java` cases. When driving windows with xdotool yourself, use `xdotool search --all --pid ...` (the default matches *any* condition) and note that 4coder started through the `4coder` symlink runs as a process named `4coder`, not `4ed`.

## Architecture

Three binaries, built by `bin/4ed_build.cpp` (a C++ program the shell script compiles and runs):

- `4ed` — platform layer (`platform_linux/linux_4ed.cpp`, X11 + OpenGL + FreeType). Loads the other two from the directory of the real executable (`/proc/self/exe`, so symlinks work).
- `4ed_app.so` — the core (`4ed_app_target.cpp` is a unity build of the `4ed_*.cpp` files): buffers, history, layout, views, the input loop in `4ed.cpp`.
- `custom_4coder.so` — the custom layer, everything in `custom/`. Entry point is `custom_layer_init`. `default_custom_target` in `bin/4ed_build.cpp` selects the entry file; it is `custom/4coder_vim_bindings.cpp` (the stock one is `4coder_default_bindings.cpp`).

The core exposes its API to the custom layer through generated tables (`custom/generated/custom_api.h`, implemented in `4ed_api_implementation.cpp`). The custom build runs a metadata generator over the preprocessed source to produce `custom/generated/command_metadata.h`; any function declared with `CUSTOM_COMMAND_SIG(name)` + `CUSTOM_DOC("...")` is registered automatically (shows in the command lister, bindable from `bindings.4coder`). That generated file changes whenever commands are added — that is expected.

Input flow: every view runs `default_view_input_handler` as a coroutine; it asks `implicit_map_function` which command handles each event. Commands can block for more input with `get_next_input` (that is how prompts, listers and vim operators read pending keys). A key press arrives as a `KeyStroke` event followed by a dependent `TextInsert` event; if a command handles the keystroke, its text is blocked, otherwise the text is delivered. Every real key/text event is also appended to the `*keyboard*` buffer (`4ed.cpp`), and `enqueue_virtual_event` replays events — keyboard macros are built on this.

At startup `default_startup` → `default_4coder_initialize` reloads config and **rebuilds `framework_mapping`** from `bindings.4coder` (falling back to `setup_default_mapping`), discarding maps set in `custom_layer_init`.

Languages: only the C++ lexer exists (`custom/generated/lexer_cpp.*`, produced by the lexer generator). `default_begin_buffer` in `4coder_default_hooks.cpp` decides which files are lexed; `.java` was added there and uses the C++ lexer. Auto indent, highlighting and the code index all need tokens, so unlexed files get none of them.

### Vim layer (`custom/4coder_vim.cpp`)

- Installed by `vim_init`: sets `implicit_map_function = vim_implicit_map` and the render caller hook. Its key maps live in a separate `vim_mapping` because of the `framework_mapping` reset above.
- Insert mode defers to the normal 4coder maps (typing, auto indent, completion). Normal/visual mode handle plain keys as **text events** (`vim_handle_text`), so shifted symbols are layout independent; Ctrl/Alt chords and special keys go through `vim_mapping`, then fall back to the 4coder maps.
- Multi key commands (operators, `f`/`t`, `g`/`z` prefixes, registers) read further characters with `vim_read_char` inside the running command.
- Normal mode keeps the cursor on a character (`vim_clamp_cursor`); `j`/`k` move by buffer lines with a remembered goal column, not by wrapped rows. Use `vim_line_count`/`vim_clamp_line`, not `buffer_get_line_count`: 4coder counts the empty line after a trailing newline, vim does not.
- `.` repeat does not replay events: `vim_read_char` captures the keys a change consumed, `vim_leave_insert` captures the inserted text from the buffer, and `vim_repeat_change` feeds them back through `vim_execute` synchronously (via `vim_queue_*`, with `vim_replaying` set). Any new prompt that reads input inside a change must also read from the queue when replaying, as `vim_search_prompt` does.
- Macros (`q`/`@`) copy a slice of the `*keyboard*` log and replay it with `enqueue_virtual_event`. Virtual events are appended to the end of the queue, so nothing that runs during playback may enqueue more events (nested `@` is refused for that reason).
- Rendering: `vim_render_caller`/`vim_render_buffer` are copies of the default render functions with the mode label, per mode cursor, visual selection and search highlight added. Keep them in sync if the defaults change.
- Search pickers live in `custom/4coder_fuzzy_search.cpp` (not vim specific): `rg --files` / `rg --vimgrep` run through `popen` from the git root of the current file (else the hot directory), shown in a lister with `fuzzy_filter` set. The fzf style scoring is in `4coder_lister_base.cpp` (`lister_get_filtered_fuzzy`); listers without the flag keep 4coder's substring filter. Grep output is also written to `*rg*` and locked as the jump buffer for `:cn`/`:cp`.
- Java LSP lives in `custom/4coder_lsp.cpp` (JSON parser, jdtls launch, client). jdtls is spawned with a socket for stdin (written with `MSG_NOSIGNAL`) and a pipe for stdout; a reader thread queues messages and calls `system_signal_step`. The tick hook (`lsp_tick`, wrapping `default_tick`) handles notifications and syncs changed buffers as full text. Commands that need an answer call `lsp_request`, which waits inside the command; it must use `animate_in_n_milliseconds(app, 0)` (only a zero delay produces the Animate event that wakes `get_next_input`). Server-to-client requests must always be answered. The launch mirrors the user's vim setup (Mason jdtls, lombok agent, `~/.config/nvim/bin/lsp-watchdog`, workspace `~/.cache/4coder/jdtls/shared[-N]`); config keys `lsp_java_command`, `java_jdk8_path`, and `user_name` (javadoc author). System headers for it are included at the top of `4coder_vim_bindings.cpp`, before the 4coder macros. Not covered by `bin/test-vim-linux.sh`: test LSP by hand against a small project.
- The default theme is `ship_files/themes/theme-gruber-darker.4coder` (a port of the user's GruberDarker vim colorscheme); file themes are named after their file, so config uses `default_theme_name = "theme-gruber-darker"`.
- The unnamed register is the system clipboard; `"a`-`"z`, `"0`-`"9` live in `vim_registers`. Marks are buffer markers (`vim_buffer_marks` attachment) so they move with edits.

## Code style

Match the surrounding 4coder style: `function` for file-local functions and `global` for globals, `Type_Name` structs/enums with `EnumName_Value` constants, 4 space indent, `if (x){` with `else{` on its own line, `}break;` in switch cases, `// NOTE(name):` comments, and `// TOP` / `// BOTTOM` markers around file contents. Use the existing helpers (`Scratch_Block`, `push_buffer_range`, `buffer_replace_range`, `History_Group`, `view_set_cursor_and_preferred_x`) rather than new utilities.
