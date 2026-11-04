/*
4coder_git.cpp - Git change signs in the margin and hunk commands, like vim-gitgutter.
*/

// TOP

#if !defined(FCODER_GIT_CPP)
#define FCODER_GIT_CPP

// NOTE(git): Buffers are compared against the index (what `git diff` shows), like
// gitgutter. The index version of a file is read once with `git cat-file --filters` and
// again only when .git/index changes. The diff itself runs in process on line hashes
// (common prefix and suffix trimmed, Myers on the rest), so it is redone on every edit
// without starting git.

typedef i32 Git_Hunk_Kind;
enum{
    GitHunk_Added,
    GitHunk_Modified,
    GitHunk_Removed,
};

struct Git_Hunk{
    Git_Hunk_Kind kind;
    i64 base_first;
    i64 base_count;
    i64 first;
    i64 count;
};

struct Git_Buffer{
    Git_Buffer *next;
    Buffer_ID buffer;
    String_Const_u8 root;
    String_Const_u8 path;
    b32 tracked;
    u64 index_time;
    u64 checked_time;
    Arena base_arena;
    String_Const_u8 *base_lines;
    u64 *base_hashes;
    i64 base_line_count;
    History_Record_Index diffed_history;
    i64 diffed_size;
    b32 diffed;
    Arena hunk_arena;
    Git_Hunk *hunks;
    i32 hunk_count;
    i64 added;
    i64 modified;
    i64 removed;
    String_Const_u8 branch;
};

global Arena git_arena = {};
global Git_Buffer *git_first_buffer = 0;

CUSTOM_ID(colors, defcolor_git_added);
CUSTOM_ID(colors, defcolor_git_modified);
CUSTOM_ID(colors, defcolor_git_removed);
CUSTOM_ID(colors, defcolor_git_bar_added);
CUSTOM_ID(colors, defcolor_git_bar_modified);
CUSTOM_ID(colors, defcolor_git_bar_removed);

////////////////////////////////
// NOTE(git): Lines

function u64
git_hash_line(String_Const_u8 line){
    u64 hash = 14695981039346656037ull;
    for (u64 i = 0; i < line.size; i += 1){
        hash ^= line.str[i];
        hash *= 1099511628211ull;
    }
    return(hash);
}

// NOTE(git): Splits text into lines without their LF or CR LF; a final line break does not
// start another line.
function i64
git_split_lines(Arena *arena, String_Const_u8 text, String_Const_u8 **lines_out, u64 **hashes_out){
    i64 count = 0;
    for (u64 i = 0; i < text.size; i += 1){
        if (text.str[i] == '\n'){
            count += 1;
        }
    }
    if (text.size > 0 && text.str[text.size - 1] != '\n'){
        count += 1;
    }
    String_Const_u8 *lines = push_array(arena, String_Const_u8, count + 1);
    u64 *hashes = push_array(arena, u64, count + 1);
    i64 line = 0;
    u64 start = 0;
    for (u64 i = 0; i <= text.size && line < count; i += 1){
        if (i == text.size || text.str[i] == '\n'){
            u64 end = i;
            if (end > start && text.str[end - 1] == '\r'){
                end -= 1;
            }
            lines[line] = SCu8(text.str + start, end - start);
            hashes[line] = git_hash_line(lines[line]);
            line += 1;
            start = i + 1;
        }
    }
    *lines_out = lines;
    *hashes_out = hashes;
    return(count);
}

////////////////////////////////
// NOTE(git): Diff

struct Git_Edit{
    b32 insert;
    i64 x;
    i64 y;
};

