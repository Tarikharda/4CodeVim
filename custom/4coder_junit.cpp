/*
4coder_junit.cpp - Runs a Gradle project's JUnit tests from the test file, like the vimrc's
:Test commands: live output in a bottom panel, failures in a jump list (Enter or :cn on a
failure opens the failing line).
*/

// TOP

#if !defined(FCODER_JUNIT_CPP)
#define FCODER_JUNIT_CPP

struct JUnit_State{
    b32 initialized;
    Arena arena;
    Arena failed_arena;
    b32 running;
    b32 stopped;
    Child_Process_ID process;
    String_Const_u8 root;
    String_Const_u8 task;
    List_String_Const_u8 filters;
    b32 has_run;
    List_String_Const_u8 failed;
    View_ID view;
};

global JUnit_State junit_state = {};

global_const String_Const_u8 junit_output_name = string_u8_litexpr("*junit*");
global_const String_Const_u8 junit_failures_name = string_u8_litexpr("*junit failures*");

function JUnit_State*
junit_get_state(void){
    if (!junit_state.initialized){
        junit_state.initialized = true;
        junit_state.arena = make_arena_system();
        junit_state.failed_arena = make_arena_system();
    }
    return(&junit_state);
}

function String_Const_u8
junit_pid_file(Arena *arena){
    return(push_u8_stringf(arena, "/tmp/4coder-junit-%d.pid", (i32)getpid()));
}

function b32
junit_is_annotation(String_Const_u8 line){
    line = string_skip_whitespace(line);
    String_Const_u8 names[] = {
        string_u8_litexpr("@Test"),
        string_u8_litexpr("@ParameterizedTest"),
        string_u8_litexpr("@RepeatedTest"),
    };
    b32 result = false;
    for (i32 i = 0; i < ArrayCount(names); i += 1){
        if (string_match(string_prefix(line, names[i].size), names[i])){
            u8 next = string_get_character(line, names[i].size);
            if (!character_is_alpha_numeric(next) && next != '_'){
                result = true;
                break;
            }
        }
    }
    return(result);
}

// NOTE(junit): The name of the method declared at or below an annotation line.
function String_Const_u8
junit_method_below(Application_Links *app, Arena *arena, Buffer_ID buffer, i64 line_number){
    String_Const_u8 result = {};
    i64 line_count = buffer_get_line_count(app, buffer);
    for (i64 i = line_number; i <= line_number + 8 && i <= line_count && result.size == 0; i += 1){
        String_Const_u8 line = string_skip_whitespace(push_buffer_line(app, arena, buffer, i));
        // NOTE(junit): Skip leading annotations, with their arguments.
        for (;string_get_character(line, 0) == '@';){
            u64 pos = 1;
            for (;pos < line.size && (character_is_alpha_numeric(line.str[pos]) || line.str[pos] == '_' || line.str[pos] == '.'); pos += 1);
            if (pos < line.size && line.str[pos] == '('){
                u64 close = string_find_first(string_skip(line, pos), ')');
                pos = clamp_top(pos + close + 1, line.size);
            }
            line = string_skip_whitespace(string_skip(line, pos));
        }
        for (u64 pos = 0; pos < line.size; pos += 1){
            if (character_is_alpha(line.str[pos]) || line.str[pos] == '_'){
                u64 end = pos;
                for (;end < line.size && (character_is_alpha_numeric(line.str[end]) || line.str[end] == '_'); end += 1);
                u64 after = end;
                for (;after < line.size && character_is_whitespace(line.str[after]); after += 1);
                if (after < line.size && line.str[after] == '('){
                    result = push_string_copy(arena, SCu8(line.str + pos, end - pos));
                    break;
                }
                pos = end;
            }
        }
    }
    return(result);
}

function List_String_Const_u8
junit_test_methods(Application_Links *app, Arena *arena, Buffer_ID buffer){
    List_String_Const_u8 result = {};
    i64 line_count = buffer_get_line_count(app, buffer);
    for (i64 i = 1; i <= line_count; i += 1){
        Temp_Memory temp = begin_temp(arena);
        b32 is_test = junit_is_annotation(push_buffer_line(app, arena, buffer, i));
        end_temp(temp);
        if (is_test){
            String_Const_u8 name = junit_method_below(app, arena, buffer, i);
            if (name.size > 0){
                string_list_push(arena, &result, name);
            }
        }
    }
    return(result);
}

