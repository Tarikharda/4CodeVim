/*
4coder_vim.cpp - A modal, vim style editing layer built on top of the default framework.
*/

// TOP

#if !defined(FCODER_VIM_CPP)
#define FCODER_VIM_CPP

// NOTE(vim): Normal and visual mode keys are interpreted from text input events, so
// shifted symbols like $ % { work regardless of keyboard layout. Commands that need
// more keys (operators, f/t/r, g/z prefixes, registers) read them with vim_read_char.
// Insert mode falls through to the regular 4coder bindings, so typing, auto indent
// and word completion behave as they do without the vim layer.
//
// The . command does not replay input events. The keys a change consumed are captured
// as characters, the text typed in insert mode is captured from the buffer on <esc>,
// and repeating feeds both back through vim_execute synchronously. Macros (q, @) do
// replay input events, taken from the *keyboard* log the core writes.

typedef i32 Vim_Mode;
enum{
    VimMode_Normal,
    VimMode_Insert,
    VimMode_Replace,
    VimMode_Visual,
    VimMode_VisualLine,
};

struct Vim_Motion{
    b32 ok;
    i64 pos;
    b32 linewise;
    b32 inclusive;
    b32 jump;
};

struct Vim_Register{
    String_Const_u8 text;
    u64 cap;
    b32 linewise;
};

#define VimCtrl(c) ((u8)((c) - 'a' + 1))

#define VimRegisterCount 36
#define VimRegisterYank 26
#define VimRegisterDelete 27

#define VimMarkCount 27
#define VimMarkLastJump 26

CUSTOM_ID(attachment, vim_buffer_marks);

struct Vim_Buffer_Marks{
    Managed_Object markers;
    u32 set_flags;
};

global Vim_Mode vim_mode = VimMode_Normal;

global Mapping vim_mapping = {};
global String_ID vim_map_normal = 0;
global String_ID vim_map_visual = 0;
global String_ID vim_map_insert = 0;
global String_ID vim_map_replace = 0;

global History_Group vim_change_history = {};
global b32 vim_change_history_active = false;

// NOTE(vim): The unnamed register is the system clipboard; these remember whether
// the text we last put there was a linewise yank.
global b32 vim_register_linewise = false;
global u64 vim_register_size = 0;
global Vim_Register vim_registers[VimRegisterCount] = {};
global u8 vim_pending_register = 0;

global u8 vim_find_kind = 0;
global u8 vim_find_char = 0;

global u8 vim_search_space[256];
global String_Const_u8 vim_search = {};
global b32 vim_search_backward = false;
global b32 vim_search_whole_word = false;
global b32 vim_search_highlight = false;
global b32 vim_hlsearch = true;

global i64 vim_insert_count = 1;
global i64 vim_insert_start = 0;
global Buffer_ID vim_insert_buffer = 0;
global i64 vim_last_insert_pos = 0;
global Buffer_ID vim_last_insert_buffer = 0;

global Buffer_ID vim_last_visual_buffer = 0;
global i64 vim_last_visual_mark = 0;
global i64 vim_last_visual_cursor = 0;
global Vim_Mode vim_last_visual_mode = VimMode_Visual;

global u8 vim_capture_space[512];
global u64 vim_capture_size = 0;
global b32 vim_capture_overflow = false;
global u8 vim_queue_space[512];
global u64 vim_queue_size = 0;
global u64 vim_queue_pos = 0;
global b32 vim_replaying = false;

global u8 vim_dot_space[512];
global u64 vim_dot_size = 0;
global b32 vim_dot_pending = false;
global u8 vim_dot_pending_space[512];
global u64 vim_dot_pending_size = 0;
global Vim_Register vim_dot_insert = {};

global u8 vim_macro_recording = 0;
global i64 vim_macro_start = 0;
global u8 vim_macro_last = 0;
global Vim_Register vim_macros[26] = {};

global u8 vim_last_ex_space[512];
global String_Const_u8 vim_last_ex = {};

////////////////////////////////
// NOTE(vim): Character and line helpers