// NOTE(git): Myers' O(ND) diff, recording each step's V so the edit path can be traced
// back. Gives up (returns false) past max_d edits; the caller marks the middle changed.
function b32
git_myers(Arena *arena, u64 *a, i64 n, u64 *b, i64 m, i64 max_d, Git_Edit **edits_out, i64 *edit_count_out){
    i64 offset = max_d + 1;
    i64 *v = push_array_zero(arena, i64, 2*offset + 2);
    i64 **trace = push_array(arena, i64*, max_d + 1);
    i64 found_d = -1;
    for (i64 d = 0; d <= max_d && found_d < 0; d += 1){
        for (i64 k = -d; k <= d; k += 2){
            i64 x = 0;
            if (k == -d || (k != d && v[offset + k - 1] < v[offset + k + 1])){
                x = v[offset + k + 1];
            }
            else{
                x = v[offset + k - 1] + 1;
            }
            i64 y = x - k;
            for (;x < n && y < m && a[x] == b[y]; x += 1, y += 1);
            v[offset + k] = x;
            if (x >= n && y >= m){
                found_d = d;
                break;
            }
        }
        trace[d] = push_array(arena, i64, 2*d + 1);
        for (i64 k = -d; k <= d; k += 1){
            trace[d][k + d] = v[offset + k];
        }
    }
    if (found_d < 0){
        return(false);
    }

    Git_Edit *edits = push_array(arena, Git_Edit, found_d + 1);
    i64 edit_count = 0;
    i64 x = n;
    i64 y = m;
    for (i64 d = found_d; d > 0; d -= 1){
        i64 *prev = trace[d - 1];
        i64 k = x - y;
        i64 prev_k = 0;
        if (k == -d || (k != d && prev[k - 1 + (d - 1)] < prev[k + 1 + (d - 1)])){
            prev_k = k + 1;
        }
        else{
            prev_k = k - 1;
        }
        i64 prev_x = prev[prev_k + (d - 1)];
        i64 prev_y = prev_x - prev_k;
        for (;x > prev_x && y > prev_y; x -= 1, y -= 1);
        edits[edit_count].insert = (x == prev_x);
        edits[edit_count].x = prev_x;
        edits[edit_count].y = prev_y;
        edit_count += 1;
        x = prev_x;
        y = prev_y;
    }
    for (i64 i = 0; i < edit_count/2; i += 1){
        Swap(Git_Edit, edits[i], edits[edit_count - 1 - i]);
    }
    *edits_out = edits;
    *edit_count_out = edit_count;
    return(true);
}

function void
git_push_hunk(Git_Buffer *state, Git_Hunk *hunks, i64 base_first, i64 base_count, i64 first, i64 count){
    Git_Hunk *hunk = &hunks[state->hunk_count++];
    hunk->base_first = base_first;
    hunk->base_count = base_count;
    hunk->first = first;
    hunk->count = count;
    if (base_count == 0){
        hunk->kind = GitHunk_Added;
        state->added += count;
    }
    else if (count == 0){
        hunk->kind = GitHunk_Removed;
        state->removed += base_count;
    }
    else{
        // NOTE(git): Like gitgutter, a block that changed n lines and grew counts the extra
        // lines as added, one that shrank counts the missing ones as removed.
        hunk->kind = GitHunk_Modified;
        state->modified += Min(count, base_count);
        state->added += clamp_bot(0, count - base_count);
        state->removed += clamp_bot(0, base_count - count);
    }
}

function void
git_diff_buffer(Application_Links *app, Git_Buffer *state){
    Scratch_Block scratch(app);
    String_Const_u8 text = push_whole_buffer(app, scratch, state->buffer);
    String_Const_u8 *lines = 0;
    u64 *hashes = 0;
    i64 line_count = git_split_lines(scratch, text, &lines, &hashes);

    u64 *a = state->base_hashes;
    i64 n = state->base_line_count;
    i64 prefix = 0;
    for (;prefix < n && prefix < line_count && a[prefix] == hashes[prefix]; prefix += 1);
    i64 suffix = 0;
    for (;suffix < n - prefix && suffix < line_count - prefix &&
         a[n - 1 - suffix] == hashes[line_count - 1 - suffix]; suffix += 1);
    i64 mid_n = n - prefix - suffix;
    i64 mid_m = line_count - prefix - suffix;

    if (state->hunk_arena.base_allocator == 0){
        state->hunk_arena = make_arena_system();
    }
    linalloc_clear(&state->hunk_arena);
    state->hunk_count = 0;
    state->added = 0;
    state->modified = 0;
    state->removed = 0;

    Git_Edit *edits = 0;
    i64 edit_count = 0;
    b32 diffed = (mid_n == 0 || mid_m == 0 ||
                  git_myers(scratch, a + prefix, mid_n, hashes + prefix, mid_m, 1000, &edits, &edit_count));
    if (mid_n == 0 && mid_m == 0){
        state->hunks = 0;
        return;
    }
    if (!diffed || mid_n == 0 || mid_m == 0){
        state->hunks = push_array(&state->hunk_arena, Git_Hunk, 1);
        git_push_hunk(state, state->hunks, prefix, mid_n, prefix, mid_m);
        return;
    }

    // NOTE(git): Runs of edits with no unchanged line between them form one hunk.
    state->hunks = push_array(&state->hunk_arena, Git_Hunk, edit_count);
    for (i64 i = 0; i < edit_count;){
        i64 x0 = edits[i].x;
        i64 y0 = edits[i].y;
        i64 x1 = x0;
        i64 y1 = y0;
        for (;i < edit_count && edits[i].x == x1 && edits[i].y == y1; i += 1){
            if (edits[i].insert){
                y1 += 1;
            }
            else{
                x1 += 1;
            }
        }
        git_push_hunk(state, state->hunks, prefix + x0, x1 - x0, prefix + y0, y1 - y0);
    }
}

