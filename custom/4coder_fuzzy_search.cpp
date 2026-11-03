/*
4coder_fuzzy_search.cpp - fzf style pickers for project files, buffers and ripgrep results.
*/

// TOP

#if !defined(FCODER_FUZZY_SEARCH_CPP)
#define FCODER_FUZZY_SEARCH_CPP

// NOTE: Files and matches come from ripgrep (rg --files, rg --vimgrep), so .gitignore is
// respected. Searches run from the git root of the current file, or the hot directory
// when the file is not in a repository. The pickers use the lister's fuzzy_filter.

struct Fuzzy_Grep_Match{
    String_Const_u8 file_name;
    i64 line;
    i64 col;
};

function String_Const_u8
fuzzy_search_root(Application_Links *app, Arena *arena){
    View_ID view = get_active_view(app, Access_Always);
    Buffer_ID buffer = view_get_buffer(app, view, Access_Always);
    String_Const_u8 start = push_buffer_file_name(app, arena, buffer);
    if (start.size > 0){
        start = string_remove_last_folder(start);
    }
    else{
        start = push_hot_directory(app, arena);
    }
    String_Const_u8 result = push_hot_directory(app, arena);
    String_Const_u8 git = push_file_search_up_path(app, arena, start, string_u8_litexpr(".git"));
    if (git.size > 0){
        result = string_remove_last_folder(git);
    }
    for (;result.size > 1 && character_is_slash(string_get_character(result, result.size - 1));){
        result = string_chop(result, 1);
    }
    return(result);
}

function String_Const_u8
fuzzy_shell_quote(Arena *arena, String_Const_u8 string){
    u8 *out = push_array(arena, u8, string.size*4 + 3);
    u64 size = 0;
    out[size++] = '\'';
    for (u64 i = 0; i < string.size; i += 1){
        if (string.str[i] == '\''){
            block_copy(out + size, "'\\''", 4);
            size += 4;
        }
        else{
            out[size++] = string.str[i];
        }
    }
    out[size++] = '\'';
    out[size] = 0;
    return(SCu8(out, size));
}

// NOTE: Runs a shell command and returns everything it wrote to stdout.
function String_Const_u8
fuzzy_run_command(Arena *arena, String_Const_u8 command){
    String_Const_u8 result = {};
#if OS_LINUX || OS_MAC
    String_Const_u8 command_z = push_string_copy(arena, command);
    FILE *pipe = popen((char*)command_z.str, "r");
    if (pipe != 0){
        List_String_Const_u8 chunks = {};
        for (;;){
            u8 *space = push_array(arena, u8, KB(64));
            u64 read_size = fread(space, 1, KB(64), pipe);
            if (read_size == 0){
                break;
            }
            string_list_push(arena, &chunks, SCu8(space, read_size));
        }
        pclose(pipe);
        result = string_list_flatten(arena, chunks);
    }
#endif
    return(result);
}

function String_Const_u8
fuzzy_relative_path(String_Const_u8 root, String_Const_u8 path){
    if (path.size > root.size + 1 && string_match(string_prefix(path, root.size), root) &&
        character_is_slash(path.str[root.size])){
        path = string_skip(path, root.size + 1);
    }
    return(path);
}

function void
fuzzy_find_file_in(Application_Links *app, String_Const_u8 root){
    Scratch_Block scratch(app);
    String_Const_u8 command = push_u8_stringf(scratch, "rg --files --hidden --glob '!.git' -- %.*s 2>/dev/null",
                                              string_expand(fuzzy_shell_quote(scratch, root)));
    String_Const_u8 output = fuzzy_run_command(scratch, command);
    if (output.size == 0){
        print_message(app, string_u8_litexpr("fuzzy_find_file: no files found (is ripgrep installed?)\n"));
        return;
    }

    List_String_Const_u8 lines = string_split(scratch, output, (u8*)"\n", 1);
    String_Const_u8 *paths = push_array(scratch, String_Const_u8, lines.node_count);

    Lister_Block lister(app, scratch);
    lister_set_query(lister, push_u8_stringf(scratch, "Files in %.*s:", string_expand(root)));
    lister_set_default_handlers(lister);
    ((Lister*)lister)->fuzzy_filter = true;
    i32 count = 0;
    for (Node_String_Const_u8 *node = lines.first;
         node != 0;
         node = node->next){
        if (node->string.size > 0){
            paths[count] = node->string;
            lister_add_item(lister, fuzzy_relative_path(root, node->string), SCu8(), IntAsPtr(count), 0);
            count += 1;
        }
    }

    Lister_Result l_result = run_lister(app, lister);
    if (!l_result.canceled){
        View_ID view = get_active_view(app, Access_Always);
        view_open_file(app, view, paths[PtrAsInt(l_result.user_data)], true);
    }
}