// NOTE(junit): The test the cursor is in, empty above the first one.
function String_Const_u8
junit_nearest_test(Application_Links *app, Arena *arena, View_ID view, Buffer_ID buffer){
    String_Const_u8 result = {};
    i64 line = get_line_number_from_pos(app, buffer, view_get_cursor_pos(app, view));
    for (i64 i = line; i >= 1; i -= 1){
        if (junit_is_annotation(push_buffer_line(app, arena, buffer, i))){
            result = junit_method_below(app, arena, buffer, i);
            break;
        }
    }
    return(result);
}

// NOTE(junit): Full name of the class in this file, empty (with a message) when it isn't a
// test file.
function String_Const_u8
junit_test_class(Application_Links *app, Arena *arena, Buffer_ID buffer){
    String_Const_u8 result = {};
    String_Const_u8 file_name = push_buffer_file_name(app, arena, buffer);
    if (string_find_first(file_name, string_u8_litexpr("/src/test/")) >= file_name.size ||
        !string_match(string_file_extension(file_name), string_u8_litexpr("java"))){
        print_message(app, string_u8_litexpr("JUnit: not a test file (src/test/**/*.java)\n"));
        return(result);
    }
    String_Const_u8 class_name = string_file_without_extension(string_front_of_path(file_name));
    String_Const_u8 package = {};
    i64 line_count = buffer_get_line_count(app, buffer);
    for (i64 i = 1; i <= line_count; i += 1){
        String_Const_u8 line = string_skip_whitespace(push_buffer_line(app, arena, buffer, i));
        if (string_match(string_prefix(line, 8), string_u8_litexpr("package ")) ||
            string_match(string_prefix(line, 8), string_u8_litexpr("package\t"))){
            line = string_skip_whitespace(string_skip(line, 8));
            u64 end = 0;
            for (;end < line.size && (character_is_alpha_numeric(line.str[end]) || line.str[end] == '_' || line.str[end] == '.'); end += 1);
            package = string_prefix(line, end);
            break;
        }
    }
    if (package.size > 0){
        result = push_u8_stringf(arena, "%.*s.%.*s", string_expand(package), string_expand(class_name));
    }
    else{
        result = push_string_copy(arena, class_name);
    }
    return(result);
}

// NOTE(junit): The folder with gradlew and the task prefix of the nearest build file below
// it, like ":cashCardApplet:" (":" for the root project).
function b32
junit_gradle_module(Application_Links *app, Arena *arena, Buffer_ID buffer, String_Const_u8 *root_out, String_Const_u8 *task_out){
    String_Const_u8 dir = push_buffer_file_name(app, arena, buffer);
    if (dir.size > 0){
        dir = string_remove_last_folder(dir);
    }
    else{
        dir = push_hot_directory(app, arena);
    }
    for (;dir.size > 1 && character_is_slash(string_get_character(dir, dir.size - 1));){
        dir = string_chop(dir, 1);
    }
    String_Const_u8 wrapper = push_file_search_up_path(app, arena, dir, string_u8_litexpr("gradlew"));
    if (wrapper.size == 0){
        print_message(app, push_u8_stringf(arena, "JUnit: no gradlew above %.*s\n", string_expand(dir)));
        return(false);
    }
    String_Const_u8 root = string_remove_last_folder(wrapper);
    for (;root.size > 1 && character_is_slash(string_get_character(root, root.size - 1));){
        root = string_chop(root, 1);
    }
    for (;dir.size > root.size;){
        String_Const_u8 groovy = push_u8_stringf(arena, "%.*s/build.gradle", string_expand(dir));
        String_Const_u8 kotlin = push_u8_stringf(arena, "%.*s/build.gradle.kts", string_expand(dir));
        if (file_exists_and_is_file(app, groovy) || file_exists_and_is_file(app, kotlin)){
            break;
        }
        dir = string_remove_last_folder(dir);
        for (;dir.size > 1 && character_is_slash(string_get_character(dir, dir.size - 1));){
            dir = string_chop(dir, 1);
        }
    }
    String_Const_u8 sub = string_skip(dir, root.size);
    String_u8 task = string_u8_push(arena, sub.size + 2);
    for (u64 i = 0; i < sub.size; i += 1){
        string_append_character(&task, (sub.str[i] == '/')?':':sub.str[i]);
    }
    string_append_character(&task, ':');
    *root_out = root;
    *task_out = task.string;
    return(true);
}