////////////////////////////////
// NOTE(git): Repository access

function String_Const_u8
git_run(Arena *arena, String_Const_u8 command){
    return(fuzzy_run_command(arena, command));
}

function void
git_load_base(Application_Links *app, Git_Buffer *state){
    Scratch_Block scratch(app);
    String_Const_u8 root_q = fuzzy_shell_quote(scratch, state->root);
    String_Const_u8 path_q = fuzzy_shell_quote(scratch, state->path);
    String_Const_u8 check = push_u8_stringf(scratch, "git -C %.*s ls-files --error-unmatch -- %.*s > /dev/null 2>&1",
                                            string_expand(root_q), string_expand(path_q));
    state->tracked = (system((char*)check.str) == 0);

    if (state->base_arena.base_allocator == 0){
        state->base_arena = make_arena_system();
    }
    linalloc_clear(&state->base_arena);
    state->base_line_count = 0;
    if (state->tracked){
        String_Const_u8 show = push_u8_stringf(scratch, "git -C %.*s cat-file --filters %.*s 2>/dev/null",
                                               string_expand(root_q),
                                               string_expand(fuzzy_shell_quote(scratch, push_u8_stringf(scratch, ":%.*s", string_expand(state->path)))));
        String_Const_u8 text = push_string_copy(&state->base_arena, git_run(scratch, show));
        state->base_line_count = git_split_lines(&state->base_arena, text, &state->base_lines, &state->base_hashes);

        String_Const_u8 branch = git_run(scratch, push_u8_stringf(scratch, "git -C %.*s rev-parse --abbrev-ref HEAD 2>/dev/null",
                                                                   string_expand(root_q)));
        state->branch = push_string_copy(&state->base_arena, string_skip_chop_whitespace(branch));
    }
    state->diffed = false;
}

function u64
git_index_time(Application_Links *app, String_Const_u8 root){
    Scratch_Block scratch(app);
    String_Const_u8 index = push_u8_stringf(scratch, "%.*s/.git/index", string_expand(root));
    return(system_quick_file_attributes(scratch, index).last_write_time);
}

function Git_Buffer*
git_buffer_state(Buffer_ID buffer){
    Git_Buffer *result = 0;
    for (Git_Buffer *state = git_first_buffer; state != 0; state = state->next){
        if (state->buffer == buffer){
            result = state;
            break;
        }
    }
    return(result);
}

function Git_Buffer*
git_track_buffer(Application_Links *app, Buffer_ID buffer){
    Scratch_Block scratch(app);
    String_Const_u8 file_name = push_buffer_file_name(app, scratch, buffer);
    if (file_name.size == 0){
        return(0);
    }
    String_Const_u8 git = push_file_search_up_path(app, scratch, string_remove_last_folder(file_name), string_u8_litexpr(".git"));
    if (git.size == 0){
        return(0);
    }
    if (git_arena.base_allocator == 0){
        git_arena = make_arena_system();
    }
    String_Const_u8 root = string_remove_last_folder(git);
    for (;root.size > 1 && character_is_slash(string_get_character(root, root.size - 1));){
        root = string_chop(root, 1);
    }
    Git_Buffer *state = push_array_zero(&git_arena, Git_Buffer, 1);
    state->buffer = buffer;
    state->root = push_string_copy(&git_arena, root);
    state->path = push_string_copy(&git_arena, fuzzy_relative_path(root, file_name));
    state->index_time = git_index_time(app, state->root);
    git_load_base(app, state);
    sll_stack_push(git_first_buffer, state);
    return(state);
}