// NOTE: The raw rg output also goes to *rg*, locked as the jump buffer, so goto_next_jump
// and goto_prev_jump step through the matches afterwards.
function void
fuzzy_grep_in(Application_Links *app, String_Const_u8 root, String_Const_u8 pattern){
    if (pattern.size == 0){
        return;
    }
    Scratch_Block scratch(app);
    String_Const_u8 command = push_u8_stringf(scratch, "rg --vimgrep --smart-case --color=never -- %.*s %.*s 2>/dev/null",
                                              string_expand(fuzzy_shell_quote(scratch, pattern)),
                                              string_expand(fuzzy_shell_quote(scratch, root)));
    String_Const_u8 output = fuzzy_run_command(scratch, command);
    if (output.size == 0){
        print_message(app, push_u8_stringf(scratch, "fuzzy_grep: no matches for %.*s\n", string_expand(pattern)));
        return;
    }

    {
        String_Const_u8 name = string_u8_litexpr("*rg*");
        Buffer_ID out_buffer = get_buffer_by_name(app, name, Access_Always);
        if (out_buffer == 0){
            out_buffer = create_buffer(app, name, BufferCreate_AlwaysNew);
            buffer_set_setting(app, out_buffer, BufferSetting_Unimportant, true);
            buffer_set_setting(app, out_buffer, BufferSetting_ReadOnly, true);
        }
        clear_buffer(app, out_buffer);
        buffer_replace_range(app, out_buffer, Ii64((i64)0), output);
        lock_jump_buffer(app, out_buffer);
    }

    List_String_Const_u8 lines = string_split(scratch, output, (u8*)"\n", 1);
    Fuzzy_Grep_Match *matches = push_array(scratch, Fuzzy_Grep_Match, lines.node_count);

    Lister_Block lister(app, scratch);
    lister_set_query(lister, push_u8_stringf(scratch, "rg %.*s:", string_expand(pattern)));
    lister_set_default_handlers(lister);
    ((Lister*)lister)->fuzzy_filter = true;
    i32 count = 0;
    for (Node_String_Const_u8 *node = lines.first;
         node != 0;
         node = node->next){
        // NOTE: path:line:column:text
        String_Const_u8 line = node->string;
        u64 colon0 = string_find_first(line, ':');
        u64 colon1 = colon0 + 1 + string_find_first(string_skip(line, colon0 + 1), ':');
        u64 colon2 = colon1 + 1 + string_find_first(string_skip(line, colon1 + 1), ':');
        if (colon2 >= line.size){
            continue;
        }
        String_Const_u8 line_number = string_substring(line, Ii64(colon0 + 1, colon1));
        String_Const_u8 column = string_substring(line, Ii64(colon1 + 1, colon2));
        if (!string_is_integer(line_number, 10) || !string_is_integer(column, 10)){
            continue;
        }
        matches[count].file_name = string_prefix(line, colon0);
        matches[count].line = (i64)string_to_integer(line_number, 10);
        matches[count].col = (i64)string_to_integer(column, 10);
        String_Const_u8 text = string_skip_chop_whitespace(string_skip(line, colon2 + 1));
        String_Const_u8 item = push_u8_stringf(scratch, "%.*s:%.*s: %.*s",
                                               string_expand(fuzzy_relative_path(root, matches[count].file_name)),
                                               string_expand(line_number), string_expand(text));
        lister_add_item(lister, item, SCu8(), IntAsPtr(count), 0);
        count += 1;
    }

    Lister_Result l_result = run_lister(app, lister);
    if (!l_result.canceled){
        Fuzzy_Grep_Match match = matches[PtrAsInt(l_result.user_data)];
        View_ID view = get_active_view(app, Access_Always);
        point_stack_push_view_cursor(app, view);
        if (view_open_file(app, view, match.file_name, true)){
            view_set_cursor_and_preferred_x(app, view, seek_line_col(match.line, match.col));
            center_view(app);
        }
    }
}

function void
fuzzy_grep_query(Application_Links *app, String_Const_u8 init){
    Scratch_Block scratch(app);
    String_Const_u8 root = fuzzy_search_root(app, scratch);
    Query_Bar_Group group(app);
    u8 space[256];
    Query_Bar bar = {};
    bar.prompt = push_u8_stringf(scratch, "rg in %.*s: ", string_expand(root));
    bar.string = SCu8(space, (u64)0);
    bar.string_capacity = sizeof(space);
    if (query_user_general(app, &bar, false, init)){
        fuzzy_grep_in(app, root, bar.string);
    }
}

CUSTOM_UI_COMMAND_SIG(fuzzy_find_file)
CUSTOM_DOC("Lists the files of the current project (ripgrep --files) with fzf style filtering and opens the chosen one.")
{
    Scratch_Block scratch(app);
    fuzzy_find_file_in(app, fuzzy_search_root(app, scratch));
}

CUSTOM_UI_COMMAND_SIG(fuzzy_grep)
CUSTOM_DOC("Queries for a pattern, searches the current project with ripgrep and lists the matches with fzf style filtering.")
{
    fuzzy_grep_query(app, string_u8_empty);
}

CUSTOM_UI_COMMAND_SIG(fuzzy_grep_identifier)
CUSTOM_DOC("Searches the current project with ripgrep for the word under the cursor and lists the matches with fzf style filtering.")
{
    Scratch_Block scratch(app);
    View_ID view = get_active_view(app, Access_ReadVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadVisible);
    Range_i64 range = enclose_pos_alpha_numeric_underscore(app, buffer, view_get_cursor_pos(app, view));
    String_Const_u8 word = push_buffer_range(app, scratch, buffer, range);
    fuzzy_grep_in(app, fuzzy_search_root(app, scratch), word);
}

CUSTOM_UI_COMMAND_SIG(fuzzy_switch_buffer)
CUSTOM_DOC("Lists the open buffers with fzf style filtering and shows the chosen one in this panel.")
{
    Scratch_Block scratch(app);
    Lister_Block lister(app, scratch);
    lister_set_query(lister, "Buffers:");
    lister_set_default_handlers(lister);
    ((Lister*)lister)->fuzzy_filter = true;
    generate_all_buffers_list(app, lister);
    Lister_Result l_result = run_lister(app, lister);
    if (!l_result.canceled){
        View_ID view = get_active_view(app, Access_Always);
        view_set_buffer(app, view, (Buffer_ID)PtrAsInt(l_result.user_data), 0);
    }
}

#endif //FCODER_FUZZY_SEARCH_CPP

// BOTTOM