function void
junit_show_buffer(Application_Links *app, Buffer_ID buffer){
    JUnit_State *state = junit_get_state();
    // NOTE(junit): Failures are long (absolute path, then the message): wrap them.
    Managed_Scope scope = buffer_get_managed_scope(app, buffer);
    b32 *wrap_lines = scope_attachment(app, scope, buffer_wrap_lines, b32);
    if (wrap_lines != 0){
        *wrap_lines = true;
    }
    View_ID active = get_active_view(app, Access_Always);
    if (!view_exists(app, state->view)){
        state->view = open_footer_panel(app, active);
        view_set_active(app, active);
    }
    view_set_buffer(app, state->view, buffer, 0);
}

function void
junit_append(Application_Links *app, Buffer_ID buffer, String_Const_u8 text){
    i64 size = buffer_get_size(app, buffer);
    buffer_replace_range(app, buffer, Ii64(size), text);
}

function void
junit_start(Application_Links *app, String_Const_u8 root, String_Const_u8 task, List_String_Const_u8 filters){
    JUnit_State *state = junit_get_state();
    Scratch_Block scratch(app);
    if (state->running){
        print_message(app, string_u8_litexpr("JUnit: tests are still running (<leader>tx stops them)\n"));
        return;
    }
    // NOTE(junit): Like IntelliJ: run what is on screen, not the last save.
    save_all_dirty_buffers(app);

    // NOTE(junit): The arguments may point into the state's memory (run again), copy first.
    root = push_string_copy(scratch, root);
    task = push_string_copy(scratch, task);
    List_String_Const_u8 filters_copy = {};
    for (Node_String_Const_u8 *node = filters.first; node != 0; node = node->next){
        string_list_push(scratch, &filters_copy, push_string_copy(scratch, node->string));
    }
    linalloc_clear(&state->arena);
    state->root = push_string_copy(&state->arena, root);
    state->task = push_string_copy(&state->arena, task);
    block_zero_struct(&state->filters);
    for (Node_String_Const_u8 *node = filters_copy.first; node != 0; node = node->next){
        string_list_push(&state->arena, &state->filters, push_string_copy(&state->arena, node->string));
    }
    state->has_run = true;

    // NOTE(junit): cleanTest: Gradle would skip a test run it thinks is up to date. The pid
    // goes to a file so <leader>tx can stop the run.
    List_String_Const_u8 parts = {};
    string_list_pushf(scratch, &parts, "echo $$ > %.*s; exec ./gradlew %.*scleanTest %.*stest --console=plain",
                      string_expand(junit_pid_file(scratch)), string_expand(task), string_expand(task));
    for (Node_String_Const_u8 *node = filters_copy.first; node != 0; node = node->next){
        string_list_pushf(scratch, &parts, " --tests %.*s", string_expand(fuzzy_shell_quote(scratch, node->string)));
    }
    string_list_push(scratch, &parts, string_u8_litexpr(" 2>&1"));
    String_Const_u8 command = string_list_flatten(scratch, parts, StringFill_NullTerminate);

    Child_Process_ID process = create_child_process(app, root, command);
    if (process == 0){
        print_message(app, string_u8_litexpr("JUnit: could not start gradlew\n"));
        return;
    }
    Buffer_ID buffer = buffer_identifier_to_id_create_out_buffer(app, buffer_identifier(junit_output_name));
    set_buffer_system_command(app, process, buffer, CLI_OverlapWithConflict|CLI_CursorAtEnd);
    state->process = process;
    state->running = true;
    state->stopped = false;
    junit_show_buffer(app, buffer);

    String_Const_u8 what = push_u8_stringf(scratch, "%.*stest", string_expand(task));
    if (filters_copy.node_count > 0){
        what = string_list_flatten(scratch, filters_copy, 0, string_u8_litexpr(" "), StringSeparator_NoFlags, StringFill_NoTerminate);
    }
    print_message(app, push_u8_stringf(scratch, "JUnit: running %.*s\n", string_expand(what)));
}