// NOTE(git): Keeps the signs of every visible buffer current; called from the tick hook.
function void
git_update(Application_Links *app){
    u64 now = system_now_time();
    for (View_ID view = get_view_next(app, 0, Access_Always);
         view != 0;
         view = get_view_next(app, view, Access_Always)){
        Buffer_ID buffer = view_get_buffer(app, view, Access_Always);
        Git_Buffer *state = git_buffer_state(buffer);
        if (state == 0){
            if (buffer_has_name_with_star(app, buffer)){
                continue;
            }
            state = git_track_buffer(app, buffer);
            if (state == 0){
                // NOTE(git): Remember files outside a repository so they are not looked up again.
                if (git_arena.base_allocator == 0){
                    git_arena = make_arena_system();
                }
                state = push_array_zero(&git_arena, Git_Buffer, 1);
                state->buffer = buffer;
                sll_stack_push(git_first_buffer, state);
                continue;
            }
        }
        if (state->root.size == 0){
            continue;
        }
        if (now - state->checked_time > 1000000){
            state->checked_time = now;
            u64 index_time = git_index_time(app, state->root);
            if (index_time != state->index_time){
                state->index_time = index_time;
                git_load_base(app, state);
            }
        }
        if (!state->tracked){
            continue;
        }
        History_Record_Index history = buffer_history_get_current_state_index(app, buffer);
        i64 size = buffer_get_size(app, buffer);
        if (!state->diffed || history != state->diffed_history || size != state->diffed_size){
            state->diffed = true;
            state->diffed_history = history;
            state->diffed_size = size;
            git_diff_buffer(app, state);
        }
    }
}

////////////////////////////////
// NOTE(git): Drawing

function ARGB_Color
git_theme_color(Managed_ID id, ARGB_Color fallback){
    ARGB_Color result = fcolor_resolve(fcolor_id(id));
    if (result == 0 || result == 0xFF990099){
        result = fallback;
    }
    return(result);
}

function b32
git_buffer_has_signs(Buffer_ID buffer){
    Git_Buffer *state = git_buffer_state(buffer);
    return(state != 0 && state->tracked);
}

// NOTE(git): Bars for added and changed lines, a notch where lines were removed.
function void
git_draw_signs(Application_Links *app, Buffer_ID buffer, Text_Layout_ID text_layout_id, Rect_f32 column){
    Git_Buffer *state = git_buffer_state(buffer);
    if (state == 0 || !state->tracked || state->hunk_count == 0){
        return;
    }
    Range_i64 visible = text_layout_get_visible_range(app, text_layout_id);
    i64 first_line = get_line_number_from_pos(app, buffer, visible.min);
    i64 last_line = get_line_number_from_pos(app, buffer, visible.max);
    ARGB_Color added = git_theme_color(defcolor_git_added, 0xFF549159);
    ARGB_Color modified = git_theme_color(defcolor_git_modified, 0xFF375FAD);
    ARGB_Color removed = git_theme_color(defcolor_git_removed, 0xFFF75464);
    f32 x0 = column.x0 + 1.f;
    f32 x1 = column.x1 - 1.f;
    for (i32 i = 0; i < state->hunk_count; i += 1){
        Git_Hunk *hunk = &state->hunks[i];
        if (hunk->kind == GitHunk_Removed){
            i64 line = clamp_bot(1, hunk->first);
            if (first_line <= line && line <= last_line){
                Range_f32 rows = text_layout_line_on_screen(app, text_layout_id, line);
                f32 y = (hunk->first == 0)?rows.min:rows.max;
                draw_rectangle(app, Rf32(column.x0, y - 2.f, column.x1, y + 2.f), 0.f, removed);
            }
        }
        else{
            i64 changed = (hunk->kind == GitHunk_Added)?0:Min(hunk->count, hunk->base_count);
            i64 a = clamp_bot(first_line, hunk->first + 1);
            i64 b = clamp_top(hunk->first + hunk->count, last_line);
            for (i64 line = a; line <= b; line += 1){
                ARGB_Color color = (line - hunk->first <= changed)?modified:added;
                Range_f32 rows = text_layout_line_on_screen(app, text_layout_id, line);
                draw_rectangle(app, Rf32(x0, rows.min, x1, rows.max), 0.f, color);
            }
            i64 last = hunk->first + hunk->count;
            if (hunk->kind == GitHunk_Modified && hunk->base_count > hunk->count &&
                first_line <= last && last <= last_line){
                Range_f32 rows = text_layout_line_on_screen(app, text_layout_id, last);
                draw_rectangle(app, Rf32(column.x0, rows.max - 2.f, column.x1, rows.max + 2.f), 0.f, removed);
            }
        }
    }
}