function b32
vim_is_space(u8 c){
    return(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v');
}

function i32
vim_char_class(u8 c, b32 big){
    i32 result = 2;
    if (vim_is_space(c)){
        result = 0;
    }
    else if (big || character_is_alpha_numeric(c) || c == '_' || c >= 128){
        result = 1;
    }
    return(result);
}

function b32
vim_mode_is_visual(Vim_Mode mode){
    return(mode == VimMode_Visual || mode == VimMode_VisualLine);
}

function i64
vim_line_of(Application_Links *app, Buffer_ID buffer, i64 pos){
    return(get_line_number_from_pos(app, buffer, pos));
}

// NOTE(vim): One past the last visible character of the line, excluding LF and CR.
function i64
vim_line_end(Application_Links *app, Buffer_ID buffer, i64 line){
    i64 start = get_line_start_pos(app, buffer, line);
    i64 end = get_line_end_pos(app, buffer, line);
    if (end > start && buffer_get_char(app, buffer, end - 1) == '\r'){
        end -= 1;
    }
    return(end);
}

function i64
vim_first_non_blank(Application_Links *app, Buffer_ID buffer, i64 line){
    i64 start = get_line_start_pos(app, buffer, line);
    i64 end = vim_line_end(app, buffer, line);
    i64 pos = start;
    for (; pos < end; pos += 1){
        u8 c = buffer_get_char(app, buffer, pos);
        if (c != ' ' && c != '\t'){
            break;
        }
    }
    return(pos);
}

// NOTE(vim): 4coder counts the empty line after a trailing line break, vim does not.
function i64
vim_line_count(Application_Links *app, Buffer_ID buffer){
    i64 line_count = buffer_get_line_count(app, buffer);
    i64 size = buffer_get_size(app, buffer);
    if (line_count > 1 && size > 0 && buffer_get_char(app, buffer, size - 1) == '\n'){
        line_count -= 1;
    }
    return(line_count);
}

function i64
vim_clamp_line(Application_Links *app, Buffer_ID buffer, i64 line){
    i64 line_count = vim_line_count(app, buffer);
    return(clamp(1, line, clamp_bot(1, line_count)));
}

// NOTE(vim): In normal mode the cursor sits on a character, never past the end of a line.
function i64
vim_clamp_normal_pos(Application_Links *app, Buffer_ID buffer, i64 pos){
    i64 line = vim_line_of(app, buffer, pos);
    i64 start = get_line_start_pos(app, buffer, line);
    i64 end = vim_line_end(app, buffer, line);
    if (pos >= end && end > start){
        pos = end - 1;
    }
    if (pos < start){
        pos = start;
    }
    return(pos);
}

function void
vim_clamp_cursor(Application_Links *app, View_ID view, Buffer_ID buffer){
    i64 pos = view_get_cursor_pos(app, view);
    i64 clamped = vim_clamp_normal_pos(app, buffer, pos);
    if (clamped != pos){
        // NOTE(vim): Keep the preferred x so j/k through short lines return to the column.
        view_set_cursor(app, view, seek_pos(clamped));
    }
}

function void
vim_set_cursor(Application_Links *app, View_ID view, i64 pos){
    view_set_cursor_and_preferred_x(app, view, seek_pos(pos));
}

function void
vim_set_cursor_line(Application_Links *app, View_ID view, Buffer_ID buffer, i64 line){
    line = vim_clamp_line(app, buffer, line);
    vim_set_cursor(app, view, vim_first_non_blank(app, buffer, line));
}

function b32
vim_buffer_is_code(Application_Links *app, Buffer_ID buffer){
    Token_Array tokens = get_token_array_from_buffer(app, buffer);
    return(tokens.tokens != 0);
}

function b32
vim_is_whole_word(Application_Links *app, Buffer_ID buffer, i64 pos, u64 size){
    u8 before = (pos > 0)?buffer_get_char(app, buffer, pos - 1):' ';
    u8 after = buffer_get_char(app, buffer, pos + size);
    return(vim_char_class(before, false) != 1 && vim_char_class(after, false) != 1);
}

////////////////////////////////
// NOTE(vim): Registers

function void
vim_register_set(Vim_Register *reg, String_Const_u8 text, b32 linewise, b32 append){
    u64 old_size = append?reg->text.size:0;
    u64 new_size = old_size + text.size;
    if (new_size > reg->cap){
        Base_Allocator *allocator = get_base_allocator_system();
        String_Const_u8 memory = base_allocate(allocator, clamp_bot(KB(1), new_size*2));
        if (old_size > 0){
            block_copy(memory.str, reg->text.str, old_size);
        }
        if (reg->text.str != 0){
            base_free(allocator, reg->text.str);
        }
        reg->text.str = memory.str;
        reg->cap = memory.size;
    }
    block_copy(reg->text.str + old_size, text.str, text.size);
    reg->text.size = new_size;
    reg->linewise = append?(reg->linewise || linewise):linewise;
}

function Vim_Register*
vim_register_slot(u8 reg){
    Vim_Register *result = 0;
    if ('a' <= reg && reg <= 'z'){
        result = &vim_registers[reg - 'a'];
    }
    else if ('A' <= reg && reg <= 'Z'){
        result = &vim_registers[reg - 'A'];
    }
    else if ('0' <= reg && reg <= '9'){
        result = &vim_registers[VimRegisterYank + (reg - '0')];
    }
    return(result);
}

function b32
vim_register_is_valid(u8 reg){
    return(vim_register_slot(reg) != 0 || reg == '"' || reg == '+' || reg == '*' || reg == '_');
}

// NOTE(vim): Every yank or delete goes to the unnamed register (the system clipboard)
// plus, like vim, "0 for plain yanks, "1-"9 for multi line deletes and "a-"z when asked.
function void
vim_yank(String_Const_u8 string, b32 linewise, b32 is_yank){
    u8 reg = vim_pending_register;
    if (reg == '_' || string.size == 0){
        return;
    }

    Vim_Register *slot = vim_register_slot(reg);
    if (slot != 0){
        b32 append = ('A' <= reg && reg <= 'Z');
        vim_register_set(slot, string, linewise, append);
        string = slot->text;
        linewise = slot->linewise;
    }
    else if (is_yank){
        vim_register_set(&vim_registers[VimRegisterYank], string, linewise, false);
    }

    if (!is_yank && (linewise || string_find_first(string, '\n') < string.size)){
        Vim_Register oldest = vim_registers[VimRegisterCount - 1];
        for (i32 i = VimRegisterCount - 1; i > VimRegisterDelete; i -= 1){
            vim_registers[i] = vim_registers[i - 1];
        }
        vim_registers[VimRegisterDelete] = oldest;
        vim_register_set(&vim_registers[VimRegisterDelete], string, linewise, false);
    }

    clipboard_post(0, string);
    vim_register_linewise = linewise;
    vim_register_size = string.size;
}

function b32
vim_register_get(Application_Links *app, Arena *arena, u8 reg, String_Const_u8 *text_out, b32 *linewise_out){
    b32 result = false;
    Vim_Register *slot = vim_register_slot(reg);
    if (slot != 0){
        if (slot->text.size > 0){
            *text_out = push_string_copy(arena, slot->text);
            *linewise_out = slot->linewise;
            result = true;
        }
    }
    else if (reg != '_'){
        clipboard_update_history_from_system(app, 0);
        if (clipboard_count(0) > 0){
            String_Const_u8 string = push_clipboard_index(arena, 0, 0);
            if (string.size > 0){
                *text_out = string;
                *linewise_out = (vim_register_linewise && string.size == vim_register_size &&
                                 string.str[string.size - 1] == '\n');
                result = true;
            }
        }
    }
    return(result);
}

////////////////////////////////
// NOTE(vim): Marks

function Vim_Buffer_Marks*
vim_get_buffer_marks(Application_Links *app, Buffer_ID buffer){
    Managed_Scope scope = buffer_get_managed_scope(app, buffer);
    Vim_Buffer_Marks *marks = scope_attachment(app, scope, vim_buffer_marks, Vim_Buffer_Marks);
    if (marks != 0 && marks->markers == 0){
        marks->markers = alloc_buffer_markers_on_buffer(app, buffer, VimMarkCount, 0);
    }
    return(marks);
}

function void
vim_set_mark(Application_Links *app, Buffer_ID buffer, i32 index, i64 pos){
    Vim_Buffer_Marks *marks = vim_get_buffer_marks(app, buffer);
    if (marks != 0){
        Marker marker = {};
        marker.pos = pos;
        if (managed_object_store_data(app, marks->markers, index, 1, &marker)){
            marks->set_flags |= (1 << index);
        }
    }
}

function b32
vim_get_mark(Application_Links *app, Buffer_ID buffer, i32 index, i64 *pos_out){
    b32 result = false;
    Vim_Buffer_Marks *marks = vim_get_buffer_marks(app, buffer);
    if (marks != 0 && HasFlag(marks->set_flags, (1 << index))){
        Marker marker = {};
        if (managed_object_load_data(app, marks->markers, index, 1, &marker)){
            *pos_out = marker.pos;
            result = true;
        }
    }
    return(result);
}

function i32
vim_mark_index(u8 c){
    i32 result = -1;
    if ('a' <= c && c <= 'z'){
        result = c - 'a';
    }
    else if (c == '\'' || c == '`'){
        result = VimMarkLastJump;
    }
    return(result);
}

// NOTE(vim): Big jumps remember where they came from for '' and <c-o>.
function void
vim_push_jump(Application_Links *app, View_ID view, Buffer_ID buffer){
    point_stack_push_view_cursor(app, view);
    vim_set_mark(app, buffer, VimMarkLastJump, view_get_cursor_pos(app, view));
}

////////////////////////////////
// NOTE(vim): Mode and undo grouping

function void
vim_set_mode(Application_Links *app, View_ID view, Vim_Mode mode){
    if (vim_mode_is_visual(vim_mode) && !vim_mode_is_visual(mode)){
        vim_last_visual_buffer = view_get_buffer(app, view, Access_Always);
        vim_last_visual_mark = view_get_mark_pos(app, view);
        vim_last_visual_cursor = view_get_cursor_pos(app, view);
        vim_last_visual_mode = vim_mode;
    }
    vim_mode = mode;
}

// NOTE(vim): A change (c, s, o, i ... <esc>) is merged into a single undo step.
function void
vim_begin_change(Application_Links *app, Buffer_ID buffer){
    if (!vim_change_history_active){
        vim_change_history = history_group_begin(app, buffer);
        vim_change_history_active = true;
    }
}

function void
vim_end_change(void){
    if (vim_change_history_active){
        history_group_end(vim_change_history);
        vim_change_history_active = false;
    }
}

function void
vim_enter_insert(Application_Links *app, View_ID view, Buffer_ID buffer){
    vim_begin_change(app, buffer);
    vim_insert_count = 1;
    vim_insert_start = view_get_cursor_pos(app, view);
    vim_insert_buffer = buffer;
    vim_set_mode(app, view, VimMode_Insert);
}

////////////////////////////////
// NOTE(vim): Reading pending keys

function void
vim_capture_char(u8 c){
    if (!vim_replaying){
        if (vim_capture_size < sizeof(vim_capture_space)){
            vim_capture_space[vim_capture_size] = c;
            vim_capture_size += 1;
        }
        else{
            vim_capture_overflow = true;
        }
    }
}

function void
vim_capture_string(String_Const_u8 string){
    for (u64 i = 0; i < string.size; i += 1){
        vim_capture_char(string.str[i]);
    }
}

function b32
vim_queue_pop(u8 *out){
    b32 result = false;
    if (vim_queue_pos < vim_queue_size){
        *out = vim_queue_space[vim_queue_pos];
        vim_queue_pos += 1;
        result = true;
    }
    return(result);
}

function b32
vim_read_char(Application_Links *app, u8 *out){
    if (vim_replaying){
        return(vim_queue_pop(out));
    }
    b32 result = false;
    for (;;){
        User_Input in = get_next_input(app, EventProperty_AnyKey|EventProperty_TextInsert,
                                       EventProperty_Escape|EventProperty_ViewActivation|EventProperty_MouseButton);
        if (in.abort){
            break;
        }
        if (in.event.kind == InputEventKind_TextInsert){
            String_Const_u8 text = to_writable(&in.event);
            if (text.size > 0){
                *out = text.str[0];
                result = true;
                break;
            }
        }
        else if (in.event.kind == InputEventKind_KeyStroke){
            Key_Code code = in.event.key.code;
            if (code == KeyCode_Shift || code == KeyCode_Control || code == KeyCode_Alt ||
                code == KeyCode_Command || code == KeyCode_Menu){
                leave_current_input_unhandled(app);
                continue;
            }
            // NOTE(vim): Control chords read as their ASCII control code, like vim's <c-x>.
            if (has_modifier(&in.event, KeyCode_Control) && KeyCode_A <= code && code <= KeyCode_Z){
                *out = (u8)(1 + (code - KeyCode_A));
                result = true;
                break;
            }
            if (in.event.key.first_dependent_text != 0){
                leave_current_input_unhandled(app);
                continue;
            }
            break;
        }
    }
    if (result){
        vim_capture_char(*out);
    }
    return(result);
}

function b32
vim_read_count(Application_Links *app, u8 *c, i32 *count){
    b32 result = true;
    for (;;){
        u8 d = *c;
        if (('1' <= d && d <= '9') || (d == '0' && *count > 0)){
            *count = clamp_top((*count)*10 + (d - '0'), 100000);
            if (!vim_read_char(app, c)){
                result = false;
                break;
            }
        }
        else{
            break;
        }
    }
    return(result);
}

function b32
vim_query(Application_Links *app, String_Const_u8 prompt, u8 *space, u64 cap, String_Const_u8 init, String_Const_u8 *out){
    Query_Bar_Group group(app);
    Query_Bar bar = {};
    bar.prompt = prompt;
    bar.string = SCu8(space, (u64)0);
    bar.string_capacity = cap;
    b32 result = query_user_general(app, &bar, false, init);
    if (result){
        *out = bar.string;
    }
    return(result);
}

////////////////////////////////
// NOTE(vim): Searching

function void
vim_set_search(String_Const_u8 string, b32 whole_word){
    u64 size = clamp_top(string.size, sizeof(vim_search_space));
    block_copy(vim_search_space, string.str, size);
    vim_search = SCu8(vim_search_space, size);
    vim_search_whole_word = whole_word;
    vim_search_highlight = true;
}

function i64
vim_search_needle(Application_Links *app, Buffer_ID buffer, i64 pos, b32 backward, String_Const_u8 needle, b32 whole_word){
    i64 result = -1;
    if (needle.size > 0){
        i64 size = buffer_get_size(app, buffer);
        i64 first = -1;
        i64 at = pos;
        for (i32 i = 0; i < 100000; i += 1){
            i64 found = -1;
            if (!backward){
                seek_string_forward(app, buffer, at, 0, needle, &found);
                if (found >= size){
                    seek_string_forward(app, buffer, -1, 0, needle, &found);
                }
                if (found >= size){
                    found = -1;
                }
            }
            else{
                seek_string_backward(app, buffer, at, 0, needle, &found);
                if (found < 0){
                    seek_string_backward(app, buffer, size, 0, needle, &found);
                }
            }
            if (found < 0 || found == first){
                break;
            }
            if (first < 0){
                first = found;
            }
            if (!whole_word || vim_is_whole_word(app, buffer, found, needle.size)){
                result = found;
                break;
            }
            at = found;
        }
    }
    return(result);
}

function i64
vim_search_from(Application_Links *app, Buffer_ID buffer, i64 pos, b32 backward){
    return(vim_search_needle(app, buffer, pos, backward, vim_search, vim_search_whole_word));
}

// NOTE(vim): Incremental / and ? prompt: the cursor follows the first match while typing.
function b32
vim_search_prompt(Application_Links *app, View_ID view, Buffer_ID buffer, b32 backward, i64 *pos_out){
    i64 origin = view_get_cursor_pos(app, view);
    b32 result = false;

    if (vim_replaying){
        u8 space[256];
        u64 size = 0;
        u8 c = 0;
        for (;vim_queue_pop(&c) && c != '\n';){
            if (size < sizeof(space)){
                space[size] = c;
                size += 1;
            }
        }
        if (size > 0){
            vim_set_search(SCu8(space, size), false);
        }
    }
    else{
        Query_Bar_Group group(app);
        u8 space[256];
        Query_Bar bar = {};
        bar.prompt = backward?string_u8_litexpr("?"):string_u8_litexpr("/");
        bar.string = SCu8(space, (u64)0);
        bar.string_capacity = sizeof(space);
        if (start_query_bar(app, &bar, 0) == 0){
            return(false);
        }
        Buffer_Scroll origin_scroll = view_get_buffer_scroll(app, view);

        b32 accepted = false;
        for (;;){
            User_Input in = get_next_input(app, EventProperty_AnyKey|EventProperty_TextInsert,
                                           EventProperty_Escape|EventProperty_MouseButton|EventProperty_ViewActivation);
            if (in.abort){
                break;
            }
            b32 changed = false;
            if (in.event.kind == InputEventKind_KeyStroke){
                Key_Code code = in.event.key.code;
                if (code == KeyCode_Return){
                    accepted = true;
                    break;
                }
                else if (code == KeyCode_Backspace){
                    if (bar.string.size == 0){
                        break;
                    }
                    bar.string = backspace_utf8(bar.string);
                    changed = true;
                }
                else{
                    leave_current_input_unhandled(app);
                }
            }
            else if (in.event.kind == InputEventKind_TextInsert){
                String_Const_u8 text = to_writable(&in.event);
                if (text.size > 0 && text.str[0] != '\n' && text.str[0] != '\t'){
                    String_u8 string = Su8(bar.string.str, bar.string.size, bar.string_capacity);
                    string_append(&string, text);
                    bar.string.size = string.string.size;
                    changed = true;
                }
            }
            if (changed){
                i64 match = vim_search_needle(app, buffer, origin, backward, bar.string, false);
                if (match >= 0){
                    view_set_cursor(app, view, seek_pos(match));
                    view_set_highlight_range(app, view, Ii64_size(match, bar.string.size));
                }
                else{
                    view_set_cursor(app, view, seek_pos(origin));
                    view_disable_highlight_range(app, view);
                }
            }
        }
        view_disable_highlight_range(app, view);
        view_set_cursor(app, view, seek_pos(origin));
        if (!accepted){
            view_set_buffer_scroll(app, view, origin_scroll, SetBufferScroll_NoCursorChange);
            return(false);
        }
        if (bar.string.size > 0){
            vim_set_search(bar.string, false);
        }
        vim_capture_string(bar.string);
        vim_capture_char('\n');
    }

    vim_search_backward = backward;
    vim_search_highlight = true;
    *pos_out = vim_search_from(app, buffer, origin, backward);
    result = (*pos_out >= 0);
    return(result);
}

////////////////////////////////
// NOTE(vim): Motions

function i64
vim_next_word_start(Application_Links *app, Buffer_ID buffer, i64 pos, b32 big){
    i64 size = buffer_get_size(app, buffer);
    if (pos < size){
        i32 cls = vim_char_class(buffer_get_char(app, buffer, pos), big);
        if (cls != 0){
            for (; pos < size && vim_char_class(buffer_get_char(app, buffer, pos), big) == cls; pos += 1);
        }
        for (; pos < size; pos += 1){
            u8 c = buffer_get_char(app, buffer, pos);
            if (!vim_is_space(c)){
                break;
            }
            // NOTE(vim): Empty lines count as words.
            if (c == '\n' && pos + 1 < size && buffer_get_char(app, buffer, pos + 1) == '\n'){
                pos += 1;
                break;
            }
        }
    }
    return(clamp_top(pos, size));
}

function i64
vim_prev_word_start(Application_Links *app, Buffer_ID buffer, i64 pos, b32 big){
    if (pos > 0){
        pos -= 1;
        for (; pos > 0 && vim_is_space(buffer_get_char(app, buffer, pos)); pos -= 1);
        i32 cls = vim_char_class(buffer_get_char(app, buffer, pos), big);
        for (; pos > 0 && vim_char_class(buffer_get_char(app, buffer, pos - 1), big) == cls; pos -= 1);
    }
    return(pos);
}

function i64
vim_next_word_end(Application_Links *app, Buffer_ID buffer, i64 pos, b32 big){
    i64 size = buffer_get_size(app, buffer);
    pos += 1;
    for (; pos < size && vim_is_space(buffer_get_char(app, buffer, pos)); pos += 1);
    if (pos >= size){
        pos = clamp_bot(0, size - 1);
    }
    else{
        i32 cls = vim_char_class(buffer_get_char(app, buffer, pos), big);
        for (; pos + 1 < size && vim_char_class(buffer_get_char(app, buffer, pos + 1), big) == cls; pos += 1);
    }
    return(pos);
}

function i64
vim_prev_word_end(Application_Links *app, Buffer_ID buffer, i64 pos, b32 big){
    i32 cls = vim_char_class(buffer_get_char(app, buffer, pos), big);
    if (cls != 0){
        for (; pos > 0 && vim_char_class(buffer_get_char(app, buffer, pos), big) == cls; pos -= 1);
    }
    for (; pos > 0 && vim_is_space(buffer_get_char(app, buffer, pos)); pos -= 1);
    return(pos);
}

function i64
vim_paragraph(Application_Links *app, Buffer_ID buffer, i64 pos, b32 forward, i32 n){
    i64 line_count = vim_line_count(app, buffer);
    i64 line = vim_line_of(app, buffer, pos);
    i64 result = 0;
    if (forward){
        for (i32 i = 0; i < n && line <= line_count; i += 1){
            line += 1;
            for (; line <= line_count && line_is_blank(app, buffer, line); line += 1);
            for (; line <= line_count && !line_is_blank(app, buffer, line); line += 1);
        }
        if (line > line_count){
            result = buffer_get_size(app, buffer);
        }
        else{
            result = get_line_start_pos(app, buffer, line);
        }
    }
    else{
        for (i32 i = 0; i < n && line >= 1; i += 1){
            line -= 1;
            for (; line >= 1 && line_is_blank(app, buffer, line); line -= 1);
            for (; line >= 1 && !line_is_blank(app, buffer, line); line -= 1);
        }
        if (line < 1){
            result = 0;
        }
        else{
            result = get_line_start_pos(app, buffer, line);
        }
    }
    return(result);
}

function b32
vim_bracket_info(u8 c, u8 *open, u8 *close, b32 *forward){
    b32 result = true;
    switch (c){
        case '(': case ')': {*open = '('; *close = ')';}break;
        case '[': case ']': {*open = '['; *close = ']';}break;
        case '{': case '}': {*open = '{'; *close = '}';}break;
        default: {result = false;}break;
    }
    if (result){
        *forward = (c == *open);
    }
    return(result);
}

function i64
vim_match_bracket(Application_Links *app, Buffer_ID buffer, i64 pos){
    i64 result = -1;
    i64 line_end = vim_line_end(app, buffer, vim_line_of(app, buffer, pos));
    u8 open = 0;
    u8 close = 0;
    b32 forward = false;
    i64 p = pos;
    for (; p < line_end; p += 1){
        if (vim_bracket_info(buffer_get_char(app, buffer, p), &open, &close, &forward)){
            break;
        }
    }
    if (p < line_end){
        i64 size = buffer_get_size(app, buffer);
        i64 depth = 0;
        u8 self = forward?open:close;
        u8 other = forward?close:open;
        i64 delta = forward?1:-1;
        for (i64 q = p; 0 <= q && q < size; q += delta){
            u8 c = buffer_get_char(app, buffer, q);
            if (c == self){
                depth += 1;
            }
            else if (c == other){
                depth -= 1;
                if (depth == 0){
                    result = q;
                    break;
                }
            }
        }
    }
    return(result);
}

function i64
vim_find_in_line(Application_Links *app, Buffer_ID buffer, i64 pos, u8 kind, u8 ch, i32 n){
    i64 line = vim_line_of(app, buffer, pos);
    i64 start = get_line_start_pos(app, buffer, line);
    i64 end = vim_line_end(app, buffer, line);
    i64 result = -1;
    b32 forward = (kind == 'f' || kind == 't');
    i64 p = pos;
    // NOTE(vim): t and T would otherwise get stuck on the character next to the target.
    if (kind == 't'){
        p += 1;
    }
    else if (kind == 'T'){
        p -= 1;
    }
    for (i32 i = 0; i < n; i += 1){
        if (forward){
            for (p += 1; p < end && buffer_get_char(app, buffer, p) != ch; p += 1);
            if (p >= end){
                p = -1;
                break;
            }
        }
        else{
            for (p -= 1; p >= start && buffer_get_char(app, buffer, p) != ch; p -= 1);
            if (p < start){
                p = -1;
                break;
            }
        }
    }
    if (p >= 0){
        result = p;
        if (kind == 't'){
            result = p - 1;
        }
        else if (kind == 'T'){
            result = p + 1;
        }
    }
    return(result);
}

// NOTE(vim): which is 0 for H, 1 for M and 2 for L.
function i64
vim_screen_line(Application_Links *app, View_ID view, Buffer_ID buffer, i32 which, i32 count){
    Rect_f32 region = view_get_buffer_region(app, view);
    f32 line_height = get_view_line_height(app, view);
    i64 top = vim_line_of(app, buffer, view_pos_from_xy(app, view, V2f32(region.x0, region.y0 + 1.f)));
    i64 bottom = vim_line_of(app, buffer, view_pos_from_xy(app, view, V2f32(region.x0, region.y1 - line_height*0.5f)));
    i64 result = top;
    switch (which){
        case 0: {result = clamp_top(top + clamp_bot(1, count) - 1, bottom);}break;
        case 1: {result = (top + bottom)/2;}break;
        case 2: {result = clamp_bot(top, bottom - clamp_bot(1, count) + 1);}break;
    }
    return(result);
}

function Vim_Motion
vim_motion(Application_Links *app, View_ID view, Buffer_ID buffer, u8 c, i32 count, b32 for_operator){
    Vim_Motion m = {};
    i32 n = clamp_bot(1, count);
    i64 pos = view_get_cursor_pos(app, view);
    i64 line = vim_line_of(app, buffer, pos);
    i64 line_start = get_line_start_pos(app, buffer, line);
    i64 line_end = vim_line_end(app, buffer, line);
    m.ok = true;
    m.pos = pos;

    switch (c){
        case 'h':
        {
            m.pos = clamp_bot(line_start, pos - n);
        }break;

        case 'l':
        {
            i64 limit = for_operator?line_end:clamp_bot(line_start, line_end - 1);
            m.pos = clamp_top(pos + n, limit);
        }break;

        case 'j': case '+': case '\n':
        {
            m.linewise = true;
            m.pos = vim_first_non_blank(app, buffer, vim_clamp_line(app, buffer, line + n));
        }break;

        case 'k': case '-':
        {
            m.linewise = true;
            m.pos = vim_first_non_blank(app, buffer, vim_clamp_line(app, buffer, line - n));
        }break;

        case 'w': case 'W':
        {
            b32 big = (c == 'W');
            for (i32 i = 0; i < n; i += 1){
                m.pos = vim_next_word_start(app, buffer, m.pos, big);
            }
            // NOTE(vim): dw on the last word of a line does not swallow the line break.
            if (for_operator && vim_line_of(app, buffer, m.pos) > line){
                i64 last_line = vim_line_of(app, buffer, m.pos) - 1;
                i64 last_line_end = vim_line_end(app, buffer, last_line);
                if (last_line_end > pos){
                    m.pos = last_line_end;
                }
            }
        }break;

        case 'b': case 'B':
        {
            for (i32 i = 0; i < n; i += 1){
                m.pos = vim_prev_word_start(app, buffer, m.pos, (c == 'B'));
            }
        }break;

        case 'e': case 'E':
        {
            m.inclusive = true;
            for (i32 i = 0; i < n; i += 1){
                m.pos = vim_next_word_end(app, buffer, m.pos, (c == 'E'));
            }
        }break;

        case '0':
        {
            m.pos = line_start;
        }break;

        case '^':
        {
            m.pos = vim_first_non_blank(app, buffer, line);
        }break;

        case '_':
        {
            m.linewise = true;
            m.pos = vim_first_non_blank(app, buffer, vim_clamp_line(app, buffer, line + n - 1));
        }break;

        case '$':
        {
            i64 target = vim_clamp_line(app, buffer, line + n - 1);
            i64 start = get_line_start_pos(app, buffer, target);
            i64 end = vim_line_end(app, buffer, target);
            if (end > start){
                m.pos = end - 1;
                m.inclusive = true;
            }
            else{
                m.pos = start;
            }
        }break;

        case 'G':
        {
            i64 target = (count > 0)?count:vim_line_count(app, buffer);
            m.linewise = true;
            m.jump = true;
            m.pos = vim_first_non_blank(app, buffer, vim_clamp_line(app, buffer, target));
        }break;

        case 'H': case 'M': case 'L':
        {
            i32 which = (c == 'H')?0:((c == 'M')?1:2);
            m.linewise = true;
            m.jump = true;
            m.pos = vim_first_non_blank(app, buffer, vim_screen_line(app, view, buffer, which, count));
        }break;

        case '}': case '{':
        {
            m.jump = true;
            m.pos = vim_paragraph(app, buffer, pos, (c == '}'), n);
        }break;

        case '%':
        {
            i64 match = vim_match_bracket(app, buffer, pos);
            m.ok = (match >= 0);
            m.pos = match;
            m.inclusive = true;
            m.jump = true;
        }break;

        case 'f': case 'F': case 't': case 'T':
        {
            u8 ch = 0;
            if (vim_read_char(app, &ch)){
                vim_find_kind = c;
                vim_find_char = ch;
                m.pos = vim_find_in_line(app, buffer, pos, c, ch, n);
                m.ok = (m.pos >= 0);
                m.inclusive = (c == 'f' || c == 't');
            }
            else{
                m.ok = false;
            }
        }break;

        case ';': case ',':
        {
            u8 kind = vim_find_kind;
            if (c == ','){
                switch (kind){
                    case 'f': kind = 'F'; break;
                    case 'F': kind = 'f'; break;
                    case 't': kind = 'T'; break;
                    case 'T': kind = 't'; break;
                }
            }
            if (kind != 0){
                m.pos = vim_find_in_line(app, buffer, pos, kind, vim_find_char, n);
                m.ok = (m.pos >= 0);
                m.inclusive = (kind == 'f' || kind == 't');
            }
            else{
                m.ok = false;
            }
        }break;

        case '/': case '?':
        {
            m.jump = true;
            m.ok = vim_search_prompt(app, view, buffer, (c == '?'), &m.pos);
        }break;

        case 'n': case 'N':
        {
            b32 backward = (c == 'n')?vim_search_backward:!vim_search_backward;
            for (i32 i = 0; i < n && m.pos >= 0; i += 1){
                m.pos = vim_search_from(app, buffer, m.pos, backward);
            }
            m.ok = (m.pos >= 0);
            m.jump = true;
            vim_search_highlight = true;
        }break;

        case '*': case '#':
        {
            Scratch_Block scratch(app);
            Range_i64 word = enclose_pos_alpha_numeric_underscore(app, buffer, pos);
            String_Const_u8 string = push_buffer_range(app, scratch, buffer, word);
            m.ok = false;
            if (string.size > 0){
                vim_set_search(string, true);
                vim_search_backward = (c == '#');
                m.pos = vim_search_from(app, buffer, (c == '#')?word.min:pos, vim_search_backward);
                m.ok = (m.pos >= 0);
                m.jump = true;
            }
        }break;

        case '\'': case '`':
        {
            u8 name = 0;
            i32 index = -1;
            m.ok = false;
            if (vim_read_char(app, &name)){
                index = vim_mark_index(name);
            }
            if (index >= 0 && vim_get_mark(app, buffer, index, &m.pos)){
                m.ok = true;
                m.jump = true;
                if (c == '\''){
                    m.linewise = true;
                    m.pos = vim_first_non_blank(app, buffer, vim_line_of(app, buffer, m.pos));
                }
            }
        }break;

        case 'g':
        {
            u8 c2 = 0;
            m.ok = false;
            if (vim_read_char(app, &c2)){
                switch (c2){
                    case 'g':
                    {
                        m.ok = true;
                        m.linewise = true;
                        m.jump = true;
                        m.pos = vim_first_non_blank(app, buffer, vim_clamp_line(app, buffer, (count > 0)?count:1));
                    }break;

                    case 'e': case 'E':
                    {
                        m.ok = true;
                        m.inclusive = true;
                        for (i32 i = 0; i < n; i += 1){
                            m.pos = vim_prev_word_end(app, buffer, m.pos, (c2 == 'E'));
                        }
                    }break;

                    case '_':
                    {
                        i64 target = vim_clamp_line(app, buffer, line + n - 1);
                        i64 start = get_line_start_pos(app, buffer, target);
                        i64 end = vim_line_end(app, buffer, target);
                        for (; end > start && vim_is_space(buffer_get_char(app, buffer, end - 1)); end -= 1);
                        m.ok = true;
                        m.inclusive = (end > start);
                        m.pos = (end > start)?(end - 1):start;
                    }break;
                }
            }
        }break;

        default:
        {
            m.ok = false;
        }break;
    }

    return(m);
}

// NOTE(vim): j/k move by buffer lines, not wrapped screen rows, and remember the column
// they started from so passing through short lines does not lose it.
global Buffer_ID vim_goal_buffer = 0;
global i64 vim_goal_pos = -1;
global i64 vim_goal_col = 0;

function void
vim_move_lines(Application_Links *app, View_ID view, Buffer_ID buffer, i64 delta){
    i64 pos = view_get_cursor_pos(app, view);
    i64 line = vim_line_of(app, buffer, pos);
    if (buffer != vim_goal_buffer || pos != vim_goal_pos){
        vim_goal_col = pos - get_line_start_pos(app, buffer, line);
    }
    i64 target = vim_clamp_line(app, buffer, line + delta);
    i64 start = get_line_start_pos(app, buffer, target);
    i64 end = vim_line_end(app, buffer, target);
    i64 last = (vim_mode == VimMode_Insert)?end:clamp_bot(start, end - 1);
    i64 new_pos = (vim_goal_col >= last - start)?last:(start + vim_goal_col);
    view_set_cursor(app, view, seek_pos(new_pos));
    vim_goal_buffer = buffer;
    vim_goal_pos = new_pos;
}

function void
vim_move(Application_Links *app, View_ID view, Buffer_ID buffer, u8 c, i32 count){
    i32 n = clamp_bot(1, count);
    if (c == 'j' || c == 'k'){
        vim_move_lines(app, view, buffer, (c == 'j')?n:-n);
    }
    else{
        Vim_Motion m = vim_motion(app, view, buffer, c, count, false);
        if (m.ok){
            if (m.jump){
                vim_push_jump(app, view, buffer);
            }
            vim_set_cursor(app, view, m.pos);
            if (c == '$'){
                vim_goal_buffer = buffer;
                vim_goal_pos = m.pos;
                vim_goal_col = max_i64;
            }
        }
    }
}

////////////////////////////////
// NOTE(vim): Text objects

function Range_i64
vim_word_object(Application_Links *app, Buffer_ID buffer, i64 pos, b32 around, b32 big){
    i64 line = vim_line_of(app, buffer, pos);
    i64 start = get_line_start_pos(app, buffer, line);
    i64 end = vim_line_end(app, buffer, line);
    Range_i64 result = Ii64(pos, pos);
    if (pos < end){
        i32 cls = vim_char_class(buffer_get_char(app, buffer, pos), big);
        i64 a = pos;
        i64 b = pos + 1;
        for (; a > start && vim_char_class(buffer_get_char(app, buffer, a - 1), big) == cls; a -= 1);
        for (; b < end && vim_char_class(buffer_get_char(app, buffer, b), big) == cls; b += 1);
        if (around){
            if (cls != 0){
                if (b < end && vim_is_space(buffer_get_char(app, buffer, b))){
                    for (; b < end && vim_is_space(buffer_get_char(app, buffer, b)); b += 1);
                }
                else{
                    for (; a > start && vim_is_space(buffer_get_char(app, buffer, a - 1)); a -= 1);
                }
            }
            else if (b < end){
                i32 next_cls = vim_char_class(buffer_get_char(app, buffer, b), big);
                for (; b < end && vim_char_class(buffer_get_char(app, buffer, b), big) == next_cls; b += 1);
            }
        }
        result = Ii64(a, b);
    }
    return(result);
}

function b32
vim_quote_object(Application_Links *app, Buffer_ID buffer, i64 pos, u8 quote, b32 around, Range_i64 *out){
    i64 line = vim_line_of(app, buffer, pos);
    i64 start = get_line_start_pos(app, buffer, line);
    i64 end = vim_line_end(app, buffer, line);
    b32 result = false;
    i64 open = -1;
    for (i64 p = start; p < end; p += 1){
        u8 c = buffer_get_char(app, buffer, p);
        if (c == '\\'){
            p += 1;
            continue;
        }
        if (c == quote){
            if (open < 0){
                open = p;
            }
            else{
                // NOTE(vim): Take the first pair that contains the cursor or starts after it.
                if (pos <= p){
                    *out = around?Ii64(open, p + 1):Ii64(open + 1, p);
                    result = true;
                    break;
                }
                open = -1;
            }
        }
    }
    return(result);
}

function b32
vim_bracket_object(Application_Links *app, Buffer_ID buffer, i64 pos, u8 open, u8 close, b32 around, Range_i64 *out){
    b32 result = false;
    i64 size = buffer_get_size(app, buffer);
    i64 open_pos = -1;
    i64 depth = 0;
    for (i64 p = pos; p >= 0; p -= 1){
        u8 c = buffer_get_char(app, buffer, p);
        if (c == close && p != pos){
            depth += 1;
        }
        else if (c == open){
            if (depth == 0){
                open_pos = p;
                break;
            }
            depth -= 1;
        }
    }
    if (open_pos >= 0){
        depth = 0;
        for (i64 p = open_pos + 1; p < size; p += 1){
            u8 c = buffer_get_char(app, buffer, p);
            if (c == open){
                depth += 1;
            }
            else if (c == close){
                if (depth == 0){
                    if (around){
                        *out = Ii64(open_pos, p + 1);
                    }
                    else{
                        // NOTE(vim): For blocks spanning lines, keep the brace lines intact.
                        i64 a = open_pos + 1;
                        i64 b = p;
                        if (buffer_get_char(app, buffer, a) == '\n'){
                            a += 1;
                        }
                        i64 q = b;
                        for (; q > a && (buffer_get_char(app, buffer, q - 1) == ' ' ||
                                         buffer_get_char(app, buffer, q - 1) == '\t'); q -= 1);
                        if (q > a && buffer_get_char(app, buffer, q - 1) == '\n'){
                            b = q;
                        }
                        *out = Ii64(a, clamp_bot(a, b));
                    }
                    result = true;
                    break;
                }
                depth -= 1;
            }
        }
    }
    return(result);
}

// NOTE(vim): A paragraph is a run of lines with the same blankness; ap adds the next run.
function Range_i64
vim_paragraph_object(Application_Links *app, Buffer_ID buffer, i64 pos, b32 around){
    i64 line_count = vim_line_count(app, buffer);
    i64 line = vim_line_of(app, buffer, pos);
    b32 blank = line_is_blank(app, buffer, line);
    i64 a = line;
    i64 b = line;
    for (; a > 1 && line_is_blank(app, buffer, a - 1) == blank; a -= 1);
    for (; b < line_count && line_is_blank(app, buffer, b + 1) == blank; b += 1);
    if (around){
        if (b < line_count){
            for (; b < line_count && line_is_blank(app, buffer, b + 1) != blank; b += 1);
        }
        else{
            for (; a > 1 && line_is_blank(app, buffer, a - 1) != blank; a -= 1);
        }
    }
    return(Ii64(a, b));
}

function b32
vim_text_object(Application_Links *app, Buffer_ID buffer, i64 pos, b32 around, u8 c, Range_i64 *out, b32 *linewise){
    b32 result = true;
    *linewise = false;
    switch (c){
        case 'w': {*out = vim_word_object(app, buffer, pos, around, false);}break;
        case 'W': {*out = vim_word_object(app, buffer, pos, around, true);}break;
        case 'p':
        {
            *out = vim_paragraph_object(app, buffer, pos, around);
            *linewise = true;
        }break;
        case '"': case '\'': case '`':
        {
            result = vim_quote_object(app, buffer, pos, c, around, out);
        }break;
        case '(': case ')': case 'b':
        {
            result = vim_bracket_object(app, buffer, pos, '(', ')', around, out);
        }break;
        case '{': case '}': case 'B':
        {
            result = vim_bracket_object(app, buffer, pos, '{', '}', around, out);
        }break;
        case '[': case ']':
        {
            result = vim_bracket_object(app, buffer, pos, '[', ']', around, out);
        }break;
        case '<': case '>':
        {
            result = vim_bracket_object(app, buffer, pos, '<', '>', around, out);
        }break;
        default: {result = false;}break;
    }
    return(result);
}

////////////////////////////////
// NOTE(vim): Operators

function void
vim_shift_lines(Application_Links *app, Buffer_ID buffer, i64 first, i64 last, b32 right){
    Scratch_Block scratch(app);
    i32 indent_width = (i32)def_get_config_u64(app, vars_save_string_lit("indent_width"));
    indent_width = clamp_bot(1, indent_width);
    b32 indent_with_tabs = def_get_config_b32(vars_save_string_lit("indent_with_tabs"));
    String_Const_u8 unit = string_u8_litexpr("\t");
    if (!indent_with_tabs){
        u8 *spaces = push_array(scratch, u8, indent_width);
        block_fill_u8(spaces, indent_width, ' ');
        unit = SCu8(spaces, indent_width);
    }
    History_Group group = history_group_begin(app, buffer);
    for (i64 line = last; line >= first; line -= 1){
        i64 start = get_line_start_pos(app, buffer, line);
        i64 end = vim_line_end(app, buffer, line);
        if (right){
            if (end > start){
                buffer_replace_range(app, buffer, Ii64(start), unit);
            }
        }
        else{
            i64 p = start;
            if (buffer_get_char(app, buffer, p) == '\t'){
                p += 1;
            }
            else{
                for (; p < end && p - start < indent_width && buffer_get_char(app, buffer, p) == ' '; p += 1);
            }
            if (p > start){
                buffer_replace_range(app, buffer, Ii64(start, p), string_u8_empty);
            }
        }
    }
    history_group_end(group);
}

function void
vim_change_case(Application_Links *app, Buffer_ID buffer, Range_i64 range, u8 op){
    Scratch_Block scratch(app);
    String_Const_u8 string = push_buffer_range(app, scratch, buffer, range);
    for (u64 i = 0; i < string.size; i += 1){
        u8 c = string.str[i];
        if (op == 'U' || (op == '~' && character_is_lower(c))){
            string.str[i] = character_to_upper(c);
        }
        else if (op == 'u' || (op == '~' && character_is_upper(c))){
            string.str[i] = character_to_lower(c);
        }
    }
    buffer_replace_range(app, buffer, range, string);
}

function void
vim_join_lines(Application_Links *app, View_ID view, Buffer_ID buffer, i32 count, b32 insert_space){
    i32 joins = clamp_bot(1, count - 1);
    History_Group group = history_group_begin(app, buffer);
    for (i32 i = 0; i < joins; i += 1){
        i64 line = vim_line_of(app, buffer, view_get_cursor_pos(app, view));
        if (line >= vim_line_count(app, buffer)){
            break;
        }
        i64 start = get_line_start_pos(app, buffer, line);
        i64 end = vim_line_end(app, buffer, line);
        i64 next = insert_space?vim_first_non_blank(app, buffer, line + 1):get_line_start_pos(app, buffer, line + 1);
        u8 next_char = buffer_get_char(app, buffer, next);
        u8 last_char = buffer_get_char(app, buffer, end - 1);
        b32 next_is_empty = (next == vim_line_end(app, buffer, line + 1));
        String_Const_u8 sep = string_u8_litexpr(" ");
        if (!insert_space || end == start || next_is_empty || next_char == ')' ||
            last_char == ' ' || last_char == '\t'){
            sep = string_u8_empty;
        }
        buffer_replace_range(app, buffer, Ii64(end, next), sep);
        vim_set_cursor(app, view, end);
    }
    history_group_end(group);
}

// NOTE(vim): For linewise operations range holds the first and last line numbers.
function void
vim_apply_operator(Application_Links *app, View_ID view, Buffer_ID buffer, u8 op, Range_i64 range, b32 linewise){
    Scratch_Block scratch(app);
    i64 size = buffer_get_size(app, buffer);
    if (linewise){
        i64 line_count = vim_line_count(app, buffer);
        i64 l0 = vim_clamp_line(app, buffer, range.min);
        i64 l1 = clamp(l0, range.max, line_count);
        Range_i64 text_range = Ii64(get_line_start_pos(app, buffer, l0), get_line_end_pos(app, buffer, l1));
        if (op == 'd' || op == 'y' || op == 'c'){
            String_Const_u8 text = push_buffer_range(app, scratch, buffer, text_range);
            vim_yank(push_u8_stringf(scratch, "%.*s\n", string_expand(text)), true, (op == 'y'));
        }
        switch (op){
            case 'd':
            {
                Range_i64 kill = {};
                if (l1 < line_count){
                    kill = Ii64(get_line_start_pos(app, buffer, l0), get_line_start_pos(app, buffer, l1 + 1));
                }
                else if (l0 > 1){
                    // NOTE(vim): Deleting the last lines keeps the file's final line break.
                    kill = Ii64(get_line_end_pos(app, buffer, l0 - 1), get_line_end_pos(app, buffer, l1));
                }
                else{
                    kill = Ii64(0, size);
                }
                buffer_replace_range(app, buffer, kill, string_u8_empty);
                vim_set_cursor_line(app, view, buffer, l0);
            }break;

            case 'y':
            {
                i64 cursor_line = vim_line_of(app, buffer, view_get_cursor_pos(app, view));
                if (cursor_line > l0){
                    vim_set_cursor_line(app, view, buffer, l0);
                }
            }break;

            case 'c':
            {
                vim_begin_change(app, buffer);
                i64 a = vim_first_non_blank(app, buffer, l0);
                i64 b = vim_line_end(app, buffer, l1);
                buffer_replace_range(app, buffer, Ii64(a, clamp_bot(a, b)), string_u8_empty);
                vim_set_cursor(app, view, a);
                vim_enter_insert(app, view, buffer);
            }break;

            case '>': case '<':
            {
                vim_shift_lines(app, buffer, l0, l1, (op == '>'));
                vim_set_cursor_line(app, view, buffer, l0);
            }break;

            case '=':
            {
                auto_indent_buffer(app, buffer, text_range);
                vim_set_cursor_line(app, view, buffer, l0);
            }break;

            case 'u': case 'U': case '~':
            {
                vim_change_case(app, buffer, text_range, op);
                vim_set_cursor_line(app, view, buffer, l0);
            }break;

            case 'J':
            {
                vim_set_cursor(app, view, get_line_start_pos(app, buffer, l0));
                vim_join_lines(app, view, buffer, (i32)(l1 - l0 + 1), true);
            }break;
        }
    }
    else{
        range.min = clamp(0, range.min, size);
        range.max = clamp(range.min, range.max, size);
        switch (op){
            case 'd': case 'y': case 'c':
            {
                String_Const_u8 text = push_buffer_range(app, scratch, buffer, range);
                vim_yank(text, false, (op == 'y'));
                if (op == 'c'){
                    vim_begin_change(app, buffer);
                }
                if (op != 'y'){
                    buffer_replace_range(app, buffer, range, string_u8_empty);
                }
                vim_set_cursor(app, view, range.min);
                if (op == 'c'){
                    vim_enter_insert(app, view, buffer);
                }
            }break;

            case '>': case '<': case '=': case 'J':
            {
                i64 l0 = vim_line_of(app, buffer, range.min);
                i64 l1 = vim_line_of(app, buffer, (range.max > range.min)?(range.max - 1):range.max);
                vim_apply_operator(app, view, buffer, op, Ii64(l0, l1), true);
            }break;

            case 'u': case 'U': case '~':
            {
                vim_change_case(app, buffer, range, op);
                vim_set_cursor(app, view, range.min);
            }break;
        }
    }
}

// NOTE(vim): cw on a word changes to the end of the word, even on its last character.
function i64
vim_change_word_end(Application_Links *app, Buffer_ID buffer, i64 pos, i32 n, b32 big){
    i64 end = pos;
    for (i32 i = 0; i < n; i += 1){
        u8 c = buffer_get_char(app, buffer, end);
        u8 next = buffer_get_char(app, buffer, end + 1);
        if (i == 0 && vim_char_class(next, big) != vim_char_class(c, big)){
            continue;
        }
        end = vim_next_word_end(app, buffer, end, big);
    }
    return(end);
}

function void
vim_operator(Application_Links *app, View_ID view, Buffer_ID buffer, u8 op, i32 count, u8 op_char){
    u8 c = 0;
    if (!vim_read_char(app, &c)){
        return;
    }
    i32 motion_count = 0;
    if (!vim_read_count(app, &c, &motion_count)){
        return;
    }
    i32 n = 0;
    if (count > 0 || motion_count > 0){
        n = clamp_bot(1, count)*clamp_bot(1, motion_count);
    }

    i64 pos = view_get_cursor_pos(app, view);
    i64 line = vim_line_of(app, buffer, pos);

    // NOTE(vim): Doubled operators (dd, yy, cc, >>, gUU ...) act on whole lines.
    if (c == op_char){
        vim_apply_operator(app, view, buffer, op, Ii64(line, line + clamp_bot(1, n) - 1), true);
    }
    else if (c == 'i' || c == 'a'){
        u8 object = 0;
        Range_i64 range = {};
        b32 linewise = false;
        if (vim_read_char(app, &object) &&
            vim_text_object(app, buffer, pos, (c == 'a'), object, &range, &linewise)){
            vim_apply_operator(app, view, buffer, op, range, linewise);
        }
    }
    else if (op == 'c' && (c == 'w' || c == 'W') && !vim_is_space(buffer_get_char(app, buffer, pos))){
        i64 end = vim_change_word_end(app, buffer, pos, clamp_bot(1, n), (c == 'W'));
        vim_apply_operator(app, view, buffer, op, Ii64(pos, end + 1), false);
    }
    else{
        Vim_Motion m = vim_motion(app, view, buffer, c, n, true);
        if (m.ok){
            if (m.linewise){
                i64 target = vim_line_of(app, buffer, m.pos);
                vim_apply_operator(app, view, buffer, op, Ii64(line, target), true);
            }
            else{
                Range_i64 range = Ii64(pos, m.pos);
                if (m.inclusive){
                    range.max += 1;
                }
                vim_apply_operator(app, view, buffer, op, range, false);
            }
        }
    }
}

////////////////////////////////
// NOTE(vim): Editing commands

function void
vim_paste_text(Application_Links *app, View_ID view, Buffer_ID buffer, String_Const_u8 string, b32 linewise, b32 before, i32 count){
    if (string.size == 0){
        return;
    }
    Scratch_Block scratch(app);
    i32 n = clamp_bot(1, count);
    u8 *repeated = push_array(scratch, u8, string.size*n + 1);
    u64 repeated_size = 0;
    for (i32 i = 0; i < n; i += 1){
        block_copy(repeated + repeated_size, string.str, string.size);
        repeated_size += string.size;
    }
    String_Const_u8 text = SCu8(repeated, repeated_size);

    i64 pos = view_get_cursor_pos(app, view);
    i64 line = vim_line_of(app, buffer, pos);
    i64 insert_pos = pos;

    if (linewise){
        i64 line_count = buffer_get_line_count(app, buffer);
        i64 target_line = before?line:(line + 1);
        if (text.str[text.size - 1] != '\n'){
            text = push_u8_stringf(scratch, "%.*s\n", string_expand(text));
        }
        if (target_line <= line_count){
            insert_pos = get_line_start_pos(app, buffer, target_line);
        }
        else{
            // NOTE(vim): The last line has no line break to paste after.
            insert_pos = buffer_get_size(app, buffer);
            text = push_u8_stringf(scratch, "\n%.*s", (i32)(text.size - 1), text.str);
        }
        buffer_replace_range(app, buffer, Ii64(insert_pos), text);
        vim_set_cursor_line(app, view, buffer, target_line);
    }
    else{
        if (!before && pos < vim_line_end(app, buffer, line)){
            insert_pos = pos + 1;
        }
        buffer_replace_range(app, buffer, Ii64(insert_pos), text);
        vim_set_cursor(app, view, insert_pos + text.size - 1);
    }

    ARGB_Color argb = fcolor_resolve(fcolor_id(defcolor_paste));
    buffer_post_fade(app, buffer, 0.667f, Ii64_size(insert_pos, text.size), argb);
}

function void
vim_paste(Application_Links *app, View_ID view, Buffer_ID buffer, b32 before, i32 count){
    Scratch_Block scratch(app);
    String_Const_u8 text = {};
    b32 linewise = false;
    if (vim_register_get(app, scratch, vim_pending_register, &text, &linewise)){
        vim_paste_text(app, view, buffer, text, linewise, before, count);
    }
}

function void
vim_open_line(Application_Links *app, View_ID view, Buffer_ID buffer, b32 above){
    Scratch_Block scratch(app);
    vim_begin_change(app, buffer);
    i64 line = vim_line_of(app, buffer, view_get_cursor_pos(app, view));
    i64 start = get_line_start_pos(app, buffer, line);
    String_Const_u8 indent = push_buffer_range(app, scratch, buffer, Ii64(start, vim_first_non_blank(app, buffer, line)));
    if (above){
        String_Const_u8 text = push_u8_stringf(scratch, "%.*s\n", string_expand(indent));
        buffer_replace_range(app, buffer, Ii64(start), text);
        vim_set_cursor(app, view, start + indent.size);
    }
    else{
        // NOTE(vim): Insert at the LF itself so CRLF line endings stay intact.
        i64 end = get_line_end_pos(app, buffer, line);
        String_Const_u8 text = push_u8_stringf(scratch, "\n%.*s", string_expand(indent));
        buffer_replace_range(app, buffer, Ii64(end), text);
        vim_set_cursor(app, view, end + text.size);
    }
    if (vim_buffer_is_code(app, buffer)){
        auto_indent_line_at_cursor(app);
    }
    vim_enter_insert(app, view, buffer);
}

function void
vim_replace_chars(Application_Links *app, View_ID view, Buffer_ID buffer, i32 count){
    u8 c = 0;
    if (!vim_read_char(app, &c)){
        return;
    }
    i32 n = clamp_bot(1, count);
    i64 pos = view_get_cursor_pos(app, view);
    i64 end = vim_line_end(app, buffer, vim_line_of(app, buffer, pos));
    if (pos + n <= end){
        if (c == '\n'){
            buffer_replace_range(app, buffer, Ii64(pos, pos + n), string_u8_litexpr("\n"));
            vim_set_cursor(app, view, pos + 1);
            if (vim_buffer_is_code(app, buffer)){
                auto_indent_line_at_cursor(app);
            }
        }
        else{
            Scratch_Block scratch(app);
            u8 *chars = push_array(scratch, u8, n);
            block_fill_u8(chars, n, c);
            buffer_replace_range(app, buffer, Ii64(pos, pos + n), SCu8(chars, n));
            vim_set_cursor(app, view, pos + n - 1);
        }
    }
}

// NOTE(vim): Replace mode overwrites characters until the end of the line, then appends.
function void
vim_replace_string(Application_Links *app, View_ID view, Buffer_ID buffer, String_Const_u8 text){
    for (u64 i = 0; i < text.size; i += 1){
        i64 pos = view_get_cursor_pos(app, view);
        i64 end = vim_line_end(app, buffer, vim_line_of(app, buffer, pos));
        Range_i64 range = Ii64(pos);
        if (text.str[i] != '\n' && pos < end){
            range = Ii64(pos, pos + 1);
        }
        buffer_replace_range(app, buffer, range, SCu8(text.str + i, 1));
        vim_set_cursor(app, view, pos + 1);
    }
}

function void
vim_increment(Application_Links *app, View_ID view, Buffer_ID buffer, i64 delta){
    Scratch_Block scratch(app);
    i64 pos = view_get_cursor_pos(app, view);
    i64 line = vim_line_of(app, buffer, pos);
    i64 start = get_line_start_pos(app, buffer, line);
    String_Const_u8 text = push_buffer_range(app, scratch, buffer, Ii64(start, vim_line_end(app, buffer, line)));
    u64 i = (u64)(pos - start);
    if (i < text.size && character_is_base10(text.str[i])){
        for (; i > 0 && character_is_base10(text.str[i - 1]); i -= 1);
    }
    else{
        for (; i < text.size && !character_is_base10(text.str[i]); i += 1);
    }
    if (i >= text.size){
        return;
    }
    u64 j = i;
    for (; j < text.size && character_is_base10(text.str[j]); j += 1);
    b32 negative = (i > 0 && text.str[i - 1] == '-');
    u64 number_start = negative?(i - 1):i;
    i64 value = (i64)string_to_integer(SCu8(text.str + i, j - i), 10);
    if (negative){
        value = -value;
    }
    value += delta;
    String_Const_u8 replacement = push_u8_stringf(scratch, "%lld", value);
    buffer_replace_range(app, buffer, Ii64(start + number_start, start + j), replacement);
    vim_set_cursor(app, view, start + number_start + replacement.size - 1);
}

function void
vim_scroll_lines(Application_Links *app, View_ID view, Buffer_ID buffer, i64 delta){
    Buffer_Scroll scroll = view_get_buffer_scroll(app, view);
    scroll.target.line_number = vim_clamp_line(app, buffer, scroll.target.line_number + delta);
    scroll.target.pixel_shift.y = 0.f;
    view_set_buffer_scroll(app, view, scroll, SetBufferScroll_SnapCursorIntoView);
}

// NOTE(vim): zt puts the cursor line at the top of the view, zb at the bottom.
function void
vim_scroll_cursor_to(Application_Links *app, View_ID view, Buffer_ID buffer, b32 top){
    Buffer_Scroll scroll = view_get_buffer_scroll(app, view);
    i64 line = vim_line_of(app, buffer, view_get_cursor_pos(app, view));
    if (!top){
        Rect_f32 region = view_get_buffer_region(app, view);
        f32 line_height = get_view_line_height(app, view);
        i64 rows = clamp_bot(1, (i64)(rect_height(region)/line_height));
        line = clamp_bot(1, line - rows + 1);
    }
    scroll.target.line_number = line;
    scroll.target.pixel_shift.y = 0.f;
    view_set_buffer_scroll(app, view, scroll, SetBufferScroll_NoCursorChange);
}

function void
vim_quit(Application_Links *app, View_ID view, b32 force){
    if (!view_close(app, view)){
        if (force){
            hard_exit(app);
        }
        else{
            exit_4coder(app);
        }
    }
}

function void
vim_cycle_buffer(Application_Links *app, View_ID view, Buffer_ID buffer, b32 forward){
    Buffer_ID next = buffer;
    for (;;){
        if (forward){
            next = get_buffer_next(app, next, Access_Always);
            if (next == 0){
                next = get_buffer_next(app, 0, Access_Always);
            }
        }
        else{
            Buffer_ID prev = 0;
            for (Buffer_ID it = get_buffer_next(app, 0, Access_Always); it != 0; it = get_buffer_next(app, it, Access_Always)){
                if (it == next && prev != 0){
                    break;
                }
                prev = it;
            }
            next = prev;
        }
        if (next == 0 || next == buffer || !buffer_has_name_with_star(app, next)){
            break;
        }
    }
    if (next != 0 && next != buffer){
        view_set_buffer(app, view, next, 0);
    }
}

////////////////////////////////
// NOTE(vim): Ex commands

// NOTE(vim): Parses a line address (N . $ 'x '< '>) with optional +N/-N offsets.
function b32
vim_parse_ex_line(Application_Links *app, View_ID view, Buffer_ID buffer, String_Const_u8 *cmd, i64 *line_out){
    b32 result = false;
    i64 line = vim_line_of(app, buffer, view_get_cursor_pos(app, view));
    if (cmd->size > 0){
        u8 c = cmd->str[0];
        if (character_is_base10(c)){
            u64 i = 0;
            for (; i < cmd->size && character_is_base10(cmd->str[i]); i += 1);
            line = (i64)string_to_integer(string_prefix(*cmd, i), 10);
            *cmd = string_skip(*cmd, i);
            result = true;
        }
        else if (c == '.'){
            *cmd = string_skip(*cmd, 1);
            result = true;
        }
        else if (c == '$'){
            line = vim_line_count(app, buffer);
            *cmd = string_skip(*cmd, 1);
            result = true;
        }
        else if (c == '\'' && cmd->size >= 2){
            u8 name = cmd->str[1];
            i64 pos = 0;
            if ((name == '<' || name == '>') && vim_last_visual_buffer == buffer){
                Range_i64 range = Ii64(vim_last_visual_mark, vim_last_visual_cursor);
                line = vim_line_of(app, buffer, (name == '<')?range.min:range.max);
                result = true;
            }
            else if (vim_mark_index(name) >= 0 && vim_get_mark(app, buffer, vim_mark_index(name), &pos)){
                line = vim_line_of(app, buffer, pos);
                result = true;
            }
            *cmd = string_skip(*cmd, 2);
        }
        else if (c == '+' || c == '-'){
            result = true;
        }
    }
    for (;result && cmd->size > 0 && (cmd->str[0] == '+' || cmd->str[0] == '-');){
        i64 sign = (cmd->str[0] == '+')?1:-1;
        *cmd = string_skip(*cmd, 1);
        u64 i = 0;
        for (; i < cmd->size && character_is_base10(cmd->str[i]); i += 1);
        i64 amount = (i > 0)?(i64)string_to_integer(string_prefix(*cmd, i), 10):1;
        *cmd = string_skip(*cmd, i);
        line += sign*amount;
    }
    *line_out = line;
    return(result);
}

// NOTE(vim): Splits off the next delimiter terminated part of a :s command; \<delim> escapes.
function String_Const_u8
vim_ex_take_part(Arena *arena, String_Const_u8 *rest, u8 delim){
    u8 *out = push_array(arena, u8, rest->size + 1);
    u64 size = 0;
    u64 i = 0;
    for (; i < rest->size && rest->str[i] != delim; i += 1){
        u8 c = rest->str[i];
        if (c == '\\' && i + 1 < rest->size){
            u8 next = rest->str[i + 1];
            if (next == delim || next == '\\'){
                c = next;
                i += 1;
            }
            else if (next == 'n'){
                c = '\n';
                i += 1;
            }
            else if (next == 't'){
                c = '\t';
                i += 1;
            }
        }
        out[size] = c;
        size += 1;
    }
    *rest = string_skip(*rest, i + 1);
    return(SCu8(out, size));
}

function b32
vim_match_at(String_Const_u8 text, u64 pos, String_Const_u8 needle, b32 ignore_case){
    b32 result = (pos + needle.size <= text.size);
    for (u64 i = 0; result && i < needle.size; i += 1){
        u8 a = text.str[pos + i];
        u8 b = needle.str[i];
        if (ignore_case){
            a = character_to_lower(a);
            b = character_to_lower(b);
        }
        result = (a == b);
    }
    return(result);
}

// NOTE(vim): :s/needle/replacement/[gi] with literal (not regex) matching. Without g only
// the first match on each line is replaced, like vim.
function b32
vim_substitute(Application_Links *app, View_ID view, Buffer_ID buffer, Range_i64 lines, String_Const_u8 cmd){
    if (cmd.size < 2 || cmd.str[0] != 's'){
        return(false);
    }
    u8 delim = cmd.str[1];
    if (character_is_alpha_numeric(delim) || vim_is_space(delim) || delim == '\\' || delim == '"'){
        return(false);
    }

    Scratch_Block scratch(app);
    String_Const_u8 rest = string_skip(cmd, 2);
    String_Const_u8 needle = vim_ex_take_part(scratch, &rest, delim);
    String_Const_u8 replacement = vim_ex_take_part(scratch, &rest, delim);
    b32 global_flag = (string_find_first(rest, 'g') < rest.size);
    b32 ignore_case = (string_find_first(rest, 'i') < rest.size);
    if (needle.size == 0){
        needle = push_string_copy(scratch, vim_search);
    }
    if (needle.size == 0){
        return(true);
    }
    vim_set_search(needle, false);

    i32 total = 0;
    i64 first_line = 0;
    History_Group group = history_group_begin(app, buffer);
    for (i64 line = lines.max; line >= lines.min; line -= 1){
        Temp_Memory_Block temp(scratch);
        Range_i64 range = Ii64(get_line_start_pos(app, buffer, line), vim_line_end(app, buffer, line));
        String_Const_u8 text = push_buffer_range(app, scratch, buffer, range);
        List_String_Const_u8 list = {};
        u64 copied = 0;
        i32 count = 0;
        for (u64 i = 0; i + needle.size <= text.size;){
            if (vim_match_at(text, i, needle, ignore_case)){
                string_list_push(scratch, &list, SCu8(text.str + copied, i - copied));
                string_list_push(scratch, &list, replacement);
                i += needle.size;
                copied = i;
                count += 1;
                if (!global_flag){
                    break;
                }
            }
            else{
                i += 1;
            }
        }
        if (count > 0){
            string_list_push(scratch, &list, SCu8(text.str + copied, text.size - copied));
            buffer_replace_range(app, buffer, range, string_list_flatten(scratch, list));
            total += count;
            first_line = line;
        }
    }
    history_group_end(group);

    if (total > 0){
        vim_set_cursor_line(app, view, buffer, first_line);
    }
    else{
        print_message(app, push_u8_stringf(scratch, "vim: pattern not found: %.*s\n", string_expand(needle)));
    }
    return(true);
}

function void
vim_ex_set(Application_Links *app, View_ID view, Buffer_ID buffer, String_Const_u8 arg){
    Scratch_Block scratch(app);
#define VimSet(S) string_match(arg, string_u8_litexpr(S))
    if (VimSet("nu") || VimSet("number") || VimSet("nonu") || VimSet("nonumber")){
        def_set_config_b32(vars_save_string_lit("show_line_number_margins"), (arg.str[0] != 'n' || arg.str[1] == 'u'));
    }
    else if (VimSet("wrap") || VimSet("nowrap")){
        Managed_Scope scope = buffer_get_managed_scope(app, buffer);
        b32 *wrap_lines_ptr = scope_attachment(app, scope, buffer_wrap_lines, b32);
        if (wrap_lines_ptr != 0){
            *wrap_lines_ptr = (arg.str[0] == 'w');
            buffer_clear_layout_cache(app, buffer);
        }
    }
    else if (VimSet("hls") || VimSet("hlsearch")){
        vim_hlsearch = true;
        vim_search_highlight = true;
    }
    else if (VimSet("nohls") || VimSet("nohlsearch")){
        vim_hlsearch = false;
    }
    else{
        print_message(app, push_u8_stringf(scratch, "vim: unknown option: %.*s\n", string_expand(arg)));
    }
#undef VimSet
}

function void
vim_execute_ex(Application_Links *app, View_ID view, Buffer_ID buffer, String_Const_u8 cmd){
    cmd = string_skip_chop_whitespace(cmd);
    if (cmd.size == 0){
        return;
    }
    {
        u64 size = clamp_top(cmd.size, sizeof(vim_last_ex_space));
        block_copy(vim_last_ex_space, cmd.str, size);
        vim_last_ex = SCu8(vim_last_ex_space, size);
        cmd = vim_last_ex;
    }

    i64 line_count = vim_line_count(app, buffer);
    i64 cursor_line = vim_line_of(app, buffer, view_get_cursor_pos(app, view));
    Range_i64 lines = Ii64(cursor_line, cursor_line);
    b32 has_range = false;
    if (cmd.str[0] == '%'){
        lines = Ii64(1, line_count);
        cmd = string_skip(cmd, 1);
        has_range = true;
    }
    else{
        i64 first = 0;
        if (vim_parse_ex_line(app, view, buffer, &cmd, &first)){
            has_range = true;
            lines = Ii64(first, first);
            if (cmd.size > 0 && (cmd.str[0] == ',' || cmd.str[0] == ';')){
                cmd = string_skip(cmd, 1);
                i64 last = 0;
                if (vim_parse_ex_line(app, view, buffer, &cmd, &last)){
                    lines = Ii64(first, last);
                }
            }
        }
    }
    lines.min = vim_clamp_line(app, buffer, lines.min);
    lines.max = vim_clamp_line(app, buffer, lines.max);
    cmd = string_skip_chop_whitespace(cmd);

    if (cmd.size == 0){
        if (has_range){
            vim_push_jump(app, view, buffer);
            vim_set_cursor_line(app, view, buffer, lines.max);
        }
        return;
    }
    if (vim_substitute(app, view, buffer, lines, cmd)){
        return;
    }

    u64 split = string_find_first(cmd, ' ');
    String_Const_u8 name = string_prefix(cmd, split);
    String_Const_u8 arg = string_skip_chop_whitespace(string_skip(cmd, split));

#define VimEx(S) string_match(name, string_u8_litexpr(S))
    if (VimEx("w") || VimEx("write")){
        save(app);
    }
    else if (VimEx("wa") || VimEx("wall")){
        save_all_dirty_buffers(app);
    }
    else if (VimEx("q") || VimEx("quit") || VimEx("close")){
        vim_quit(app, view, false);
    }
    else if (VimEx("q!") || VimEx("quit!")){
        vim_quit(app, view, true);
    }
    else if (VimEx("wq") || VimEx("x") || VimEx("xit")){
        save(app);
        vim_quit(app, view, false);
    }
    else if (VimEx("qa") || VimEx("qall") || VimEx("wqa") || VimEx("xa")){
        if (name.str[0] != 'q'){
            save_all_dirty_buffers(app);
        }
        exit_4coder(app);
    }
    else if (VimEx("qa!") || VimEx("qall!")){
        hard_exit(app);
    }
    else if (VimEx("e!") || VimEx("edit!")){
        reopen(app);
    }
    else if (VimEx("e") || VimEx("edit")){
        if (arg.size == 0){
            interactive_open_or_new(app);
        }
        else{
            Scratch_Block scratch(app);
            String_Const_u8 path = arg;
            if (arg.str[0] != '/'){
                String_Const_u8 hot = push_hot_directory(app, scratch);
                path = push_u8_stringf(scratch, "%.*s/%.*s", string_expand(hot), string_expand(arg));
            }
            view_open_file(app, view, path, false);
        }
    }
    else if (VimEx("sp") || VimEx("split") || VimEx("new")){
        open_panel_hsplit(app);
    }
    else if (VimEx("vs") || VimEx("vsplit") || VimEx("vnew")){
        open_panel_vsplit(app);
    }
    else if (VimEx("b") || VimEx("buffer") || VimEx("ls") || VimEx("buffers")){
        interactive_switch_buffer(app);
    }
    else if (VimEx("bn") || VimEx("bnext")){
        vim_cycle_buffer(app, view, buffer, true);
    }
    else if (VimEx("bp") || VimEx("bprevious") || VimEx("bN") || VimEx("bNext")){
        vim_cycle_buffer(app, view, buffer, false);
    }
    else if (VimEx("bd") || VimEx("bdelete")){
        kill_buffer(app);
    }
    else if (VimEx("Files") || VimEx("files") || VimEx("fzf")){
        fuzzy_find_file(app);
    }
    else if (VimEx("Buffers") || VimEx("buffers!")){
        fuzzy_switch_buffer(app);
    }
    else if (VimEx("Rg") || VimEx("rg") || VimEx("grep")){
        if (arg.size == 0){
            fuzzy_grep(app);
        }
        else{
            Scratch_Block scratch(app);
            fuzzy_grep_in(app, fuzzy_search_root(app, scratch), arg);
        }
    }
    else if (VimEx("d") || VimEx("delete")){
        vim_apply_operator(app, view, buffer, 'd', lines, true);
    }
    else if (VimEx("y") || VimEx("yank")){
        vim_apply_operator(app, view, buffer, 'y', lines, true);
    }
    else if (VimEx(">") || VimEx("<")){
        vim_apply_operator(app, view, buffer, name.str[0], lines, true);
    }
    else if (VimEx("j") || VimEx("join")){
        vim_apply_operator(app, view, buffer, 'J', Ii64(lines.min, clamp_bot(lines.min + 1, lines.max)), true);
    }
    else if (VimEx("set") || VimEx("se")){
        vim_ex_set(app, view, buffer, arg);
    }
    else if (VimEx("make") || VimEx("mak")){
        build_in_build_panel(app);
    }
    else if (VimEx("cn") || VimEx("cnext")){
        goto_next_jump(app);
    }
    else if (VimEx("cp") || VimEx("cprev") || VimEx("cprevious")){
        goto_prev_jump(app);
    }
    else if (VimEx("noh") || VimEx("nohlsearch")){
        vim_search_highlight = false;
    }
    else{
        Scratch_Block scratch(app);
        print_message(app, push_u8_stringf(scratch, "vim: not an editor command: %.*s\n", string_expand(cmd)));
    }
#undef VimEx
}

function void
vim_ex_command(Application_Links *app, View_ID view, Buffer_ID buffer, String_Const_u8 init){
    u8 space[512];
    String_Const_u8 cmd = {};
    if (vim_query(app, string_u8_litexpr(":"), space, sizeof(space), init, &cmd)){
        vim_execute_ex(app, view, buffer, cmd);
    }
}

function void
vim_leader(Application_Links *app){
    u8 c = 0;
    if (!vim_read_char(app, &c)){
        return;
    }
    switch (c){
        case 'f': fuzzy_find_file(app); break;
        case 'e': interactive_open_or_new(app); break;
        case 'b': fuzzy_switch_buffer(app); break;
        case '/': case 'g': fuzzy_grep(app); break;
        case '*':
        {
            // NOTE(vim): Also make it the search pattern, so n and N work in the opened file.
            View_ID view = get_active_view(app, Access_ReadVisible);
            Buffer_ID buffer = view_get_buffer(app, view, Access_ReadVisible);
            Scratch_Block scratch(app);
            Range_i64 word = enclose_pos_alpha_numeric_underscore(app, buffer, view_get_cursor_pos(app, view));
            String_Const_u8 string = push_buffer_range(app, scratch, buffer, word);
            if (string.size > 0){
                vim_set_search(string, true);
                vim_search_backward = false;
            }
            fuzzy_grep_identifier(app);
        }break;
        case 'k': interactive_kill_buffer(app); break;
        case 'x': command_lister(app); break;
        case 'w': save(app); break;
        case 'm': build_in_build_panel(app); break;
        case 'n': goto_next_jump(app); break;
        case 'p': goto_prev_jump(app); break;
        case 'd': jump_to_definition_at_cursor(app); break;
        case 'r': list_all_locations_of_identifier(app); break;
        case 's': list_all_locations(app); break;
        case 'l': list_all_functions_current_buffer_lister(app); break;
        case 'o': change_active_panel(app); break;
    }
}

////////////////////////////////
// NOTE(vim): Macros

function void
vim_macro_play(Application_Links *app, Vim_Register *macro, i32 count){
    Scratch_Block scratch(app);
    List_String_Const_u8 lines = string_split(scratch, macro->text, (u8*)"\n", 1);
    for (i32 i = 0; i < clamp_bot(1, count); i += 1){
        for (Node_String_Const_u8 *node = lines.first;
             node != 0;
             node = node->next){
            String_Const_u8 line = string_skip_chop_whitespace(node->string);
            Input_Event event = parse_keyboard_event(scratch, line);
            if (event.kind == InputEventKind_TextInsert || event.kind == InputEventKind_KeyStroke){
                enqueue_virtual_event(app, &event);
            }
        }
    }
}

// NOTE(vim): Recording copies the stretch of the *keyboard* log between q{reg} and q.
function void
vim_macro_toggle_recording(Application_Links *app, i64 log_line_start){
    Buffer_ID log = get_keyboard_log_buffer(app);
    if (vim_macro_recording != 0){
        Scratch_Block scratch(app);
        String_Const_u8 text = push_buffer_range(app, scratch, log, Ii64(vim_macro_start, log_line_start));
        vim_register_set(&vim_macros[vim_macro_recording - 'a'], text, false, false);
        vim_macro_recording = 0;
        global_keyboard_macro_is_recording = false;
    }
    else{
        u8 reg = 0;
        if (vim_read_char(app, &reg)){
            reg = character_to_lower(reg);
            if ('a' <= reg && reg <= 'z'){
                vim_macro_recording = reg;
                vim_macro_start = buffer_get_size(app, log);
                global_keyboard_macro_is_recording = true;
            }
        }
    }
}

function i64
vim_log_last_line_start(Application_Links *app){
    Buffer_ID log = get_keyboard_log_buffer(app);
    i64 size = buffer_get_size(app, log);
    i64 result = size;
    if (size > 0){
        result = get_line_start_pos(app, log, get_line_number_from_pos(app, log, size - 1));
    }
    return(result);
}

////////////////////////////////
// NOTE(vim): Mode dispatch

function void vim_repeat_change(Application_Links *app, View_ID view, Buffer_ID buffer, i32 count);
function void vim_leave_insert(Application_Links *app, View_ID view, Buffer_ID buffer);

function void
vim_normal(Application_Links *app, View_ID view, Buffer_ID buffer, u8 c, i32 count){
    i32 n = clamp_bot(1, count);
    i64 pos = view_get_cursor_pos(app, view);
    i64 line = vim_line_of(app, buffer, pos);

    switch (c){
        case 'i': case 'a': case 'I': case 'A':
        {
            if (c == 'a' && pos < vim_line_end(app, buffer, line)){
                vim_set_cursor(app, view, pos + 1);
            }
            else if (c == 'I'){
                vim_set_cursor(app, view, vim_first_non_blank(app, buffer, line));
            }
            else if (c == 'A'){
                vim_set_cursor(app, view, vim_line_end(app, buffer, line));
            }
            vim_enter_insert(app, view, buffer);
            vim_insert_count = n;
        }break;

        case 'o': case 'O':
        {
            vim_open_line(app, view, buffer, (c == 'O'));
        }break;

        case 'R':
        {
            vim_begin_change(app, buffer);
            vim_insert_count = 1;
            vim_insert_start = pos;
            vim_insert_buffer = buffer;
            vim_set_mode(app, view, VimMode_Replace);
        }break;

        case 'v':
        {
            view_set_mark(app, view, seek_pos(pos));
            vim_set_mode(app, view, VimMode_Visual);
        }break;

        case 'V':
        {
            view_set_mark(app, view, seek_pos(pos));
            vim_set_mode(app, view, VimMode_VisualLine);
        }break;

        case 'x': case 's':
        {
            i64 end = vim_line_end(app, buffer, line);
            Range_i64 range = Ii64(pos, clamp_top(pos + n, end));
            if (c == 's'){
                vim_apply_operator(app, view, buffer, 'c', range, false);
            }
            else if (range.max > range.min){
                vim_apply_operator(app, view, buffer, 'd', range, false);
            }
        }break;

        case 'X':
        {
            Range_i64 range = Ii64(clamp_bot(get_line_start_pos(app, buffer, line), pos - n), pos);
            if (range.max > range.min){
                vim_apply_operator(app, view, buffer, 'd', range, false);
            }
        }break;

        // NOTE(vim): Y yanks to the end of the line, as in Neovim.
        case 'D': case 'C': case 'Y':
        {
            i64 target = vim_clamp_line(app, buffer, line + n - 1);
            Range_i64 range = Ii64(pos, vim_line_end(app, buffer, target));
            u8 op = (c == 'D')?'d':((c == 'C')?'c':'y');
            vim_apply_operator(app, view, buffer, op, range, false);
        }break;

        case 'S':
        {
            vim_apply_operator(app, view, buffer, 'c', Ii64(line, line + n - 1), true);
        }break;

        case 'd': case 'c': case 'y': case '>': case '<': case '=':
        {
            vim_operator(app, view, buffer, c, count, c);
        }break;

        case 'p': case 'P':
        {
            vim_paste(app, view, buffer, (c == 'P'), count);
        }break;

        case 'u':
        {
            for (i32 i = 0; i < n; i += 1){
                undo(app);
            }
        }break;

        case VimCtrl('r'):
        {
            for (i32 i = 0; i < n; i += 1){
                redo(app);
            }
        }break;

        case '.':
        {
            vim_repeat_change(app, view, buffer, count);
        }break;

        case 'r':
        {
            vim_replace_chars(app, view, buffer, count);
        }break;

        case 'J':
        {
            vim_join_lines(app, view, buffer, n, true);
        }break;

        case '~':
        {
            i64 end = vim_line_end(app, buffer, line);
            Range_i64 range = Ii64(pos, clamp_top(pos + n, end));
            if (range.max > range.min){
                vim_change_case(app, buffer, range, '~');
                vim_set_cursor(app, view, range.max);
            }
        }break;

        case VimCtrl('a'): case VimCtrl('x'):
        {
            vim_increment(app, view, buffer, (c == VimCtrl('a'))?n:-n);
        }break;

        case VimCtrl('e'): case VimCtrl('y'):
        {
            vim_scroll_lines(app, view, buffer, (c == VimCtrl('e'))?n:-n);
        }break;

        case 'm':
        {
            u8 name = 0;
            if (vim_read_char(app, &name) && 'a' <= name && name <= 'z'){
                vim_set_mark(app, buffer, name - 'a', pos);
            }
        }break;

        case 'q':
        {
            vim_macro_toggle_recording(app, vim_log_last_line_start(app));
        }break;

        case '@':
        {
            u8 reg = 0;
            if (vim_read_char(app, &reg)){
                if (reg == ':'){
                    if (vim_last_ex.size > 0){
                        Scratch_Block scratch(app);
                        String_Const_u8 cmd = push_string_copy(scratch, vim_last_ex);
                        for (i32 i = 0; i < n; i += 1){
                            vim_execute_ex(app, view, buffer, cmd);
                        }
                    }
                }
                else{
                    if (reg == '@'){
                        reg = vim_macro_last;
                    }
                    reg = character_to_lower(reg);
                    // NOTE(vim): Macros cannot run while recording or from inside another macro.
                    if ('a' <= reg && reg <= 'z' && vim_macro_recording == 0 &&
                        !get_current_input_is_virtual(app)){
                        vim_macro_last = reg;
                        vim_macro_play(app, &vim_macros[reg - 'a'], n);
                    }
                }
            }
        }break;

        case 'g':
        {
            u8 c2 = 0;
            if (vim_read_char(app, &c2)){
                switch (c2){
                    case 'g':
                    {
                        vim_push_jump(app, view, buffer);
                        vim_set_cursor_line(app, view, buffer, (count > 0)?count:1);
                    }break;
                    case 'e': case 'E':
                    {
                        for (i32 i = 0; i < n; i += 1){
                            vim_set_cursor(app, view, vim_prev_word_end(app, buffer, view_get_cursor_pos(app, view), (c2 == 'E')));
                        }
                    }break;
                    case '_':
                    {
                        i64 end = vim_line_end(app, buffer, line);
                        for (; end > get_line_start_pos(app, buffer, line) && vim_is_space(buffer_get_char(app, buffer, end - 1)); end -= 1);
                        vim_set_cursor(app, view, clamp_bot(get_line_start_pos(app, buffer, line), end - 1));
                    }break;
                    case 'd': jump_to_definition_at_cursor(app); break;
                    case 'r': list_all_locations_of_identifier(app); break;
                    case 'c':
                    {
                        u8 c3 = 0;
                        if (vim_read_char(app, &c3) && c3 == 'c'){
                            for (i32 i = 0; i < n; i += 1){
                                vim_set_cursor_line(app, view, buffer, line + i);
                                comment_line_toggle(app);
                            }
                            vim_set_cursor_line(app, view, buffer, line);
                        }
                    }break;
                    case 'u': vim_operator(app, view, buffer, 'u', count, 'u'); break;
                    case 'U': vim_operator(app, view, buffer, 'U', count, 'U'); break;
                    case '~': vim_operator(app, view, buffer, '~', count, '~'); break;
                    case 'J': vim_join_lines(app, view, buffer, n, false); break;
                    case 'j': move_vertical_lines(app, view, n); break;
                    case 'k': move_vertical_lines(app, view, -n); break;
                    case 'v':
                    {
                        if (vim_last_visual_buffer == buffer){
                            view_set_mark(app, view, seek_pos(vim_last_visual_mark));
                            vim_set_cursor(app, view, vim_last_visual_cursor);
                            vim_set_mode(app, view, vim_last_visual_mode);
                        }
                    }break;
                    case 'i':
                    {
                        if (vim_last_insert_buffer == buffer){
                            vim_set_cursor(app, view, vim_last_insert_pos);
                        }
                        vim_enter_insert(app, view, buffer);
                    }break;
                }
            }
        }break;

        case 'z':
        {
            u8 c2 = 0;
            if (vim_read_char(app, &c2)){
                switch (c2){
                    case 'z': case '.': center_view(app); break;
                    case 't': case '\n': vim_scroll_cursor_to(app, view, buffer, true); break;
                    case 'b': case '-': vim_scroll_cursor_to(app, view, buffer, false); break;
                }
            }
        }break;

        case 'Z':
        {
            u8 c2 = 0;
            if (vim_read_char(app, &c2)){
                if (c2 == 'Z'){
                    save(app);
                    vim_quit(app, view, false);
                }
                else if (c2 == 'Q'){
                    vim_quit(app, view, true);
                }
            }
        }break;

        // NOTE(vim): A count before : prefills the range of that many lines, as in 3: -> :.,.+2
        case ':':
        {
            if (count > 1){
                Scratch_Block scratch(app);
                vim_ex_command(app, view, buffer, push_u8_stringf(scratch, ".,.+%d", count - 1));
            }
            else{
                vim_ex_command(app, view, buffer, string_u8_empty);
            }
        }break;

        case ' ':
        {
            vim_leader(app);
        }break;

        default:
        {
            vim_move(app, view, buffer, c, count);
        }break;
    }
}

function Range_i64
vim_visual_char_range(Application_Links *app, View_ID view, Buffer_ID buffer){
    i64 size = buffer_get_size(app, buffer);
    Range_i64 range = Ii64(view_get_cursor_pos(app, view), view_get_mark_pos(app, view));
    range.max = clamp_top(range.max + 1, size);
    return(range);
}

function Range_i64
vim_visual_line_range(Application_Links *app, View_ID view, Buffer_ID buffer){
    Range_i64 range = Ii64(view_get_cursor_pos(app, view), view_get_mark_pos(app, view));
    return(Ii64(vim_line_of(app, buffer, range.min), vim_line_of(app, buffer, range.max)));
}

function void
vim_visual(Application_Links *app, View_ID view, Buffer_ID buffer, u8 c, i32 count){
    b32 linewise = (vim_mode == VimMode_VisualLine);
    i64 pos = view_get_cursor_pos(app, view);

    u8 op = 0;
    switch (c){
        case 'd': case 'x': {op = 'd';}break;
        case 'y': {op = 'y';}break;
        case 'c': case 's': {op = 'c';}break;
        case '>': case '<': case '=': case '~': case 'u': case 'U': case 'J': {op = c;}break;
        case 'D': case 'X': {op = 'd'; linewise = true;}break;
        case 'Y': {op = 'y'; linewise = true;}break;
        case 'C': case 'S': case 'R': {op = 'c'; linewise = true;}break;
    }

    if (op != 0){
        Range_i64 range = linewise?vim_visual_line_range(app, view, buffer):vim_visual_char_range(app, view, buffer);
        vim_set_mode(app, view, VimMode_Normal);
        vim_apply_operator(app, view, buffer, op, range, linewise);
        return;
    }

    switch (c){
        case 'v': case 'V':
        {
            Vim_Mode target = (c == 'v')?VimMode_Visual:VimMode_VisualLine;
            vim_set_mode(app, view, (vim_mode == target)?VimMode_Normal:target);
        }break;

        case 'o':
        {
            i64 mark = view_get_mark_pos(app, view);
            view_set_mark(app, view, seek_pos(pos));
            vim_set_cursor(app, view, mark);
        }break;

        case 'r':
        {
            u8 ch = 0;
            if (vim_read_char(app, &ch) && ch != '\n'){
                Scratch_Block scratch(app);
                Range_i64 range = vim_visual_char_range(app, view, buffer);
                if (linewise){
                    Range_i64 lines = vim_visual_line_range(app, view, buffer);
                    range = Ii64(get_line_start_pos(app, buffer, lines.min), vim_line_end(app, buffer, lines.max));
                }
                String_Const_u8 text = push_buffer_range(app, scratch, buffer, range);
                for (u64 i = 0; i < text.size; i += 1){
                    if (text.str[i] != '\n' && text.str[i] != '\r'){
                        text.str[i] = ch;
                    }
                }
                vim_set_mode(app, view, VimMode_Normal);
                buffer_replace_range(app, buffer, range, text);
                vim_set_cursor(app, view, range.min);
            }
        }break;

        case 'p': case 'P':
        {
            Scratch_Block scratch(app);
            String_Const_u8 text = {};
            b32 text_linewise = false;
            if (!vim_register_get(app, scratch, vim_pending_register, &text, &text_linewise)){
                break;
            }
            Range_i64 range = linewise?vim_visual_line_range(app, view, buffer):vim_visual_char_range(app, view, buffer);
            vim_set_mode(app, view, VimMode_Normal);
            History_Group group = history_group_begin(app, buffer);
            // NOTE(vim): p puts the replaced text in the unnamed register, P keeps it.
            u8 pending = vim_pending_register;
            vim_pending_register = (c == 'p')?0:'_';
            if (linewise){
                vim_apply_operator(app, view, buffer, 'd', range, true);
                b32 deleted_last_line = (range.min > vim_line_count(app, buffer));
                vim_paste_text(app, view, buffer, text, true, !deleted_last_line, 1);
            }
            else{
                vim_apply_operator(app, view, buffer, 'd', range, false);
                vim_paste_text(app, view, buffer, text, text_linewise, !text_linewise, 1);
            }
            vim_pending_register = pending;
            history_group_end(group);
        }break;

        case 'i': case 'a':
        {
            u8 object = 0;
            Range_i64 range = {};
            b32 object_linewise = false;
            if (vim_read_char(app, &object) &&
                vim_text_object(app, buffer, pos, (c == 'a'), object, &range, &object_linewise)){
                if (object_linewise){
                    view_set_mark(app, view, seek_pos(get_line_start_pos(app, buffer, range.min)));
                    vim_set_cursor(app, view, get_line_start_pos(app, buffer, range.max));
                    vim_set_mode(app, view, VimMode_VisualLine);
                }
                else if (range.max > range.min){
                    view_set_mark(app, view, seek_pos(range.min));
                    vim_set_cursor(app, view, range.max - 1);
                }
            }
        }break;

        case ':':
        {
            vim_set_mode(app, view, VimMode_Normal);
            vim_ex_command(app, view, buffer, string_u8_litexpr("'<,'>"));
        }break;

        default:
        {
            vim_move(app, view, buffer, c, count);
        }break;
    }
}

// NOTE(vim): Runs one normal or visual mode command starting with c, reading counts and
// a register prefix first. Returns the command character or 0 if it was cancelled.
function u8
vim_execute(Application_Links *app, View_ID view, Buffer_ID buffer, u8 c){
    i32 count = 0;
    for (;;){
        if (!vim_read_count(app, &c, &count)){
            return(0);
        }
        if (c != '"'){
            break;
        }
        u8 reg = 0;
        if (!vim_read_char(app, &reg) || !vim_register_is_valid(reg) || !vim_read_char(app, &c)){
            return(0);
        }
        vim_pending_register = reg;
    }
    if (vim_mode == VimMode_Normal){
        vim_normal(app, view, buffer, c, count);
    }
    else if (vim_mode_is_visual(vim_mode)){
        vim_visual(app, view, buffer, c, count);
    }
    return(c);
}

function void
vim_finish_command(Application_Links *app){
    vim_pending_register = 0;
    View_ID view = get_active_view(app, Access_ReadVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadVisible);
    if (buffer != 0 && vim_mode == VimMode_Normal){
        vim_clamp_cursor(app, view, buffer);
    }
}

// NOTE(vim): Replays the keys of the last change and, if it entered insert mode, the
// text that was typed, then leaves insert mode again.
function void
vim_repeat_change(Application_Links *app, View_ID view, Buffer_ID buffer, i32 count){
    if (vim_dot_size == 0 || vim_replaying){
        return;
    }
    u64 skip = 0;
    vim_queue_size = 0;
    vim_queue_pos = 0;
    if (count > 0){
        for (; skip < vim_dot_size && character_is_base10(vim_dot_space[skip]) &&
             !(skip == 0 && vim_dot_space[0] == '0'); skip += 1);
        Scratch_Block scratch(app);
        String_Const_u8 digits = push_u8_stringf(scratch, "%d", count);
        block_copy(vim_queue_space, digits.str, digits.size);
        vim_queue_size = digits.size;
    }
    u64 rest = clamp_top(vim_dot_size - skip, sizeof(vim_queue_space) - vim_queue_size);
    block_copy(vim_queue_space + vim_queue_size, vim_dot_space + skip, rest);
    vim_queue_size += rest;

    u8 c = 0;
    vim_replaying = true;
    if (vim_queue_pop(&c)){
        vim_execute(app, view, buffer, c);
        if (vim_mode == VimMode_Insert){
            buffer_replace_range(app, buffer, Ii64(view_get_cursor_pos(app, view)), vim_dot_insert.text);
            vim_set_cursor(app, view, view_get_cursor_pos(app, view) + vim_dot_insert.text.size);
            vim_leave_insert(app, view, buffer);
        }
        else if (vim_mode == VimMode_Replace){
            vim_replace_string(app, view, buffer, vim_dot_insert.text);
            vim_leave_insert(app, view, buffer);
        }
    }
    vim_replaying = false;
    vim_queue_size = 0;
    vim_queue_pos = 0;
}

function void
vim_leave_insert(Application_Links *app, View_ID view, Buffer_ID buffer){
    if (buffer != 0){
        Scratch_Block scratch(app);
        i64 pos = view_get_cursor_pos(app, view);

        String_Const_u8 inserted = {};
        if (buffer == vim_insert_buffer && pos >= vim_insert_start){
            inserted = push_buffer_range(app, scratch, buffer, Ii64(vim_insert_start, pos));
        }

        // NOTE(vim): Counts repeat the inserted text, as in 3ia<esc>.
        if (vim_mode == VimMode_Insert && vim_insert_count > 1 && inserted.size > 0){
            for (i64 i = 1; i < vim_insert_count; i += 1){
                buffer_replace_range(app, buffer, Ii64(pos), inserted);
                pos += inserted.size;
            }
            vim_set_cursor(app, view, pos);
        }

        // NOTE(vim): Like autoindent, leaving a line that only holds indentation clears it.
        i64 line = vim_line_of(app, buffer, pos);
        i64 start = get_line_start_pos(app, buffer, line);
        i64 end = vim_line_end(app, buffer, line);
        if (end > start && pos == end && line_is_blank(app, buffer, line)){
            buffer_replace_range(app, buffer, Ii64(start, end), string_u8_empty);
            pos = start;
            vim_set_cursor(app, view, pos);
        }

        vim_last_insert_buffer = buffer;
        vim_last_insert_pos = pos;
        if (pos > start){
            vim_set_cursor(app, view, pos - 1);
        }

        if (vim_dot_pending){
            block_copy(vim_dot_space, vim_dot_pending_space, vim_dot_pending_size);
            vim_dot_size = vim_dot_pending_size;
            vim_register_set(&vim_dot_insert, inserted, false, false);
            vim_dot_pending = false;
        }
    }
    vim_dot_pending = false;
    vim_end_change();
    vim_insert_count = 1;
    vim_set_mode(app, view, VimMode_Normal);
    if (buffer != 0){
        vim_clamp_cursor(app, view, buffer);
    }
}

CUSTOM_COMMAND_SIG(vim_handle_text)
CUSTOM_DOC("Interprets typed text as a vim normal or visual mode command.")
{
    View_ID view = get_active_view(app, Access_ReadVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadVisible);
    if (buffer == 0){
        return;
    }
    User_Input in = get_current_input(app);
    String_Const_u8 text = to_writable(&in.event);
    if (text.size == 0){
        return;
    }
    u8 c = text.str[0];

    i64 log_line_start = vim_log_last_line_start(app);
    Vim_Mode mode_before = vim_mode;
    History_Record_Index history_before = buffer_history_get_current_state_index(app, buffer);
    vim_capture_size = 0;
    vim_capture_overflow = false;
    vim_capture_char(c);

    u8 command = 0;
    if (c == 'q' && vim_mode == VimMode_Normal && vim_macro_recording != 0){
        vim_macro_toggle_recording(app, log_line_start);
    }
    else{
        command = vim_execute(app, view, buffer, c);
    }

    // NOTE(vim): Remember the change for the . command.
    b32 repeatable = (mode_before == VimMode_Normal && command != 0 && !vim_capture_overflow &&
                      command != 'u' && command != '.' && command != ':' && command != '@' &&
                      command != 'q' && command != VimCtrl('r'));
    if (repeatable){
        if (vim_mode == VimMode_Insert || vim_mode == VimMode_Replace){
            block_copy(vim_dot_pending_space, vim_capture_space, vim_capture_size);
            vim_dot_pending_size = vim_capture_size;
            vim_dot_pending = true;
        }
        else if (view_get_buffer(app, view, Access_ReadVisible) == buffer &&
                 buffer_history_get_current_state_index(app, buffer) != history_before){
            block_copy(vim_dot_space, vim_capture_space, vim_capture_size);
            vim_dot_size = vim_capture_size;
            vim_register_set(&vim_dot_insert, string_u8_empty, false, false);
        }
    }

    vim_finish_command(app);
}

////////////////////////////////
// NOTE(vim): Key bound commands

CUSTOM_COMMAND_SIG(vim_normal_mode)
CUSTOM_DOC("Leaves insert, replace or visual mode and returns to vim normal mode.")
{
    View_ID view = get_active_view(app, Access_ReadVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadVisible);
    if (vim_mode == VimMode_Insert || vim_mode == VimMode_Replace){
        vim_leave_insert(app, view, buffer);
    }
    else{
        vim_set_mode(app, view, VimMode_Normal);
        if (buffer != 0){
            vim_clamp_cursor(app, view, buffer);
        }
    }
}

CUSTOM_COMMAND_SIG(vim_replace_text)
CUSTOM_DOC("Overwrites the character under the cursor with typed text in vim replace mode.")
{
    View_ID view = get_active_view(app, Access_ReadWriteVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadWriteVisible);
    if (buffer != 0){
        User_Input in = get_current_input(app);
        vim_replace_string(app, view, buffer, to_writable(&in.event));
    }
}

CUSTOM_COMMAND_SIG(vim_return)
CUSTOM_DOC("In read only buffers follows the jump under the cursor, otherwise moves to the next line.")
{
    View_ID view = get_active_view(app, Access_ReadVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadWriteVisible);
    if (buffer == 0){
        if_read_only_goto_position(app);
    }
    else{
        i64 line = vim_line_of(app, buffer, view_get_cursor_pos(app, view));
        vim_set_cursor_line(app, view, buffer, line + 1);
    }
}

function void
vim_key_motion(Application_Links *app, u8 c){
    View_ID view = get_active_view(app, Access_ReadVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadVisible);
    if (buffer != 0){
        vim_move(app, view, buffer, c, 0);
        if (vim_mode == VimMode_Normal){
            vim_clamp_cursor(app, view, buffer);
        }
    }
}

CUSTOM_COMMAND_SIG(vim_left)
CUSTOM_DOC("Vim h motion.")
{
    vim_key_motion(app, 'h');
}

CUSTOM_COMMAND_SIG(vim_right)
CUSTOM_DOC("Vim l motion.")
{
    vim_key_motion(app, 'l');
}

CUSTOM_COMMAND_SIG(vim_up)
CUSTOM_DOC("Vim k motion.")
{
    vim_key_motion(app, 'k');
}

CUSTOM_COMMAND_SIG(vim_down)
CUSTOM_DOC("Vim j motion.")
{
    vim_key_motion(app, 'j');
}

CUSTOM_COMMAND_SIG(vim_line_start)
CUSTOM_DOC("Vim 0 motion.")
{
    vim_key_motion(app, '0');
}

CUSTOM_COMMAND_SIG(vim_line_end_motion)
CUSTOM_DOC("Vim $ motion.")
{
    vim_key_motion(app, '$');
}

// NOTE(vim): Control chords bound as keys run the same code as when they follow a count.
function void
vim_key_command(Application_Links *app, u8 c){
    View_ID view = get_active_view(app, Access_ReadVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadVisible);
    if (buffer != 0){
        vim_normal(app, view, buffer, c, 0);
        vim_finish_command(app);
    }
}

CUSTOM_COMMAND_SIG(vim_delete_char)
CUSTOM_DOC("Vim x in normal mode.")
{
    vim_key_command(app, 'x');
}

CUSTOM_COMMAND_SIG(vim_redo)
CUSTOM_DOC("Vim <c-r>, redo.")
{
    vim_key_command(app, VimCtrl('r'));
}

CUSTOM_COMMAND_SIG(vim_increment_number)
CUSTOM_DOC("Vim <c-a>, adds one to the number at or after the cursor.")
{
    vim_key_command(app, VimCtrl('a'));
}

CUSTOM_COMMAND_SIG(vim_decrement_number)
CUSTOM_DOC("Vim <c-x>, subtracts one from the number at or after the cursor.")
{
    vim_key_command(app, VimCtrl('x'));
}

CUSTOM_COMMAND_SIG(vim_scroll_down_line)
CUSTOM_DOC("Vim <c-e>, scrolls the view down one line.")
{
    vim_key_command(app, VimCtrl('e'));
}

CUSTOM_COMMAND_SIG(vim_scroll_up_line)
CUSTOM_DOC("Vim <c-y>, scrolls the view up one line.")
{
    vim_key_command(app, VimCtrl('y'));
}

function void
vim_half_page(Application_Links *app, b32 down){
    View_ID view = get_active_view(app, Access_ReadVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadVisible);
    if (buffer != 0){
        Rect_f32 region = view_get_buffer_region(app, view);
        f32 line_height = get_view_line_height(app, view);
        i64 lines = clamp_bot(1, (i64)(rect_height(region)/line_height/2.f));
        vim_move_lines(app, view, buffer, down?lines:-lines);
        if (vim_mode == VimMode_Normal){
            vim_clamp_cursor(app, view, buffer);
        }
    }
}

CUSTOM_COMMAND_SIG(vim_half_page_down)
CUSTOM_DOC("Vim <c-d>, moves the cursor down half a screen.")
{
    vim_half_page(app, true);
}

CUSTOM_COMMAND_SIG(vim_half_page_up)
CUSTOM_DOC("Vim <c-u>, moves the cursor up half a screen.")
{
    vim_half_page(app, false);
}

CUSTOM_COMMAND_SIG(vim_window_command)
CUSTOM_DOC("Vim <c-w> prefix for panel commands.")
{
    u8 c = 0;
    if (!vim_read_char(app, &c)){
        return;
    }
    switch (c){
        case 'w': case 'l': case 'j': case VimCtrl('w'): change_active_panel(app); break;
        case 'W': case 'h': case 'k': case 'p': change_active_panel_backwards(app); break;
        case 's': case 'S': case VimCtrl('s'): open_panel_hsplit(app); break;
        case 'v': case VimCtrl('v'): open_panel_vsplit(app); break;
        case 'q': case 'c': close_panel(app); break;
        case 'x': swap_panels(app); break;
    }
}

CUSTOM_COMMAND_SIG(vim_visual_block_unsupported)
CUSTOM_DOC("Placeholder for vim <c-v>, visual block mode is not implemented.")
{
    print_message(app, string_u8_litexpr("vim: visual block mode is not supported yet\n"));
}

CUSTOM_COMMAND_SIG(vim_insert_delete_word)
CUSTOM_DOC("Vim insert mode <c-w>, deletes the word before the cursor.")
{
    View_ID view = get_active_view(app, Access_ReadWriteVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadWriteVisible);
    if (buffer != 0){
        i64 pos = view_get_cursor_pos(app, view);
        i64 start = get_line_start_pos(app, buffer, vim_line_of(app, buffer, pos));
        i64 p = pos;
        if (p == start){
            p = clamp_bot(0, p - 1);
        }
        else{
            for (; p > start && vim_is_space(buffer_get_char(app, buffer, p - 1)); p -= 1);
            if (p > start){
                i32 cls = vim_char_class(buffer_get_char(app, buffer, p - 1), false);
                for (; p > start && vim_char_class(buffer_get_char(app, buffer, p - 1), false) == cls; p -= 1);
            }
        }
        buffer_replace_range(app, buffer, Ii64(p, pos), string_u8_empty);
        vim_set_cursor(app, view, p);
    }
}

CUSTOM_COMMAND_SIG(vim_insert_delete_line)
CUSTOM_DOC("Vim insert mode <c-u>, deletes back to the indentation or the start of the line.")
{
    View_ID view = get_active_view(app, Access_ReadWriteVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadWriteVisible);
    if (buffer != 0){
        i64 pos = view_get_cursor_pos(app, view);
        i64 line = vim_line_of(app, buffer, pos);
        i64 p = vim_first_non_blank(app, buffer, line);
        if (p >= pos){
            p = get_line_start_pos(app, buffer, line);
        }
        buffer_replace_range(app, buffer, Ii64(p, pos), string_u8_empty);
        vim_set_cursor(app, view, p);
    }
}

function void
vim_insert_shift(Application_Links *app, b32 right){
    View_ID view = get_active_view(app, Access_ReadWriteVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadWriteVisible);
    if (buffer != 0){
        i64 pos = view_get_cursor_pos(app, view);
        i64 line = vim_line_of(app, buffer, pos);
        i64 from_end = vim_line_end(app, buffer, line) - pos;
        vim_shift_lines(app, buffer, line, line, right);
        i64 start = get_line_start_pos(app, buffer, line);
        vim_set_cursor(app, view, clamp_bot(start, vim_line_end(app, buffer, line) - from_end));
    }
}

CUSTOM_COMMAND_SIG(vim_insert_indent)
CUSTOM_DOC("Vim insert mode <c-t>, indents the current line.")
{
    vim_insert_shift(app, true);
}

CUSTOM_COMMAND_SIG(vim_insert_dedent)
CUSTOM_DOC("Vim insert mode <c-d>, removes one level of indentation from the current line.")
{
    vim_insert_shift(app, false);
}

CUSTOM_COMMAND_SIG(vim_insert_register)
CUSTOM_DOC("Vim insert mode <c-r>{register}, inserts the contents of a register.")
{
    View_ID view = get_active_view(app, Access_ReadWriteVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadWriteVisible);
    u8 reg = 0;
    if (buffer != 0 && vim_read_char(app, &reg) && vim_register_is_valid(reg)){
        Scratch_Block scratch(app);
        String_Const_u8 text = {};
        b32 linewise = false;
        if (vim_register_get(app, scratch, reg, &text, &linewise)){
            i64 pos = view_get_cursor_pos(app, view);
            buffer_replace_range(app, buffer, Ii64(pos), text);
            vim_set_cursor(app, view, pos + text.size);
        }
    }
}

////////////////////////////////
// NOTE(vim): Rendering

function String_Const_u8
vim_mode_label(void){
    String_Const_u8 result = string_u8_litexpr(" NORMAL ");
    switch (vim_mode){
        case VimMode_Insert: {result = string_u8_litexpr(" INSERT ");}break;
        case VimMode_Replace: {result = string_u8_litexpr(" REPLACE ");}break;
        case VimMode_Visual: {result = string_u8_litexpr(" VISUAL ");}break;
        case VimMode_VisualLine: {result = string_u8_litexpr(" V-LINE ");}break;
    }
    return(result);
}

function void
vim_draw_mode_label(Application_Links *app, Face_ID face_id, Rect_f32 bar){
    Scratch_Block scratch(app);
    String_Const_u8 label = vim_mode_label();
    if (vim_macro_recording != 0){
        label = push_u8_stringf(scratch, " REC @%c %.*s", vim_macro_recording, string_expand(label));
    }
    f32 width = get_string_advance(app, face_id, label);
    Rect_f32 box = Rf32(bar.x1 - width, bar.y0, bar.x1, bar.y1);

    FColor back = fcolor_id(defcolor_keyword);
    switch (vim_mode){
        case VimMode_Insert: case VimMode_Replace: {back = fcolor_id(defcolor_str_constant);}break;
        case VimMode_Visual: case VimMode_VisualLine: {back = fcolor_id(defcolor_pop2);}break;
    }
    draw_rectangle_fcolor(app, box, 0.f, back);
    draw_string(app, face_id, label, V2f32(box.x0, box.y0 + 2.f), fcolor_resolve(fcolor_id(defcolor_back)));
}

function void
vim_draw_search_highlight(Application_Links *app, Buffer_ID buffer, Text_Layout_ID text_layout_id, f32 roundness){
    if (vim_hlsearch && vim_search_highlight && vim_search.size > 0){
        Scratch_Block scratch(app);
        Range_i64 visible_range = text_layout_get_visible_range(app, text_layout_id);
        String_Const_u8 text = push_buffer_range(app, scratch, buffer, visible_range);
        for (u64 i = 0; i + vim_search.size <= text.size; i += 1){
            if (vim_match_at(text, i, vim_search, false)){
                i64 pos = visible_range.min + i;
                if (!vim_search_whole_word || vim_is_whole_word(app, buffer, pos, vim_search.size)){
                    draw_character_block(app, text_layout_id, Ii64_size(pos, vim_search.size), roundness,
                                         fcolor_id(defcolor_highlight_white));
                }
                i += vim_search.size - 1;
            }
        }
    }
}

// NOTE(vim): Block cursor in normal mode, bar in insert mode, outline in replace mode
// and in inactive views. Visual selections are drawn from the mark every frame.
function void
vim_draw_cursor(Application_Links *app, View_ID view_id, b32 is_active_view,
                Buffer_ID buffer, Text_Layout_ID text_layout_id,
                f32 roundness, f32 outline_thickness){
    b32 has_highlight_range = draw_highlight_range(app, view_id, buffer, text_layout_id, roundness);
    if (!has_highlight_range){
        i32 cursor_sub_id = default_cursor_sub_id();
        i64 cursor_pos = view_get_cursor_pos(app, view_id);
        if (is_active_view){
            if (vim_mode_is_visual(vim_mode)){
                i64 size = buffer_get_size(app, buffer);
                Range_i64 range = Ii64(cursor_pos, view_get_mark_pos(app, view_id));
                if (vim_mode == VimMode_VisualLine){
                    range = Ii64(get_line_start_pos(app, buffer, vim_line_of(app, buffer, range.min)),
                                 get_line_end_pos(app, buffer, vim_line_of(app, buffer, range.max)) + 1);
                }
                else{
                    range.max += 1;
                }
                range.max = clamp_top(range.max, size);
                draw_character_block(app, text_layout_id, range, roundness, fcolor_id(defcolor_highlight));
                paint_text_color_fcolor(app, text_layout_id, range, fcolor_id(defcolor_at_highlight));
            }
            switch (vim_mode){
                case VimMode_Insert:
                {
                    draw_character_i_bar(app, text_layout_id, cursor_pos, fcolor_id(defcolor_cursor, cursor_sub_id));
                }break;
                case VimMode_Replace:
                {
                    draw_character_wire_frame(app, text_layout_id, cursor_pos, roundness, outline_thickness,
                                              fcolor_id(defcolor_cursor, cursor_sub_id));
                }break;
                default:
                {
                    draw_character_block(app, text_layout_id, cursor_pos, roundness,
                                         fcolor_id(defcolor_cursor, cursor_sub_id));
                    paint_text_color_pos(app, text_layout_id, cursor_pos, fcolor_id(defcolor_at_cursor));
                }break;
            }
        }
        else{
            draw_character_wire_frame(app, text_layout_id, cursor_pos, roundness, outline_thickness,
                                      fcolor_id(defcolor_cursor, cursor_sub_id));
        }
    }
}

// NOTE(vim): A copy of default_render_buffer with the vim cursor and search highlight.
function void
vim_render_buffer(Application_Links *app, View_ID view_id, Face_ID face_id,
                  Buffer_ID buffer, Text_Layout_ID text_layout_id,
                  Rect_f32 rect){
    ProfileScope(app, "render buffer");

    View_ID active_view = get_active_view(app, Access_Always);
    b32 is_active_view = (active_view == view_id);
    Rect_f32 prev_clip = draw_set_clip(app, rect);

    Range_i64 visible_range = text_layout_get_visible_range(app, text_layout_id);

    // NOTE(allen): Cursor shape
    Face_Metrics metrics = get_face_metrics(app, face_id);
    u64 cursor_roundness_100 = def_get_config_u64(app, vars_save_string_lit("cursor_roundness"));
    f32 cursor_roundness = metrics.normal_advance*cursor_roundness_100*0.01f;
    f32 mark_thickness = (f32)def_get_config_u64(app, vars_save_string_lit("mark_thickness"));

    // NOTE(allen): Token colorizing
    Token_Array token_array = get_token_array_from_buffer(app, buffer);
    if (token_array.tokens != 0){
        draw_cpp_token_colors(app, text_layout_id, &token_array);

        // NOTE(allen): Scan for TODOs and NOTEs
        b32 use_comment_keyword = def_get_config_b32(vars_save_string_lit("use_comment_keyword"));
        if (use_comment_keyword){
            Comment_Highlight_Pair pairs[] = {
                {string_u8_litexpr("NOTE"), finalize_color(defcolor_comment_pop, 0)},
                {string_u8_litexpr("TODO"), finalize_color(defcolor_comment_pop, 1)},
            };
            draw_comment_highlights(app, buffer, text_layout_id, &token_array, pairs, ArrayCount(pairs));
        }
    }
    else{
        paint_text_color_fcolor(app, text_layout_id, visible_range, fcolor_id(defcolor_text_default));
    }

    i64 cursor_pos = view_correct_cursor(app, view_id);
    view_correct_mark(app, view_id);

    // NOTE(allen): Scope highlight
    b32 use_scope_highlight = def_get_config_b32(vars_save_string_lit("use_scope_highlight"));
    if (use_scope_highlight){
        Color_Array colors = finalize_color_array(defcolor_back_cycle);
        draw_scope_highlight(app, buffer, text_layout_id, cursor_pos, colors.vals, colors.count);
    }

    b32 use_error_highlight = def_get_config_b32(vars_save_string_lit("use_error_highlight"));
    b32 use_jump_highlight = def_get_config_b32(vars_save_string_lit("use_jump_highlight"));
    if (use_error_highlight || use_jump_highlight){
        // NOTE(allen): Error highlight
        String_Const_u8 name = string_u8_litexpr("*compilation*");
        Buffer_ID compilation_buffer = get_buffer_by_name(app, name, Access_Always);
        if (use_error_highlight){
            draw_jump_highlights(app, buffer, text_layout_id, compilation_buffer,
                                 fcolor_id(defcolor_highlight_junk));
        }

        // NOTE(allen): Search highlight
        if (use_jump_highlight){
            Buffer_ID jump_buffer = get_locked_jump_buffer(app);
            if (jump_buffer != compilation_buffer){
                draw_jump_highlights(app, buffer, text_layout_id, jump_buffer,
                                     fcolor_id(defcolor_highlight_white));
            }
        }
    }

    // NOTE(allen): Color parens
    b32 use_paren_helper = def_get_config_b32(vars_save_string_lit("use_paren_helper"));
    if (use_paren_helper){
        Color_Array colors = finalize_color_array(defcolor_text_cycle);
        draw_paren_highlight(app, buffer, text_layout_id, cursor_pos, colors.vals, colors.count);
    }

    // NOTE(allen): Line highlight
    b32 highlight_line_at_cursor = def_get_config_b32(vars_save_string_lit("highlight_line_at_cursor"));
    if (highlight_line_at_cursor && is_active_view){
        i64 line_number = get_line_number_from_pos(app, buffer, cursor_pos);
        draw_line_highlight(app, text_layout_id, line_number, fcolor_id(defcolor_highlight_cursor_line));
    }

    // NOTE(allen): Whitespace highlight
    b64 show_whitespace = false;
    view_get_setting(app, view_id, ViewSetting_ShowWhitespace, &show_whitespace);
    if (show_whitespace){
        if (token_array.tokens == 0){
            draw_whitespace_highlight(app, buffer, text_layout_id, cursor_roundness);
        }
        else{
            draw_whitespace_highlight(app, text_layout_id, &token_array, cursor_roundness);
        }
    }

    // NOTE(vim): Search matches and cursor
    vim_draw_search_highlight(app, buffer, text_layout_id, cursor_roundness);
    vim_draw_cursor(app, view_id, is_active_view, buffer, text_layout_id, cursor_roundness, mark_thickness);

    // NOTE(allen): Fade ranges
    paint_fade_ranges(app, text_layout_id, buffer);

    // NOTE(allen): put the actual text on the actual screen
    draw_text_layout_default(app, text_layout_id);

    draw_set_clip(app, prev_clip);
}

// NOTE(vim): A copy of default_render_caller that uses vim_render_buffer and shows the mode.
function void
vim_render_caller(Application_Links *app, Frame_Info frame_info, View_ID view_id){
    ProfileScope(app, "vim render caller");
    View_ID active_view = get_active_view(app, Access_Always);
    b32 is_active_view = (active_view == view_id);

    Rect_f32 region = draw_background_and_margin(app, view_id, is_active_view);
    Rect_f32 prev_clip = draw_set_clip(app, region);

    Buffer_ID buffer = view_get_buffer(app, view_id, Access_Always);
    Face_ID face_id = get_face_id(app, buffer);
    Face_Metrics face_metrics = get_face_metrics(app, face_id);
    f32 line_height = face_metrics.line_height;
    f32 digit_advance = face_metrics.decimal_digit_advance;

    // NOTE(allen): file bar
    b64 showing_file_bar = false;
    if (view_get_setting(app, view_id, ViewSetting_ShowFileBar, &showing_file_bar) && showing_file_bar){
        Rect_f32_Pair pair = layout_file_bar_on_top(region, line_height);
        draw_file_bar(app, view_id, buffer, face_id, pair.min);
        if (is_active_view){
            vim_draw_mode_label(app, face_id, pair.min);
        }
        region = pair.max;
    }

    Buffer_Scroll scroll = view_get_buffer_scroll(app, view_id);

    Buffer_Point_Delta_Result delta = delta_apply(app, view_id,
                                                  frame_info.animation_dt, scroll);
    if (!block_match_struct(&scroll.position, &delta.point)){
        block_copy_struct(&scroll.position, &delta.point);
        view_set_buffer_scroll(app, view_id, scroll, SetBufferScroll_NoCursorChange);
    }
    if (delta.still_animating){
        animate_in_n_milliseconds(app, 0);
    }

    // NOTE(allen): query bars
    region = default_draw_query_bars(app, region, view_id, face_id);

    // NOTE(allen): FPS hud
    if (show_fps_hud){
        Rect_f32_Pair pair = layout_fps_hud_on_bottom(region, line_height);
        draw_fps_hud(app, frame_info, face_id, pair.max);
        region = pair.min;
        animate_in_n_milliseconds(app, 1000);
    }

    // NOTE(allen): layout line numbers
    b32 show_line_number_margins = def_get_config_b32(vars_save_string_lit("show_line_number_margins"));
    Rect_f32 line_number_rect = {};
    if (show_line_number_margins){
        Rect_f32_Pair pair = layout_line_number_margin(app, buffer, region, digit_advance);
        line_number_rect = pair.min;
        region = pair.max;
    }

    // NOTE(allen): begin buffer render
    Buffer_Point buffer_point = scroll.position;
    Text_Layout_ID text_layout_id = text_layout_create(app, buffer, region, buffer_point);

    // NOTE(allen): draw line numbers
    if (show_line_number_margins){
        draw_line_number_margin(app, view_id, buffer, face_id, text_layout_id, line_number_rect);
    }

    // NOTE(allen): draw the buffer
    vim_render_buffer(app, view_id, face_id, buffer, text_layout_id, region);

    text_layout_free(app, text_layout_id);
    draw_set_clip(app, prev_clip);
}

////////////////////////////////
// NOTE(vim): Hooks

function Implicit_Map_Result
vim_implicit_map(Application_Links *app, String_ID lang, String_ID mode, Input_Event *event){
    Implicit_Map_Result result = {};

    Command_Map_ID map_id = vim_map_normal;
    switch (vim_mode){
        case VimMode_Insert: {map_id = vim_map_insert;}break;
        case VimMode_Replace: {map_id = vim_map_replace;}break;
        case VimMode_Visual: case VimMode_VisualLine: {map_id = vim_map_visual;}break;
    }

    Command_Binding binding = map_get_binding_recursive(&vim_mapping, map_id, event);
    if (binding.custom != 0){
        result.command = binding.custom;
        return(result);
    }

    if (vim_mode != VimMode_Insert){
        if (event->kind == InputEventKind_TextInsert){
            result.command = (vim_mode == VimMode_Replace)?vim_replace_text:vim_handle_text;
            return(result);
        }
        // NOTE(vim): Leave plain keys unhandled so their text event reaches vim_handle_text.
        if (event->kind == InputEventKind_KeyStroke &&
            event->key.first_dependent_text != 0 &&
            !has_modifier(event, KeyCode_Control) &&
            !has_modifier(event, KeyCode_Alt)){
            return(result);
        }
    }

    return(default_implicit_map(app, lang, mode, event));
}

////////////////////////////////
// NOTE(vim): Setup

function void
vim_setup_mapping(Mapping *mapping){
    MappingScope();
    SelectMapping(mapping);

    SelectMap(vim_map_normal);
    Bind(vim_normal_mode,        KeyCode_Escape);
    Bind(vim_return,             KeyCode_Return);
    Bind(vim_left,               KeyCode_Backspace);
    Bind(vim_delete_char,        KeyCode_Delete);
    Bind(vim_left,               KeyCode_Left);
    Bind(vim_right,              KeyCode_Right);
    Bind(vim_up,                 KeyCode_Up);
    Bind(vim_down,               KeyCode_Down);
    Bind(vim_line_start,         KeyCode_Home);
    Bind(vim_line_end_motion,    KeyCode_End);
    Bind(page_up,                KeyCode_PageUp);
    Bind(page_down,              KeyCode_PageDown);
    Bind(vim_redo,               KeyCode_R, KeyCode_Control);
    Bind(vim_half_page_down,     KeyCode_D, KeyCode_Control);
    Bind(vim_half_page_up,       KeyCode_U, KeyCode_Control);
    Bind(page_down,              KeyCode_F, KeyCode_Control);
    Bind(page_up,                KeyCode_B, KeyCode_Control);
    Bind(vim_scroll_down_line,   KeyCode_E, KeyCode_Control);
    Bind(vim_scroll_up_line,     KeyCode_Y, KeyCode_Control);
    Bind(vim_increment_number,   KeyCode_A, KeyCode_Control);
    Bind(vim_decrement_number,   KeyCode_X, KeyCode_Control);
    Bind(jump_to_last_point,     KeyCode_O, KeyCode_Control);
    Bind(vim_window_command,     KeyCode_W, KeyCode_Control);
    Bind(vim_visual_block_unsupported, KeyCode_V, KeyCode_Control);
    Bind(jump_to_definition_at_cursor, KeyCode_RightBracket, KeyCode_Control);

    SelectMap(vim_map_visual);
    ParentMap(vim_map_normal);

    SelectMap(vim_map_insert);
    Bind(vim_normal_mode,        KeyCode_Escape);
    Bind(vim_normal_mode,        KeyCode_LeftBracket, KeyCode_Control);
    Bind(vim_insert_delete_word, KeyCode_W, KeyCode_Control);
    Bind(vim_insert_delete_line, KeyCode_U, KeyCode_Control);
    Bind(backspace_char,         KeyCode_H, KeyCode_Control);
    Bind(vim_insert_register,    KeyCode_R, KeyCode_Control);
    Bind(vim_insert_indent,      KeyCode_T, KeyCode_Control);
    Bind(vim_insert_dedent,      KeyCode_D, KeyCode_Control);
    Bind(word_complete,          KeyCode_N, KeyCode_Control);
    Bind(word_complete,          KeyCode_P, KeyCode_Control);

    SelectMap(vim_map_replace);
    ParentMap(vim_map_insert);
    Bind(vim_left,               KeyCode_Backspace);
}

function void
vim_init(Application_Links *app){
    Thread_Context *tctx = get_thread_context(app);
    vim_map_normal = vars_save_string_lit("keys_vim_normal");
    vim_map_visual = vars_save_string_lit("keys_vim_visual");
    vim_map_insert = vars_save_string_lit("keys_vim_insert");
    vim_map_replace = vars_save_string_lit("keys_vim_replace");

    // NOTE(vim): The vim maps live in their own mapping because the startup hook releases
    // and rebuilds framework_mapping from bindings.4coder.
    mapping_init(tctx, &vim_mapping);
    vim_setup_mapping(&vim_mapping);

    implicit_map_function = vim_implicit_map;
    set_custom_hook(app, HookID_RenderCaller, vim_render_caller);
}

#endif //FCODER_VIM_CPP

// BOTTOM