function String_Const_u8
junit_unescape(Arena *arena, String_Const_u8 text){
    String_u8 result = string_u8_push(arena, text.size);
    for (u64 i = 0; i < text.size; i += 1){
        String_Const_u8 rest = string_skip(text, i);
        if (string_match(string_prefix(rest, 4), string_u8_litexpr("&lt;"))){
            string_append_character(&result, '<');
            i += 3;
        }
        else if (string_match(string_prefix(rest, 4), string_u8_litexpr("&gt;"))){
            string_append_character(&result, '>');
            i += 3;
        }
        else if (string_match(string_prefix(rest, 6), string_u8_litexpr("&quot;"))){
            string_append_character(&result, '"');
            i += 5;
        }
        else if (string_match(string_prefix(rest, 6), string_u8_litexpr("&apos;"))){
            string_append_character(&result, '\'');
            i += 5;
        }
        else if (string_match(string_prefix(rest, 5), string_u8_litexpr("&amp;"))){
            string_append_character(&result, '&');
            i += 4;
        }
        else{
            string_append_character(&result, text.str[i]);
        }
    }
    return(result.string);
}

// NOTE(junit): The value of attribute="..." in an XML line, empty when it is missing.
function String_Const_u8
junit_attribute(String_Const_u8 line, String_Const_u8 attribute){
    String_Const_u8 result = {};
    u64 pos = string_find_first(line, attribute);
    if (pos < line.size){
        String_Const_u8 rest = string_skip(line, pos + attribute.size);
        result = string_prefix(rest, string_find_first(rest, '"'));
    }
    return(result);
}