function String_Const_u8
git_status_label(Application_Links *app, Arena *arena, Buffer_ID buffer){
    String_Const_u8 result = {};
    Git_Buffer *state = git_buffer_state(buffer);
    if (state != 0 && state->tracked){
        if (state->hunk_count > 0){
            result = push_u8_stringf(arena, " %.*s +%lld ~%lld -%lld ", string_expand(state->branch),
                                     state->added, state->modified, state->removed);
        }
        else{
            result = push_u8_stringf(arena, " %.*s ", string_expand(state->branch));
        }
    }
    return(result);
}

////////////////////////////////
// NOTE(git): Hunk commands

function Git_Buffer*
git_active_state(Application_Links *app, View_ID *view_out, Buffer_ID *buffer_out){
    View_ID view = get_active_view(app, Access_ReadVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadVisible);
    Git_Buffer *state = git_buffer_state(buffer);
    if (state == 0 || !state->tracked){
        state = 0;
        print_message(app, string_u8_litexpr("git: this file is not tracked by git\n"));
    }
    *view_out = view;
    *buffer_out = buffer;
    return(state);
}

function Git_Hunk*
git_hunk_at_cursor(Application_Links *app, Git_Buffer *state, View_ID view, Buffer_ID buffer){
    i64 line = get_line_number_from_pos(app, buffer, view_get_cursor_pos(app, view)) - 1;
    Git_Hunk *result = 0;
    for (i32 i = 0; i < state->hunk_count; i += 1){
        Git_Hunk *hunk = &state->hunks[i];
        i64 last = hunk->first + clamp_bot(1, hunk->count) - 1;
        if (hunk->kind == GitHunk_Removed){
            last = hunk->first;
        }
        if (hunk->first - ((hunk->kind == GitHunk_Removed)?1:0) <= line && line <= last){
            result = hunk;
            break;
        }
    }
    if (result == 0){
        print_message(app, string_u8_litexpr("git: no change on this line\n"));
    }
    return(result);
}

function void
git_goto_hunk(Application_Links *app, b32 forward){
    View_ID view = 0;
    Buffer_ID buffer = 0;
    Git_Buffer *state = git_active_state(app, &view, &buffer);
    if (state == 0 || state->hunk_count == 0){
        return;
    }
    i64 line = get_line_number_from_pos(app, buffer, view_get_cursor_pos(app, view)) - 1;
    Git_Hunk *target = 0;
    if (forward){
        for (i32 i = 0; i < state->hunk_count && target == 0; i += 1){
            if (state->hunks[i].first > line){
                target = &state->hunks[i];
            }
        }
        if (target == 0){
            target = &state->hunks[0];
        }
    }
    else{
        for (i32 i = state->hunk_count - 1; i >= 0 && target == 0; i -= 1){
            if (state->hunks[i].first < line){
                target = &state->hunks[i];
            }
        }
        if (target == 0){
            target = &state->hunks[state->hunk_count - 1];
        }
    }
    point_stack_push_view_cursor(app, view);
    i64 target_line = clamp_bot(1, target->first + ((target->kind == GitHunk_Removed)?0:1));
    view_set_cursor_and_preferred_x(app, view, seek_pos(get_pos_past_lead_whitespace_from_line_number(app, buffer, target_line)));
}

CUSTOM_COMMAND_SIG(git_next_hunk)
CUSTOM_DOC("Moves to the next changed block of the file compared to the git index.")
{
    git_goto_hunk(app, true);
}

CUSTOM_COMMAND_SIG(git_prev_hunk)
CUSTOM_DOC("Moves to the previous changed block of the file compared to the git index.")
{
    git_goto_hunk(app, false);
}

CUSTOM_COMMAND_SIG(git_preview_hunk)
CUSTOM_DOC("Shows the change under the cursor as a diff in a popup.")
{
    View_ID view = 0;
    Buffer_ID buffer = 0;
    Git_Buffer *state = git_active_state(app, &view, &buffer);
    Git_Hunk *hunk = (state != 0)?git_hunk_at_cursor(app, state, view, buffer):0;
    if (hunk == 0){
        return;
    }
    Scratch_Block scratch(app);
    List_String_Const_u8 lines = {};
    for (i64 i = 0; i < hunk->base_count; i += 1){
        string_list_pushf(scratch, &lines, "-%.*s", string_expand(state->base_lines[hunk->base_first + i]));
    }
    for (i64 i = 0; i < hunk->count; i += 1){
        string_list_pushf(scratch, &lines, "+%.*s", string_expand(push_buffer_line(app, scratch, buffer, hunk->first + 1 + i)));
    }
    lsp_show_popup(app, view, string_list_flatten(scratch, lines, 0, string_u8_litexpr("\n"), 0, StringFill_NoTerminate));
}