// NOTE(junit): Reads Gradle's XML reports (they hold the message and stack trace of every
// failure), writes the failures as a jump list and reports the counts.
function void
junit_done(Application_Links *app, i64 status){
    JUnit_State *state = junit_get_state();
    state->running = false;
    Scratch_Block scratch(app);

    String_u8 dir_string = string_u8_push(scratch, state->root.size + state->task.size);
    string_append(&dir_string, state->root);
    for (u64 i = 0; i < state->task.size; i += 1){
        string_append_character(&dir_string, (state->task.str[i] == ':')?'/':state->task.str[i]);
    }
    String_Const_u8 dir = dir_string.string;
    String_Const_u8 reports = fuzzy_run_command(scratch, push_u8_stringf(scratch, "cat %.*s/build/test-results/test/*.xml 2>/dev/null",
                                                                         string_expand(fuzzy_shell_quote(scratch, string_chop(dir, 1)))));

    linalloc_clear(&state->failed_arena);
    block_zero_struct(&state->failed);
    i64 total = 0;
    List_String_Const_u8 items = {};
    String_Const_u8 method = {};
    String_Const_u8 class_name = {};
    String_Const_u8 file = {};
    String_Const_u8 text = {};
    b32 in_failure = false;
    i64 line_number = 0;
    List_String_Const_u8 lines = string_split(scratch, reports, (u8*)"\n", 1);
    for (Node_String_Const_u8 *node = lines.first; node != 0; node = node->next){
        String_Const_u8 line = string_skip_whitespace(node->string);
        if (string_match(string_prefix(line, 16), string_u8_litexpr("<testcase name=\""))){
            if (in_failure){
                string_list_pushf(scratch, &items, "%.*s:%lld:1: %.*s\n", string_expand(file), clamp_bot(1, line_number), string_expand(text));
            }
            in_failure = false;
            total += 1;
            method = junit_attribute(line, string_u8_litexpr("name=\""));
            method = string_prefix(method, string_find_first(method, '('));
            class_name = junit_attribute(line, string_u8_litexpr("classname=\""));
            class_name = string_prefix(class_name, string_find_first(class_name, '$'));
        }
        else if (string_match(string_prefix(line, 18), string_u8_litexpr("<failure message=\""))){
            String_Const_u8 message = junit_attribute(line, string_u8_litexpr("message=\""));
            message = string_prefix(message, string_find_first(message, string_u8_litexpr("&#10;")));
            // NOTE(junit): Drop the exception class: "org.opentest4j.AssertionFailedError: ".
            u64 colon = string_find_first(message, string_u8_litexpr(": "));
            b32 is_class = (colon < message.size);
            for (u64 i = 0; i < colon && is_class; i += 1){
                u8 c = message.str[i];
                is_class = (character_is_alpha_numeric(c) || c == '_' || c == '.' || c == '$');
            }
            if (is_class){
                message = string_skip(message, colon + 2);
            }
            String_u8 path = string_u8_push(scratch, dir.size + class_name.size + 20);
            string_append(&path, dir);
            string_append(&path, string_u8_litexpr("src/test/java/"));
            for (u64 i = 0; i < class_name.size; i += 1){
                string_append_character(&path, (class_name.str[i] == '.')?'/':class_name.str[i]);
            }
            string_append(&path, string_u8_litexpr(".java"));
            file = path.string;
            text = push_u8_stringf(scratch, "%.*s: %.*s", string_expand(method), string_expand(junit_unescape(scratch, message)));
            in_failure = true;
            line_number = 0;
            string_list_push(&state->failed_arena, &state->failed,
                             push_u8_stringf(&state->failed_arena, "%.*s.%.*s", string_expand(class_name), string_expand(method)));
        }
        else if (in_failure && line_number == 0 && string_match(string_prefix(line, 3), string_u8_litexpr("at "))){
            // NOTE(junit): The test's own line, not a helper's; frames look like
            // "at app//com.example.PinTest.greets(PinTest.java:12)".
            String_Const_u8 frame = string_skip(line, 3);
            u64 slashes = string_find_first(frame, string_u8_litexpr("//"));
            if (slashes < string_find_first(frame, '(')){
                frame = string_skip(frame, slashes + 2);
            }
            String_Const_u8 name = push_u8_stringf(scratch, "%.*s.%.*s(", string_expand(class_name), string_expand(method));
            if (string_match(string_prefix(frame, name.size), name)){
                u64 close = string_find_first(frame, ')');
                String_Const_u8 location = string_prefix(frame, close);
                u64 colon = string_find_last(location, ':');
                if (colon < location.size){
                    line_number = string_to_integer(string_skip(location, colon + 1), 10);
                }
            }
        }
    }
    if (in_failure){
        string_list_pushf(scratch, &items, "%.*s:%lld:1: %.*s\n", string_expand(file), clamp_bot(1, line_number), string_expand(text));
    }

    // NOTE(junit): No reports: the code didn't compile, the errors are in the output.
    Buffer_ID output = get_buffer_by_name(app, junit_output_name, Access_Always);
    if (items.node_count == 0 && status != 0 && output != 0){
        List_String_Const_u8 out_lines = string_split(scratch, push_whole_buffer(app, scratch, output), (u8*)"\n", 1);
        for (Node_String_Const_u8 *node = out_lines.first; node != 0; node = node->next){
            String_Const_u8 line = node->string;
            u64 error = string_find_first(line, string_u8_litexpr(": error: "));
            if (character_is_slash(string_get_character(line, 0)) && error < line.size){
                String_Const_u8 location = string_prefix(line, error);
                u64 colon = string_find_last(location, ':');
                if (colon < location.size){
                    string_list_pushf(scratch, &items, "%.*s:%.*s:1: %.*s\n",
                                      string_expand(string_prefix(location, colon)),
                                      string_expand(string_skip(location, colon + 1)),
                                      string_expand(string_skip(line, error + 2)));
                }
            }
        }
    }

    String_Const_u8 summary = {};
    if (state->stopped){
        summary = string_u8_litexpr("JUnit: stopped\n");
    }
    else if (status == 0){
        summary = push_u8_stringf(scratch, "JUnit: %lld passed\n", total);
    }
    else if (total > 0){
        summary = push_u8_stringf(scratch, "JUnit: %lld failed, %lld passed\n",
                                  (i64)state->failed.node_count, total - (i64)state->failed.node_count);
    }
    else{
        summary = string_u8_litexpr("JUnit: build failed, no tests ran\n");
    }
    print_message(app, summary);
    if (output != 0){
        junit_append(app, output, push_u8_stringf(scratch, "\n%.*s", string_expand(summary)));
    }

    if (items.node_count == 0){
        // NOTE(junit): Nothing failed: drop the old failures so their lines stop being marked.
        Buffer_ID failures = get_buffer_by_name(app, junit_failures_name, Access_Always);
        if (failures != 0){
            clear_buffer(app, failures);
        }
    }
    else{
        Buffer_ID failures = buffer_identifier_to_id_create_out_buffer(app, buffer_identifier(junit_failures_name));
        clear_buffer(app, failures);
        junit_append(app, failures, summary);
        junit_append(app, failures, string_list_flatten(scratch, items, StringFill_NoTerminate));
        lock_jump_buffer(app, failures);
        junit_show_buffer(app, failures);
        // NOTE(junit): On the summary line, so the first :cn goes to the first failure.
        if (view_exists(app, state->view)){
            view_set_cursor_and_preferred_x(app, state->view, seek_line_col(1, 1));
        }
    }
}

function void
junit_update(Application_Links *app){
    JUnit_State *state = junit_get_state();
    if (state->running){
        Process_State process = child_process_get_state(app, state->process);
        if (!process.is_updating){
            junit_done(app, process.valid?process.return_code:-1);
        }
    }
}

// NOTE(junit): Tests in names: methods of this class, or anything else (starting with a
// capital, or with a dot or star) as a Gradle pattern; no names runs this class.
function void
junit_test(Application_Links *app, List_String_Const_u8 names){
    Scratch_Block scratch(app);
    View_ID view = get_active_view(app, Access_Always);
    Buffer_ID buffer = view_get_buffer(app, view, Access_Always);
    List_String_Const_u8 filters = {};
    if (names.node_count == 0){
        string_list_push(scratch, &names, string_u8_empty);
    }
    for (Node_String_Const_u8 *node = names.first; node != 0; node = node->next){
        String_Const_u8 name = node->string;
        if (character_is_upper(string_get_character(name, 0)) ||
            string_find_first(name, '.') < name.size || string_find_first(name, '*') < name.size){
            string_list_pushf(scratch, &filters, "%s%.*s", (name.str[0] == '*')?"":"*.", string_expand(name));
        }
        else{
            String_Const_u8 class_name = junit_test_class(app, scratch, buffer);
            if (class_name.size == 0){
                return;
            }
            if (name.size > 0){
                string_list_pushf(scratch, &filters, "%.*s.%.*s", string_expand(class_name), string_expand(name));
            }
            else{
                string_list_push(scratch, &filters, class_name);
            }
        }
    }
    String_Const_u8 root = {};
    String_Const_u8 task = {};
    if (junit_gradle_module(app, scratch, buffer, &root, &task)){
        junit_start(app, root, task, filters);
    }
}

function void
junit_test_all(Application_Links *app){
    Scratch_Block scratch(app);
    View_ID view = get_active_view(app, Access_Always);
    Buffer_ID buffer = view_get_buffer(app, view, Access_Always);
    String_Const_u8 root = {};
    String_Const_u8 task = {};
    if (junit_gradle_module(app, scratch, buffer, &root, &task)){
        List_String_Const_u8 none = {};
        junit_start(app, root, task, none);
    }
}

CUSTOM_COMMAND_SIG(junit_test_nearest)
CUSTOM_DOC("Runs the JUnit test the cursor is in (the class when above the first test).")
{
    Scratch_Block scratch(app);
    View_ID view = get_active_view(app, Access_Always);
    Buffer_ID buffer = view_get_buffer(app, view, Access_Always);
    List_String_Const_u8 names = {};
    string_list_push(scratch, &names, junit_nearest_test(app, scratch, view, buffer));
    junit_test(app, names);
}