CUSTOM_COMMAND_SIG(git_reset_hunk)
CUSTOM_DOC("Puts back the git index version of the change under the cursor.")
{
    View_ID view = 0;
    Buffer_ID buffer = 0;
    Git_Buffer *state = git_active_state(app, &view, &buffer);
    Git_Hunk *hunk = (state != 0)?git_hunk_at_cursor(app, state, view, buffer):0;
    if (hunk == 0){
        return;
    }
    Scratch_Block scratch(app);
    List_String_Const_u8 lines = {};
    for (i64 i = 0; i < hunk->base_count; i += 1){
        string_list_pushf(scratch, &lines, "%.*s\n", string_expand(state->base_lines[hunk->base_first + i]));
    }
    String_Const_u8 text = string_list_flatten(scratch, lines, 0, string_u8_empty, 0, StringFill_NoTerminate);
    i64 line_count = buffer_get_line_count(app, buffer);
    Range_i64 range = Ii64(get_line_start_pos(app, buffer, hunk->first + 1));
    if (hunk->count > 0){
        i64 last = hunk->first + hunk->count;
        range.max = (last < line_count)?get_line_start_pos(app, buffer, last + 1):buffer_get_size(app, buffer);
    }
    buffer_replace_range(app, buffer, range, text);
}

// NOTE(git): Builds a zero context patch for the hunk and applies it to the index.
CUSTOM_COMMAND_SIG(git_stage_hunk)
CUSTOM_DOC("Stages the change under the cursor (git add for just that block).")
{
    View_ID view = 0;
    Buffer_ID buffer = 0;
    Git_Buffer *state = git_active_state(app, &view, &buffer);
    Git_Hunk *hunk = (state != 0)?git_hunk_at_cursor(app, state, view, buffer):0;
    if (hunk == 0){
        return;
    }
    Scratch_Block scratch(app);
    i64 old_start = (hunk->base_count > 0)?(hunk->base_first + 1):hunk->base_first;
    i64 new_start = (hunk->count > 0)?(hunk->base_first + 1):hunk->base_first;
    List_String_Const_u8 patch = {};
    string_list_pushf(scratch, &patch, "--- a/%.*s\n+++ b/%.*s\n@@ -%lld,%lld +%lld,%lld @@\n",
                      string_expand(state->path), string_expand(state->path),
                      old_start, hunk->base_count, new_start, hunk->count);
    for (i64 i = 0; i < hunk->base_count; i += 1){
        string_list_pushf(scratch, &patch, "-%.*s\n", string_expand(state->base_lines[hunk->base_first + i]));
    }
    for (i64 i = 0; i < hunk->count; i += 1){
        String_Const_u8 line = push_buffer_line(app, scratch, buffer, hunk->first + 1 + i);
        if (line.size > 0 && line.str[line.size - 1] == '\r'){
            line = string_chop(line, 1);
        }
        string_list_pushf(scratch, &patch, "+%.*s\n", string_expand(line));
    }
    String_Const_u8 text = string_list_flatten(scratch, patch, 0, string_u8_empty, 0, StringFill_NoTerminate);
    String_Const_u8 command = push_u8_stringf(scratch, "git -C %.*s apply --cached --unidiff-zero - 2>/dev/null",
                                              string_expand(fuzzy_shell_quote(scratch, state->root)));
#if OS_LINUX || OS_MAC
    FILE *pipe = popen((char*)command.str, "w");
    if (pipe != 0){
        fwrite(text.str, 1, text.size, pipe);
        if (pclose(pipe) != 0){
            print_message(app, string_u8_litexpr("git: could not stage the hunk\n"));
        }
    }
#endif
    state->checked_time = 0;
}