CUSTOM_COMMAND_SIG(junit_test_class_command)
CUSTOM_DOC("Runs the JUnit tests of the class in this file.")
{
    List_String_Const_u8 names = {};
    junit_test(app, names);
}

CUSTOM_COMMAND_SIG(junit_test_module)
CUSTOM_DOC("Runs every JUnit test of the Gradle module of this file.")
{
    junit_test_all(app);
}

CUSTOM_UI_COMMAND_SIG(junit_test_pick)
CUSTOM_DOC("Lists the tests of this file with fzf style filtering and runs the chosen one.")
{
    Scratch_Block scratch(app);
    View_ID view = get_active_view(app, Access_Always);
    Buffer_ID buffer = view_get_buffer(app, view, Access_Always);
    String_Const_u8 class_name = junit_test_class(app, scratch, buffer);
    if (class_name.size == 0){
        return;
    }
    List_String_Const_u8 methods = junit_test_methods(app, scratch, buffer);
    if (methods.node_count == 0){
        print_message(app, string_u8_litexpr("JUnit: no tests in this file\n"));
        return;
    }
    Lister_Block lister(app, scratch);
    lister_set_query(lister, "Test:");
    lister_set_default_handlers(lister);
    ((Lister*)lister)->fuzzy_filter = true;
    for (Node_String_Const_u8 *node = methods.first; node != 0; node = node->next){
        lister_add_item(lister, node->string, string_u8_empty, node->string.str, 0);
    }
    Lister_Result l_result = run_lister(app, lister);
    if (!l_result.canceled && l_result.user_data != 0){
        List_String_Const_u8 names = {};
        for (Node_String_Const_u8 *node = methods.first; node != 0; node = node->next){
            if (node->string.str == (u8*)l_result.user_data){
                string_list_push(scratch, &names, node->string);
            }
        }
        junit_test(app, names);
    }
}

function void
junit_again(Application_Links *app, b32 failed){
    JUnit_State *state = junit_get_state();
    if (!state->has_run || (failed && state->failed.node_count == 0)){
        print_message(app, failed?string_u8_litexpr("JUnit: no failed tests\n"):string_u8_litexpr("JUnit: nothing ran yet\n"));
    }
    else{
        junit_start(app, state->root, state->task, failed?state->failed:state->filters);
    }
}

CUSTOM_COMMAND_SIG(junit_test_last)
CUSTOM_DOC("Runs the last JUnit run again.")
{
    junit_again(app, false);
}

CUSTOM_COMMAND_SIG(junit_test_failed)
CUSTOM_DOC("Runs the tests that failed in the last JUnit run again.")
{
    junit_again(app, true);
}

CUSTOM_COMMAND_SIG(junit_toggle_panel)
CUSTOM_DOC("Shows or hides the JUnit output panel.")
{
    JUnit_State *state = junit_get_state();
    if (view_exists(app, state->view)){
        view_close(app, state->view);
        state->view = 0;
    }
    else{
        Buffer_ID buffer = get_buffer_by_name(app, junit_output_name, Access_Always);
        if (buffer != 0){
            junit_show_buffer(app, buffer);
        }
    }
}

CUSTOM_COMMAND_SIG(junit_stop)
CUSTOM_DOC("Stops the running JUnit tests.")
{
    JUnit_State *state = junit_get_state();
    if (state->running){
        Scratch_Block scratch(app);
        String_Const_u8 pid = string_skip_chop_whitespace(fuzzy_run_command(scratch, push_u8_stringf(scratch, "cat %.*s 2>/dev/null", string_expand(junit_pid_file(scratch)))));
        if (pid.size > 0 && string_is_integer(pid, 10)){
            kill((pid_t)string_to_integer(pid, 10), SIGTERM);
            state->stopped = true;
        }
    }
}

#endif //FCODER_JUNIT_CPP

// BOTTOM