CUSTOM_UI_COMMAND_SIG(git_list_hunks)
CUSTOM_DOC("Lists the changed blocks of the file with fzf style filtering.")
{
    View_ID view = 0;
    Buffer_ID buffer = 0;
    Git_Buffer *state = git_active_state(app, &view, &buffer);
    if (state == 0 || state->hunk_count == 0){
        return;
    }
    Scratch_Block scratch(app);
    Lister_Block lister(app, scratch);
    lister_set_query(lister, "Changes:");
    lister_set_default_handlers(lister);
    ((Lister*)lister)->fuzzy_filter = true;
    for (i32 i = 0; i < state->hunk_count; i += 1){
        Git_Hunk *hunk = &state->hunks[i];
        i64 line = clamp_bot(1, hunk->first + ((hunk->kind == GitHunk_Removed)?0:1));
        String_Const_u8 kind = string_u8_litexpr("removed");
        if (hunk->kind == GitHunk_Added){
            kind = string_u8_litexpr("added");
        }
        else if (hunk->kind == GitHunk_Modified){
            kind = string_u8_litexpr("changed");
        }
        String_Const_u8 text = string_skip_chop_whitespace(push_buffer_line(app, scratch, buffer, line));
        lister_add_item(lister, push_u8_stringf(scratch, "%lld: %.*s", line, string_expand(text)), kind, IntAsPtr(line), 0);
    }
    Lister_Result l_result = run_lister(app, lister);
    if (!l_result.canceled){
        point_stack_push_view_cursor(app, view);
        view_set_cursor_and_preferred_x(app, view, seek_pos(get_pos_past_lead_whitespace_from_line_number(app, buffer, PtrAsInt(l_result.user_data))));
    }
}

CUSTOM_UI_COMMAND_SIG(git_status_files)
CUSTOM_DOC("Lists the changed and untracked files of the repository with fzf style filtering and opens the chosen one.")
{
    Scratch_Block scratch(app);
    String_Const_u8 root = fuzzy_search_root(app, scratch);
    String_Const_u8 output = git_run(scratch, push_u8_stringf(scratch, "git -C %.*s status --porcelain 2>/dev/null",
                                                               string_expand(fuzzy_shell_quote(scratch, root))));
    List_String_Const_u8 lines = string_split(scratch, output, (u8*)"\n", 1);
    if (lines.node_count == 0){
        print_message(app, string_u8_litexpr("git: no changes\n"));
        return;
    }
    String_Const_u8 *paths = push_array(scratch, String_Const_u8, lines.node_count);
    Lister_Block lister(app, scratch);
    lister_set_query(lister, push_u8_stringf(scratch, "git status %.*s:", string_expand(string_front_of_path(root))));
    lister_set_default_handlers(lister);
    ((Lister*)lister)->fuzzy_filter = true;
    i32 count = 0;
    for (Node_String_Const_u8 *node = lines.first; node != 0; node = node->next){
        if (node->string.size > 3){
            String_Const_u8 path = string_skip(node->string, 3);
            u64 arrow = string_find_first(path, string_u8_litexpr(" -> "));
            if (arrow < path.size){
                path = string_skip(path, arrow + 4);
            }
            paths[count] = push_u8_stringf(scratch, "%.*s/%.*s", string_expand(root), string_expand(path));
            lister_add_item(lister, path, string_prefix(node->string, 2), IntAsPtr(count), 0);
            count += 1;
        }
    }
    Lister_Result l_result = run_lister(app, lister);
    if (!l_result.canceled){
        View_ID view = get_active_view(app, Access_Always);
        view_open_file(app, view, paths[PtrAsInt(l_result.user_data)], true);
    }
}

CUSTOM_COMMAND_SIG(git_blame_line)
CUSTOM_DOC("Shows who last changed the cursor line, when and why, in a popup.")
{
    View_ID view = 0;
    Buffer_ID buffer = 0;
    Git_Buffer *state = git_active_state(app, &view, &buffer);
    if (state == 0){
        return;
    }
    Scratch_Block scratch(app);
    i64 line = get_line_number_from_pos(app, buffer, view_get_cursor_pos(app, view));
    // NOTE(git): Blame the buffer as it is now, unsaved edits included.
    String_Const_u8 temp = push_u8_stringf(scratch, "/tmp/4coder-blame-%d", (i32)getpid());
    FILE *file = fopen((char*)temp.str, "wb");
    if (file == 0){
        return;
    }
    String_Const_u8 text = push_whole_buffer(app, scratch, buffer);
    fwrite(text.str, 1, text.size, file);
    fclose(file);
    String_Const_u8 output = git_run(scratch, push_u8_stringf(scratch, "git -C %.*s blame -L %lld,%lld --porcelain --contents %.*s -- %.*s 2>/dev/null",
                                                               string_expand(fuzzy_shell_quote(scratch, state->root)), line, line,
                                                               string_expand(fuzzy_shell_quote(scratch, temp)),
                                                               string_expand(fuzzy_shell_quote(scratch, state->path))));
    remove((char*)temp.str);
    String_Const_u8 hash = {};
    String_Const_u8 author = {};
    String_Const_u8 summary = {};
    i64 time = 0;
    List_String_Const_u8 lines = string_split(scratch, output, (u8*)"\n", 1);
    for (Node_String_Const_u8 *node = lines.first; node != 0; node = node->next){
        String_Const_u8 l = node->string;
        if (hash.size == 0){
            hash = string_prefix(l, 8);
        }
        else if (string_match(string_prefix(l, 7), string_u8_litexpr("author "))){
            author = string_skip(l, 7);
        }
        else if (string_match(string_prefix(l, 12), string_u8_litexpr("author-time "))){
            time = (i64)string_to_integer(string_skip(l, 12), 10);
        }
        else if (string_match(string_prefix(l, 8), string_u8_litexpr("summary "))){
            summary = string_skip(l, 8);
        }
    }
    if (hash.size == 0){
        print_message(app, string_u8_litexpr("git: no blame for this line\n"));
        return;
    }
    char date[32] = {};
    time_t t = (time_t)time;
    struct tm *local = localtime(&t);
    if (local != 0){
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M", local);
    }
    String_Const_u8 message = push_u8_stringf(scratch, "%.*s %.*s, %s\n%.*s", string_expand(hash), string_expand(author), date, string_expand(summary));
    if (string_match(hash, string_u8_litexpr("00000000"))){
        message = string_u8_litexpr("Not committed yet");
    }
    lsp_show_popup(app, view, message);
}

// NOTE(git): Runs a terminal program in a new Ghostty window in dir (empty program: a shell).
// Single instance mode is turned off so the window gets this directory and program even when
// another Ghostty is already running.
function void
git_spawn_terminal(Application_Links *app, String_Const_u8 dir, String_Const_u8 program){
    Scratch_Block scratch(app);
    String_Const_u8 dir_q = fuzzy_shell_quote(scratch, dir);
    String_Const_u8 command = push_u8_stringf(scratch, "(cd %.*s && setsid ghostty --gtk-single-instance=false --working-directory=%.*s%s%.*s > /dev/null 2>&1 &)",
                                              string_expand(dir_q), string_expand(dir_q),
                                              (program.size > 0)?" -e ":"", string_expand(program));
    if (system((char*)command.str) != 0){
        print_message(app, string_u8_litexpr("git: could not start ghostty\n"));
    }
}

CUSTOM_COMMAND_SIG(git_lazygit)
CUSTOM_DOC("Opens lazygit for the current repository in a new Ghostty window.")
{
    Scratch_Block scratch(app);
    git_spawn_terminal(app, fuzzy_search_root(app, scratch), string_u8_litexpr("lazygit"));
}

CUSTOM_COMMAND_SIG(git_lazygit_log)
CUSTOM_DOC("Opens the lazygit log of the current repository in a new Ghostty window.")
{
    Scratch_Block scratch(app);
    git_spawn_terminal(app, fuzzy_search_root(app, scratch), string_u8_litexpr("lazygit log"));
}

CUSTOM_COMMAND_SIG(git_lazygit_log_cwd)
CUSTOM_DOC("Opens the lazygit log of the working directory in a new Ghostty window.")
{
    Scratch_Block scratch(app);
    git_spawn_terminal(app, system_get_path(scratch, SystemPath_CurrentDirectory), string_u8_litexpr("lazygit log"));
}

CUSTOM_COMMAND_SIG(git_lazygit_file)
CUSTOM_DOC("Opens lazygit with the history of the current file in a new Ghostty window.")
{
    Scratch_Block scratch(app);
    View_ID view = get_active_view(app, Access_Always);
    String_Const_u8 file_name = push_buffer_file_name(app, scratch, view_get_buffer(app, view, Access_Always));
    git_spawn_terminal(app, fuzzy_search_root(app, scratch),
                    push_u8_stringf(scratch, "lazygit -f %.*s", string_expand(fuzzy_shell_quote(scratch, file_name))));
}

CUSTOM_COMMAND_SIG(open_terminal_root)
CUSTOM_DOC("Opens a Ghostty terminal in the current repository (or the working directory).")
{
    Scratch_Block scratch(app);
    git_spawn_terminal(app, fuzzy_search_root(app, scratch), string_u8_empty);
}

CUSTOM_COMMAND_SIG(open_terminal_cwd)
CUSTOM_DOC("Opens a Ghostty terminal in 4coder's working directory.")
{
    Scratch_Block scratch(app);
    git_spawn_terminal(app, system_get_path(scratch, SystemPath_CurrentDirectory), string_u8_empty);
}

#endif //FCODER_GIT_CPP

// BOTTOM
