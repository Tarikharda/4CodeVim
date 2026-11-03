/*
4coder_lsp.cpp - A small language server client, set up for Java with jdtls.
*/

// TOP

#if !defined(FCODER_LSP_CPP)
#define FCODER_LSP_CPP

// NOTE(lsp): One jdtls serves every Java buffer; buffers from other projects join it as
// extra workspace folders. The server is spawned with a socket for stdin and a pipe for
// stdout. A reader thread splits stdout into messages and wakes the main loop, the tick
// hook handles notifications (diagnostics, status) and sends buffer changes as full
// text, and commands that need an answer (definition, hover, references) send a request
// and wait for it with lsp_request, which keeps processing messages while it waits.
//
// The launch mirrors the user's vim/nvim setup: jdtls from PATH or Mason, lombok as a
// java agent when Mason ships it, lsp-watchdog when installed, and a shared workspace
// under ~/.cache/4coder/jdtls that falls back to shared-2, shared-3 ... while another
// jdtls holds it.

////////////////////////////////
// NOTE(lsp): JSON

typedef i32 Json_Kind;
enum{
    JsonKind_Null,
    JsonKind_Bool,
    JsonKind_Number,
    JsonKind_String,
    JsonKind_Array,
    JsonKind_Object,
};

struct Json_Value{
    Json_Value *next;
    Json_Kind kind;
    String_Const_u8 key;
    String_Const_u8 string;
    b32 boolean;
    Json_Value *first;
    Json_Value *last;
    i32 count;
};

struct Json_Parser{
    Arena *arena;
    String_Const_u8 text;
    u64 pos;
    b32 error;
};

global Json_Value json_null = {};

function void
json_skip_whitespace(Json_Parser *parser){
    for (;parser->pos < parser->text.size && character_is_whitespace(parser->text.str[parser->pos]); parser->pos += 1);
}

function u8
json_peek(Json_Parser *parser){
    u8 result = 0;
    if (parser->pos < parser->text.size){
        result = parser->text.str[parser->pos];
    }
    return(result);
}

function u64
json_encode_utf8(u8 *out, u32 codepoint){
    u64 size = 0;
    if (codepoint < 0x80){
        out[size++] = (u8)codepoint;
    }
    else if (codepoint < 0x800){
        out[size++] = (u8)(0xC0 | (codepoint >> 6));
        out[size++] = (u8)(0x80 | (codepoint & 0x3F));
    }
    else if (codepoint < 0x10000){
        out[size++] = (u8)(0xE0 | (codepoint >> 12));
        out[size++] = (u8)(0x80 | ((codepoint >> 6) & 0x3F));
        out[size++] = (u8)(0x80 | (codepoint & 0x3F));
    }
    else{
        out[size++] = (u8)(0xF0 | (codepoint >> 18));
        out[size++] = (u8)(0x80 | ((codepoint >> 12) & 0x3F));
        out[size++] = (u8)(0x80 | ((codepoint >> 6) & 0x3F));
        out[size++] = (u8)(0x80 | (codepoint & 0x3F));
    }
    return(size);
}

function u32
json_parse_hex4(Json_Parser *parser){
    u32 result = 0;
    if (parser->pos + 4 <= parser->text.size){
        String_Const_u8 hex = SCu8(parser->text.str + parser->pos, 4);
        if (string_is_integer(hex, 16)){
            result = (u32)string_to_integer(hex, 16);
        }
        parser->pos += 4;
    }
    else{
        parser->error = true;
    }
    return(result);
}

function String_Const_u8
json_parse_string(Json_Parser *parser){
    String_Const_u8 result = {};
    if (json_peek(parser) != '"'){
        parser->error = true;
        return(result);
    }
    parser->pos += 1;
    u64 start = parser->pos;
    u64 end = start;
    for (;end < parser->text.size && parser->text.str[end] != '"'; end += 1){
        if (parser->text.str[end] == '\\'){
            end += 1;
        }
    }
    u8 *out = push_array(parser->arena, u8, end - start + 1);
    u64 size = 0;
    for (;parser->pos < parser->text.size;){
        u8 c = parser->text.str[parser->pos];
        parser->pos += 1;
        if (c == '"'){
            break;
        }
        if (c != '\\'){
            out[size++] = c;
            continue;
        }
        u8 e = json_peek(parser);
        parser->pos += 1;
        switch (e){
            case 'n': {out[size++] = '\n';}break;
            case 't': {out[size++] = '\t';}break;
            case 'r': {out[size++] = '\r';}break;
            case 'b': {out[size++] = '\b';}break;
            case 'f': {out[size++] = '\f';}break;
            case 'u':
            {
                u32 codepoint = json_parse_hex4(parser);
                if (0xD800 <= codepoint && codepoint < 0xDC00 &&
                    parser->pos + 6 <= parser->text.size &&
                    parser->text.str[parser->pos] == '\\' && parser->text.str[parser->pos + 1] == 'u'){
                    parser->pos += 2;
                    u32 low = json_parse_hex4(parser);
                    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                }
                size += json_encode_utf8(out + size, codepoint);
            }break;
            default: {out[size++] = e;}break;
        }
    }
    result = SCu8(out, size);
    return(result);
}

function Json_Value*
json_parse_value(Json_Parser *parser){
    Json_Value *value = push_array_zero(parser->arena, Json_Value, 1);
    json_skip_whitespace(parser);
    u8 c = json_peek(parser);
    switch (c){
        case '{': case '[':
        {
            b32 is_object = (c == '{');
            u8 close = is_object?'}':']';
            value->kind = is_object?JsonKind_Object:JsonKind_Array;
            parser->pos += 1;
            for (;!parser->error;){
                json_skip_whitespace(parser);
                if (json_peek(parser) == close){
                    parser->pos += 1;
                    break;
                }
                String_Const_u8 key = {};
                if (is_object){
                    key = json_parse_string(parser);
                    json_skip_whitespace(parser);
                    if (json_peek(parser) != ':'){
                        parser->error = true;
                        break;
                    }
                    parser->pos += 1;
                }
                Json_Value *child = json_parse_value(parser);
                child->key = key;
                sll_queue_push(value->first, value->last, child);
                value->count += 1;
                json_skip_whitespace(parser);
                if (json_peek(parser) == ','){
                    parser->pos += 1;
                }
                else if (json_peek(parser) != close){
                    parser->error = true;
                }
            }
        }break;

        case '"':
        {
            value->kind = JsonKind_String;
            value->string = json_parse_string(parser);
        }break;

        case 't': case 'f': case 'n':
        {
            String_Const_u8 rest = string_skip(parser->text, parser->pos);
            if (string_match(string_prefix(rest, 4), string_u8_litexpr("true"))){
                value->kind = JsonKind_Bool;
                value->boolean = true;
                parser->pos += 4;
            }
            else if (string_match(string_prefix(rest, 5), string_u8_litexpr("false"))){
                value->kind = JsonKind_Bool;
                parser->pos += 5;
            }
            else if (string_match(string_prefix(rest, 4), string_u8_litexpr("null"))){
                parser->pos += 4;
            }
            else{
                parser->error = true;
            }
        }break;

        default:
        {
            u64 start = parser->pos;
            for (;parser->pos < parser->text.size;){
                u8 d = parser->text.str[parser->pos];
                if (character_is_base10(d) || d == '-' || d == '+' || d == '.' || d == 'e' || d == 'E'){
                    parser->pos += 1;
                }
                else{
                    break;
                }
            }
            if (parser->pos == start){
                parser->error = true;
            }
            value->kind = JsonKind_Number;
            value->string = SCu8(parser->text.str + start, parser->pos - start);
        }break;
    }
    return(value);
}

function Json_Value*
json_parse(Arena *arena, String_Const_u8 text){
    Json_Parser parser = {};
    parser.arena = arena;
    parser.text = text;
    Json_Value *result = json_parse_value(&parser);
    if (parser.error){
        result = &json_null;
    }
    return(result);
}

function Json_Value*
json_get(Json_Value *value, char *key){
    Json_Value *result = &json_null;
    if (value != 0 && value->kind == JsonKind_Object){
        String_Const_u8 key_string = SCu8(key);
        for (Json_Value *child = value->first; child != 0; child = child->next){
            if (string_match(child->key, key_string)){
                result = child;
                break;
            }
        }
    }
    return(result);
}

function i64
json_i64(Json_Value *value){
    i64 result = 0;
    if (value->kind == JsonKind_Number && value->string.size > 0){
        String_Const_u8 digits = value->string;
        b32 negative = (digits.str[0] == '-');
        if (negative){
            digits = string_skip(digits, 1);
        }
        u64 size = 0;
        for (;size < digits.size && character_is_base10(digits.str[size]); size += 1);
        result = (i64)string_to_integer(string_prefix(digits, size), 10);
        if (negative){
            result = -result;
        }
    }
    return(result);
}

function String_Const_u8
json_escape(Arena *arena, String_Const_u8 string){
    u8 *out = push_array(arena, u8, string.size*6 + 1);
    u64 size = 0;
    for (u64 i = 0; i < string.size; i += 1){
        u8 c = string.str[i];
        switch (c){
            case '"': {out[size++] = '\\'; out[size++] = '"';}break;
            case '\\': {out[size++] = '\\'; out[size++] = '\\';}break;
            case '\n': {out[size++] = '\\'; out[size++] = 'n';}break;
            case '\r': {out[size++] = '\\'; out[size++] = 'r';}break;
            case '\t': {out[size++] = '\\'; out[size++] = 't';}break;
            default:
            {
                if (c < 0x20){
                    String_Const_u8 hex = push_u8_stringf(arena, "\\u%04x", (i32)c);
                    block_copy(out + size, hex.str, hex.size);
                    size += hex.size;
                }
                else{
                    out[size++] = c;
                }
            }break;
        }
    }
    out[size] = 0;
    return(SCu8(out, size));
}

////////////////////////////////
// NOTE(lsp): Paths and positions

function String_Const_u8
lsp_uri_from_path(Arena *arena, String_Const_u8 path){
    u8 *out = push_array(arena, u8, path.size*3 + 8);
    u64 size = 0;
    block_copy(out, "file://", 7);
    size = 7;
    for (u64 i = 0; i < path.size; i += 1){
        u8 c = path.str[i];
        if (character_is_alpha_numeric(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~'){
            out[size++] = c;
        }
        else{
            String_Const_u8 hex = push_u8_stringf(arena, "%%%02X", (i32)c);
            block_copy(out + size, hex.str, hex.size);
            size += hex.size;
        }
    }
    out[size] = 0;
    return(SCu8(out, size));
}

function String_Const_u8
lsp_path_from_uri(Arena *arena, String_Const_u8 uri){
    if (string_match(string_prefix(uri, 7), string_u8_litexpr("file://"))){
        uri = string_skip(uri, 7);
    }
    u8 *out = push_array(arena, u8, uri.size + 1);
    u64 size = 0;
    for (u64 i = 0; i < uri.size; i += 1){
        if (uri.str[i] == '%' && i + 2 < uri.size && string_is_integer(SCu8(uri.str + i + 1, 2), 16)){
            out[size++] = (u8)string_to_integer(SCu8(uri.str + i + 1, 2), 16);
            i += 2;
        }
        else{
            out[size++] = uri.str[i];
        }
    }
    out[size] = 0;
    return(SCu8(out, size));
}

// NOTE(lsp): LSP columns count UTF-16 code units, buffers count bytes.
function i64
lsp_pos_from_position(Application_Links *app, Buffer_ID buffer, i64 line, i64 character){
    Scratch_Block scratch(app);
    i64 line_number = line + 1;
    if (line_number > buffer_get_line_count(app, buffer)){
        return(buffer_get_size(app, buffer));
    }
    Range_i64 range = Ii64(get_line_start_pos(app, buffer, line_number), get_line_end_pos(app, buffer, line_number));
    String_Const_u8 text = push_buffer_range(app, scratch, buffer, range);
    i64 units = 0;
    u64 i = 0;
    for (;i < text.size && units < character;){
        Character_Consume_Result consume = utf8_consume(text.str + i, text.size - i);
        units += (consume.codepoint >= 0x10000)?2:1;
        i += clamp_bot(1, consume.inc);
    }
    return(range.min + clamp_top(i, text.size));
}

function void
lsp_position_from_pos(Application_Links *app, Buffer_ID buffer, i64 pos, i64 *line_out, i64 *character_out){
    Scratch_Block scratch(app);
    i64 line_number = get_line_number_from_pos(app, buffer, pos);
    i64 start = get_line_start_pos(app, buffer, line_number);
    String_Const_u8 text = push_buffer_range(app, scratch, buffer, Ii64(start, pos));
    i64 units = 0;
    for (u64 i = 0; i < text.size;){
        Character_Consume_Result consume = utf8_consume(text.str + i, text.size - i);
        units += (consume.codepoint >= 0x10000)?2:1;
        i += clamp_bot(1, consume.inc);
    }
    *line_out = line_number - 1;
    *character_out = units;
}

function String_Const_u8
lsp_text_document_position(Application_Links *app, Arena *arena, Buffer_ID buffer, String_Const_u8 uri, i64 pos){
    i64 line = 0;
    i64 character = 0;
    lsp_position_from_pos(app, buffer, pos, &line, &character);
    return(push_u8_stringf(arena, "{\"textDocument\":{\"uri\":\"%.*s\"},\"position\":{\"line\":%lld,\"character\":%lld}}",
                           string_expand(json_escape(arena, uri)), line, character));
}

////////////////////////////////
// NOTE(lsp): Connection

struct Lsp_Message{
    Lsp_Message *next;
    String_Const_u8 body;
};

struct Lsp_Connection{
    i32 pid;
    i32 in_fd;
    i32 out_fd;
    System_Mutex mutex;
    Lsp_Message *first;
    Lsp_Message *last;
    b32 closed;
};

#if OS_LINUX || OS_MAC
function b32
lsp_read_exact(i32 fd, u8 *dst, u64 size){
    b32 result = true;
    for (;size > 0;){
        ssize_t n = read(fd, dst, size);
        if (n < 0 && errno == EINTR){
            continue;
        }
        if (n <= 0){
            result = false;
            break;
        }
        dst += n;
        size -= n;
    }
    return(result);
}

function b32
lsp_write_all(i32 fd, u8 *src, u64 size){
    b32 result = true;
#if OS_LINUX
    i32 flags = MSG_NOSIGNAL;
#else
    i32 flags = 0;
#endif
    for (;size > 0;){
        ssize_t n = send(fd, src, size, flags);
        if (n < 0 && errno == EINTR){
            continue;
        }
        if (n <= 0){
            result = false;
            break;
        }
        src += n;
        size -= n;
    }
    return(result);
}

// NOTE(lsp): Runs on its own thread. Messages are "Content-Length: N\r\n\r\n" + N bytes.
function void
lsp_reader_thread(void *ptr){
    Lsp_Connection *connection = (Lsp_Connection*)ptr;
    Base_Allocator *allocator = get_base_allocator_system();
    for (;;){
        u8 header[1024];
        u64 header_size = 0;
        b32 ok = true;
        for (;;){
            if (header_size >= sizeof(header) || !lsp_read_exact(connection->out_fd, header + header_size, 1)){
                ok = false;
                break;
            }
            header_size += 1;
            if (header_size >= 4 && string_match(SCu8(header + header_size - 4, 4), string_u8_litexpr("\r\n\r\n"))){
                break;
            }
        }
        if (!ok){
            break;
        }

        String_Const_u8 header_string = SCu8(header, header_size);
        String_Const_u8 key = string_u8_litexpr("Content-Length:");
        u64 key_pos = string_find_first(header_string, key);
        u64 content_length = 0;
        for (u64 i = key_pos + key.size; i < header_size; i += 1){
            u8 c = header[i];
            if (character_is_base10(c)){
                content_length = content_length*10 + (c - '0');
            }
            else if (c != ' '){
                break;
            }
        }
        if (content_length == 0){
            continue;
        }

        Lsp_Message *message = (Lsp_Message*)base_allocate(allocator, sizeof(Lsp_Message) + content_length).str;
        message->next = 0;
        message->body = SCu8((u8*)(message + 1), content_length);
        if (!lsp_read_exact(connection->out_fd, message->body.str, content_length)){
            base_free(allocator, message);
            break;
        }
        system_mutex_acquire(connection->mutex);
        sll_queue_push(connection->first, connection->last, message);
        system_mutex_release(connection->mutex);
        system_signal_step(0);
    }
    system_mutex_acquire(connection->mutex);
    connection->closed = true;
    system_mutex_release(connection->mutex);
    system_signal_step(0);
}

function Lsp_Connection*
lsp_spawn(char **argv, char *dir){
    i32 in_pair[2];
    i32 out_pipe[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, in_pair) != 0){
        return(0);
    }
    if (pipe(out_pipe) != 0){
        close(in_pair[0]);
        close(in_pair[1]);
        return(0);
    }
    pid_t pid = fork();
    if (pid == 0){
        dup2(in_pair[1], 0);
        dup2(out_pipe[1], 1);
        i32 null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0){
            dup2(null_fd, 2);
        }
        for (i32 fd = 3; fd < 1024; fd += 1){
            close(fd);
        }
        if (chdir(dir) != 0){
            _exit(126);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(in_pair[1]);
    close(out_pipe[1]);
    if (pid < 0){
        close(in_pair[0]);
        close(out_pipe[0]);
        return(0);
    }

    Base_Allocator *allocator = get_base_allocator_system();
    Lsp_Connection *connection = (Lsp_Connection*)base_allocate(allocator, sizeof(Lsp_Connection)).str;
    block_zero_struct(connection);
    connection->pid = (i32)pid;
    connection->in_fd = in_pair[0];
    connection->out_fd = out_pipe[0];
    connection->mutex = system_mutex_make();
    system_thread_launch(lsp_reader_thread, connection);
    return(connection);
}

function void
lsp_kill(Lsp_Connection *connection){
    kill((pid_t)connection->pid, SIGTERM);
    close(connection->in_fd);
}

function void
lsp_reap(Lsp_Connection *connection){
    waitpid((pid_t)connection->pid, 0, WNOHANG);
}
#else
function Lsp_Connection*
lsp_spawn(char **argv, char *dir){
    return(0);
}
function b32
lsp_write_all(i32 fd, u8 *src, u64 size){
    return(false);
}
function void
lsp_kill(Lsp_Connection *connection){}
function void
lsp_reap(Lsp_Connection *connection){}
#endif

////////////////////////////////
// NOTE(lsp): Client state

struct Lsp_Diagnostic{
    Range_i64 range;
    i64 line;
    i32 severity;
    String_Const_u8 message;
};

struct Lsp_Document{
    Lsp_Document *next;
    Buffer_ID buffer;
    String_Const_u8 path;
    String_Const_u8 uri;
    i32 version;
    History_Record_Index synced_history;
    i64 synced_size;
    Arena diagnostic_arena;
    Lsp_Diagnostic *diagnostics;
    i32 diagnostic_count;
};

struct Lsp_Pending{
    i64 id;
    b32 done;
    Lsp_Message *response;
};

global Lsp_Connection *lsp_connection = 0;
global b32 lsp_initialized = false;
global b32 lsp_unavailable = false;
global i64 lsp_next_id = 1;
global i64 lsp_initialize_id = 0;
global Arena lsp_arena = {};
global Lsp_Document *lsp_first_document = 0;
global String_Const_u8 lsp_roots[64] = {};
global i32 lsp_root_count = 0;
global Lsp_Pending lsp_pending[16] = {};
global u8 lsp_status_space[96];
global String_Const_u8 lsp_status = {};
global u64 lsp_last_scan_time = 0;

global Arena lsp_hover_arena = {};
global String_Const_u8 lsp_hover_text = {};
global View_ID lsp_hover_view = 0;
global i64 lsp_hover_pos = 0;

CUSTOM_ID(colors, defcolor_lsp_error);
CUSTOM_ID(colors, defcolor_lsp_warning);
CUSTOM_ID(colors, defcolor_lsp_info);
CUSTOM_ID(colors, defcolor_lsp_popup_back);

function void
lsp_set_status(String_Const_u8 status){
    u64 size = clamp_top(status.size, sizeof(lsp_status_space));
    block_copy(lsp_status_space, status.str, size);
    lsp_status = SCu8(lsp_status_space, size);
}

function b32
lsp_buffer_is_java(Application_Links *app, Buffer_ID buffer){
    Scratch_Block scratch(app);
    String_Const_u8 file_name = push_buffer_file_name(app, scratch, buffer);
    return(file_name.size > 0 && string_match(string_file_extension(file_name), string_u8_litexpr("java")));
}

function Lsp_Document*
lsp_document_from_buffer(Buffer_ID buffer){
    Lsp_Document *result = 0;
    for (Lsp_Document *doc = lsp_first_document; doc != 0; doc = doc->next){
        if (doc->buffer == buffer){
            result = doc;
            break;
        }
    }
    return(result);
}

function Lsp_Document*
lsp_document_from_path(String_Const_u8 path){
    Lsp_Document *result = 0;
    for (Lsp_Document *doc = lsp_first_document; doc != 0; doc = doc->next){
        if (string_match(doc->path, path)){
            result = doc;
            break;
        }
    }
    return(result);
}

function void
lsp_send(Application_Links *app, String_Const_u8 body){
    if (lsp_connection != 0){
        Scratch_Block scratch(app);
        String_Const_u8 header = push_u8_stringf(scratch, "Content-Length: %llu\r\n\r\n", body.size);
        lsp_write_all(lsp_connection->in_fd, header.str, header.size);
        lsp_write_all(lsp_connection->in_fd, body.str, body.size);
    }
}

function void
lsp_notify(Application_Links *app, char *method, String_Const_u8 params){
    Scratch_Block scratch(app);
    lsp_send(app, push_u8_stringf(scratch, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%.*s}", method, string_expand(params)));
}

function i64
lsp_send_request(Application_Links *app, char *method, String_Const_u8 params){
    Scratch_Block scratch(app);
    i64 id = lsp_next_id;
    lsp_next_id += 1;
    lsp_send(app, push_u8_stringf(scratch, "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"method\":\"%s\",\"params\":%.*s}", id, method, string_expand(params)));
    return(id);
}

////////////////////////////////
// NOTE(lsp): jdtls setup

global char *lsp_java_root_markers[] = {
    "settings.gradle", "settings.gradle.kts", "build.gradle", "build.gradle.kts",
    "pom.xml", "build.xml", ".project", ".classpath", "mvnw", "gradlew", ".git",
};

// NOTE(lsp): The closest parent folder of the file that holds one of the root markers.
function String_Const_u8
lsp_java_root(Application_Links *app, Arena *arena, String_Const_u8 file_name){
    String_Const_u8 file_dir = string_remove_last_folder(file_name);
    String_Const_u8 dir = file_dir;
    String_Const_u8 result = {};
    for (;dir.size > 0 && result.size == 0;){
        for (;dir.size > 1 && character_is_slash(string_get_character(dir, dir.size - 1));){
            dir = string_chop(dir, 1);
        }
        for (i32 i = 0; i < ArrayCount(lsp_java_root_markers); i += 1){
            String_Const_u8 marker = push_u8_stringf(arena, "%.*s/%s", string_expand(dir), lsp_java_root_markers[i]);
            if (file_exists(app, marker)){
                result = dir;
                break;
            }
        }
        if (dir.size <= 1){
            break;
        }
        dir = string_remove_last_folder(dir);
    }
    if (result.size == 0){
        result = file_dir;
        for (;result.size > 1 && character_is_slash(string_get_character(result, result.size - 1));){
            result = string_chop(result, 1);
        }
    }
    return(push_string_copy(arena, result));
}

function String_Const_u8
lsp_expand_home(Arena *arena, String_Const_u8 path){
    char *home = getenv("HOME");
    if (path.size > 0 && path.str[0] == '~' && home != 0){
        path = push_u8_stringf(arena, "%s%.*s", home, string_expand(string_skip(path, 1)));
    }
    return(path);
}

function b32
lsp_is_executable(Arena *arena, String_Const_u8 path){
    b32 result = false;
#if OS_LINUX || OS_MAC
    String_Const_u8 path_z = push_string_copy(arena, path);
    result = (access((char*)path_z.str, X_OK) == 0);
#endif
    return(result);
}

function String_Const_u8
lsp_find_jdtls(Application_Links *app, Arena *arena){
    String_Const_u8 result = def_get_config_string(arena, vars_save_string_lit("lsp_java_command"));
    result = lsp_expand_home(arena, result);
    if (result.size > 0 && lsp_is_executable(arena, result)){
        return(result);
    }
    result = string_u8_empty;
    char *path_env = getenv("PATH");
    if (path_env != 0){
        List_String_Const_u8 dirs = string_split(arena, SCu8(path_env), (u8*)":", 1);
        for (Node_String_Const_u8 *node = dirs.first; node != 0; node = node->next){
            String_Const_u8 candidate = push_u8_stringf(arena, "%.*s/jdtls", string_expand(node->string));
            if (lsp_is_executable(arena, candidate)){
                result = candidate;
                break;
            }
        }
    }
    if (result.size == 0){
        String_Const_u8 mason = lsp_expand_home(arena, string_u8_litexpr("~/.local/share/nvim/mason/bin/jdtls"));
        if (lsp_is_executable(arena, mason)){
            result = mason;
        }
    }
    return(result);
}

// NOTE(lsp): Eclipse locks a workspace, so skip the ones another jdtls is running on.
function String_Const_u8
lsp_java_workspace(Arena *arena){
    String_Const_u8 base = lsp_expand_home(arena, string_u8_litexpr("~/.cache/4coder/jdtls"));
    String_Const_u8 result = {};
    for (i32 i = 1; i <= 9 && result.size == 0; i += 1){
        String_Const_u8 dir = (i == 1)?push_u8_stringf(arena, "%.*s/shared", string_expand(base)):push_u8_stringf(arena, "%.*s/shared-%d", string_expand(base), i);
        String_Const_u8 command = push_u8_stringf(arena, "pgrep -f -- '-data %.*s/workspace( |$)' > /dev/null 2>&1", string_expand(dir));
        if (system((char*)command.str) != 0){
            result = dir;
        }
    }
    if (result.size == 0){
        result = push_u8_stringf(arena, "%.*s/shared-%d", string_expand(base), (i32)getpid());
    }
    return(result);
}

function String_Const_u8
lsp_java_settings(Application_Links *app, Arena *arena){
    List_String_Const_u8 parts = {};
    String_Const_u8 user_name = def_get_config_string(arena, vars_save_string_lit("user_name"));
    if (user_name.size > 0 && !string_match(user_name, string_u8_litexpr("not-set"))){
        string_list_pushf(arena, &parts, "\"templates\":{\"typeComment\":[\"/**\",\" * ${tags}\",\" * @author %.*s\",\" */\"]}",
                          string_expand(json_escape(arena, user_name)));
    }
    String_Const_u8 jdk8 = lsp_expand_home(arena, def_get_config_string(arena, vars_save_string_lit("java_jdk8_path")));
    if (jdk8.size > 0 && file_exists(app, jdk8)){
        string_list_pushf(arena, &parts, "\"configuration\":{\"runtimes\":[{\"name\":\"JavaSE-1.8\",\"path\":\"%.*s\",\"default\":true}]}",
                          string_expand(json_escape(arena, jdk8)));
    }
    String_Const_u8 java = string_list_flatten(arena, parts, 0, string_u8_litexpr(","), 0, StringFill_NoTerminate);
    return(push_u8_stringf(arena, "{%.*s}", string_expand(java)));
}

function void
lsp_start_java(Application_Links *app, String_Const_u8 root){
    Scratch_Block scratch(app);
    String_Const_u8 jdtls = lsp_find_jdtls(app, scratch);
    if (jdtls.size == 0){
        lsp_unavailable = true;
        print_message(app, string_u8_litexpr("lsp: jdtls not found; install it with Mason or put it on PATH, or set lsp_java_command\n"));
        return;
    }

    String_Const_u8 workspace = lsp_java_workspace(scratch);
    String_Const_u8 lombok = lsp_expand_home(scratch, string_u8_litexpr("~/.local/share/nvim/mason/share/jdtls/lombok.jar"));
    String_Const_u8 watchdog = lsp_expand_home(scratch, string_u8_litexpr("~/.config/nvim/bin/lsp-watchdog"));

    char *argv[16] = {};
    i32 argc = 0;
    if (lsp_is_executable(scratch, watchdog)){
        argv[argc++] = (char*)push_string_copy(scratch, watchdog).str;
    }
    argv[argc++] = (char*)push_string_copy(scratch, jdtls).str;
    if (file_exists(app, lombok)){
        argv[argc++] = (char*)push_u8_stringf(scratch, "--jvm-arg=-javaagent:%.*s", string_expand(lombok)).str;
    }
    argv[argc++] = "-configuration";
    argv[argc++] = (char*)push_u8_stringf(scratch, "%.*s/config", string_expand(workspace)).str;
    argv[argc++] = "-data";
    argv[argc++] = (char*)push_u8_stringf(scratch, "%.*s/workspace", string_expand(workspace)).str;

    String_Const_u8 root_z = push_string_copy(scratch, root);
    lsp_connection = lsp_spawn(argv, (char*)root_z.str);
    if (lsp_connection == 0){
        lsp_unavailable = true;
        print_message(app, string_u8_litexpr("lsp: could not start jdtls\n"));
        return;
    }
    print_message(app, push_u8_stringf(scratch, "lsp: started %.*s for %.*s\n", string_expand(jdtls), string_expand(root)));
    lsp_set_status(string_u8_litexpr("starting"));

    lsp_root_count = 0;
    lsp_roots[lsp_root_count++] = push_string_copy(&lsp_arena, root);

    String_Const_u8 root_uri = json_escape(scratch, lsp_uri_from_path(scratch, root));
    String_Const_u8 root_name = json_escape(scratch, root);
    String_Const_u8 settings = lsp_java_settings(app, scratch);
    String_Const_u8 params = push_u8_stringf(scratch,
                                             "{\"processId\":%d,\"rootUri\":\"%.*s\",\"rootPath\":\"%.*s\","
                                             "\"workspaceFolders\":[{\"uri\":\"%.*s\",\"name\":\"%.*s\"}],"
                                             "\"capabilities\":{"
                                             "\"workspace\":{\"workspaceFolders\":true,\"configuration\":true},"
                                             "\"textDocument\":{"
                                             "\"synchronization\":{\"didSave\":false},"
                                             "\"hover\":{\"contentFormat\":[\"markdown\",\"plaintext\"]},"
                                             "\"definition\":{\"linkSupport\":true},"
                                             "\"references\":{},"
                                             "\"publishDiagnostics\":{\"relatedInformation\":false}}},"
                                             "\"initializationOptions\":{\"settings\":{\"java\":%.*s},"
                                             "\"extendedClientCapabilities\":{\"classFileContentsSupport\":true}}}",
                                             (i32)getpid(), string_expand(root_uri), string_expand(root_name),
                                             string_expand(root_uri), string_expand(root_name), string_expand(settings));
    lsp_initialize_id = lsp_send_request(app, "initialize", params);
}

////////////////////////////////
// NOTE(lsp): Documents

function void
lsp_add_root(Application_Links *app, String_Const_u8 root){
    for (i32 i = 0; i < lsp_root_count; i += 1){
        if (string_match(lsp_roots[i], root)){
            return;
        }
    }
    if (lsp_root_count < ArrayCount(lsp_roots)){
        lsp_roots[lsp_root_count++] = push_string_copy(&lsp_arena, root);
        Scratch_Block scratch(app);
        lsp_notify(app, "workspace/didChangeWorkspaceFolders",
                   push_u8_stringf(scratch, "{\"event\":{\"added\":[{\"uri\":\"%.*s\",\"name\":\"%.*s\"}],\"removed\":[]}}",
                                   string_expand(json_escape(scratch, lsp_uri_from_path(scratch, root))),
                                   string_expand(json_escape(scratch, root))));
    }
}

function void
lsp_open_document(Application_Links *app, Buffer_ID buffer){
    Scratch_Block scratch(app);
    String_Const_u8 path = push_buffer_file_name(app, scratch, buffer);
    lsp_add_root(app, lsp_java_root(app, scratch, path));

    Lsp_Document *doc = push_array_zero(&lsp_arena, Lsp_Document, 1);
    doc->buffer = buffer;
    doc->path = push_string_copy(&lsp_arena, path);
    doc->uri = lsp_uri_from_path(&lsp_arena, path);
    doc->version = 1;
    doc->synced_history = buffer_history_get_current_state_index(app, buffer);
    doc->synced_size = buffer_get_size(app, buffer);
    doc->diagnostic_arena = make_arena_system();
    sll_stack_push(lsp_first_document, doc);

    String_Const_u8 text = push_whole_buffer(app, scratch, buffer);
    lsp_notify(app, "textDocument/didOpen",
               push_u8_stringf(scratch, "{\"textDocument\":{\"uri\":\"%.*s\",\"languageId\":\"java\",\"version\":1,\"text\":\"%.*s\"}}",
                               string_expand(json_escape(scratch, doc->uri)), string_expand(json_escape(scratch, text))));
}

function void
lsp_sync_document(Application_Links *app, Lsp_Document *doc){
    History_Record_Index history = buffer_history_get_current_state_index(app, doc->buffer);
    i64 size = buffer_get_size(app, doc->buffer);
    if (history != doc->synced_history || size != doc->synced_size){
        Scratch_Block scratch(app);
        doc->synced_history = history;
        doc->synced_size = size;
        doc->version += 1;
        String_Const_u8 text = push_whole_buffer(app, scratch, doc->buffer);
        lsp_notify(app, "textDocument/didChange",
                   push_u8_stringf(scratch, "{\"textDocument\":{\"uri\":\"%.*s\",\"version\":%d},\"contentChanges\":[{\"text\":\"%.*s\"}]}",
                                   string_expand(json_escape(scratch, doc->uri)), doc->version,
                                   string_expand(json_escape(scratch, text))));
    }
}

function void
lsp_reset_documents(void){
    for (Lsp_Document *doc = lsp_first_document; doc != 0; doc = doc->next){
        linalloc_clear(&doc->diagnostic_arena);
    }
    lsp_first_document = 0;
    lsp_root_count = 0;
}

// NOTE(lsp): Opens every Java buffer with the server and closes the ones that are gone.
function void
lsp_scan_buffers(Application_Links *app){
    for (Lsp_Document **ptr = &lsp_first_document; *ptr != 0;){
        Lsp_Document *doc = *ptr;
        if (!buffer_exists(app, doc->buffer)){
            Scratch_Block scratch(app);
            lsp_notify(app, "textDocument/didClose",
                       push_u8_stringf(scratch, "{\"textDocument\":{\"uri\":\"%.*s\"}}", string_expand(json_escape(scratch, doc->uri))));
            linalloc_clear(&doc->diagnostic_arena);
            *ptr = doc->next;
        }
        else{
            ptr = &doc->next;
        }
    }

    for (Buffer_ID buffer = get_buffer_next(app, 0, Access_Always);
         buffer != 0;
         buffer = get_buffer_next(app, buffer, Access_Always)){
        if (lsp_document_from_buffer(buffer) == 0 && lsp_buffer_is_java(app, buffer)){
            if (lsp_connection == 0){
                if (!lsp_unavailable){
                    Scratch_Block scratch(app);
                    String_Const_u8 path = push_buffer_file_name(app, scratch, buffer);
                    lsp_start_java(app, lsp_java_root(app, scratch, path));
                }
                break;
            }
            if (lsp_initialized){
                lsp_open_document(app, buffer);
            }
        }
    }
}

////////////////////////////////
// NOTE(lsp): Incoming messages

function void
lsp_handle_diagnostics(Application_Links *app, Json_Value *params){
    Scratch_Block scratch(app);
    String_Const_u8 path = lsp_path_from_uri(scratch, json_get(params, "uri")->string);
    Lsp_Document *doc = lsp_document_from_path(path);
    if (doc == 0){
        return;
    }
    linalloc_clear(&doc->diagnostic_arena);
    Json_Value *list = json_get(params, "diagnostics");
    doc->diagnostics = push_array_zero(&doc->diagnostic_arena, Lsp_Diagnostic, list->count);
    doc->diagnostic_count = 0;
    for (Json_Value *item = list->first; item != 0; item = item->next){
        Json_Value *range = json_get(item, "range");
        Json_Value *start = json_get(range, "start");
        Json_Value *end = json_get(range, "end");
        Lsp_Diagnostic *diagnostic = &doc->diagnostics[doc->diagnostic_count++];
        i64 min = lsp_pos_from_position(app, doc->buffer, json_i64(json_get(start, "line")), json_i64(json_get(start, "character")));
        i64 max = lsp_pos_from_position(app, doc->buffer, json_i64(json_get(end, "line")), json_i64(json_get(end, "character")));
        diagnostic->range = Ii64(min, max);
        diagnostic->line = json_i64(json_get(start, "line")) + 1;
        diagnostic->severity = (i32)json_i64(json_get(item, "severity"));
        if (diagnostic->severity == 0){
            diagnostic->severity = 1;
        }
        diagnostic->message = push_string_copy(&doc->diagnostic_arena, json_get(item, "message")->string);
    }
}

// NOTE(lsp): Requests from the server must be answered or jdtls may wait on them forever.
function void
lsp_answer_server_request(Application_Links *app, Json_Value *message){
    Scratch_Block scratch(app);
    String_Const_u8 id = json_get(message, "id")->string;
    Json_Value *id_value = json_get(message, "id");
    if (id_value->kind == JsonKind_String){
        id = push_u8_stringf(scratch, "\"%.*s\"", string_expand(json_escape(scratch, id_value->string)));
    }
    String_Const_u8 method = json_get(message, "method")->string;
    String_Const_u8 result = string_u8_litexpr("null");
    if (string_match(method, string_u8_litexpr("workspace/configuration"))){
        String_Const_u8 settings = lsp_java_settings(app, scratch);
        List_String_Const_u8 items = {};
        Json_Value *list = json_get(json_get(message, "params"), "items");
        for (Json_Value *item = list->first; item != 0; item = item->next){
            String_Const_u8 section = json_get(item, "section")->string;
            string_list_push(scratch, &items, string_match(section, string_u8_litexpr("java"))?settings:string_u8_litexpr("null"));
        }
        result = push_u8_stringf(scratch, "[%.*s]", string_expand(string_list_flatten(scratch, items, 0, string_u8_litexpr(","), 0, StringFill_NoTerminate)));
    }
    else if (string_match(method, string_u8_litexpr("workspace/applyEdit"))){
        result = string_u8_litexpr("{\"applied\":false}");
    }
    lsp_send(app, push_u8_stringf(scratch, "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":%.*s}", string_expand(id), string_expand(result)));
}

function void
lsp_handle_message(Application_Links *app, Lsp_Message *message){
    Scratch_Block scratch(app);
    Json_Value *json = json_parse(scratch, message->body);
    Json_Value *id = json_get(json, "id");
    Json_Value *method = json_get(json, "method");
    b32 keep_message = false;

    if (method->kind == JsonKind_String && id->kind != JsonKind_Null){
        lsp_answer_server_request(app, json);
    }
    else if (method->kind == JsonKind_String){
        Json_Value *params = json_get(json, "params");
        if (string_match(method->string, string_u8_litexpr("textDocument/publishDiagnostics"))){
            lsp_handle_diagnostics(app, params);
        }
        else if (string_match(method->string, string_u8_litexpr("language/status"))){
            String_Const_u8 type = json_get(params, "type")->string;
            if (string_match(type, string_u8_litexpr("ServiceReady"))){
                lsp_set_status(string_u8_litexpr("ready"));
            }
            else if (!string_match(lsp_status, string_u8_litexpr("ready"))){
                lsp_set_status(json_get(params, "message")->string);
            }
        }
        else if (string_match(method->string, string_u8_litexpr("window/showMessage"))){
            print_message(app, push_u8_stringf(scratch, "jdtls: %.*s\n", string_expand(json_get(params, "message")->string)));
        }
    }
    else if (id->kind == JsonKind_Number){
        i64 id_number = json_i64(id);
        if (id_number == lsp_initialize_id){
            lsp_initialized = true;
            lsp_notify(app, "initialized", string_u8_litexpr("{}"));
            lsp_notify(app, "workspace/didChangeConfiguration",
                       push_u8_stringf(scratch, "{\"settings\":{\"java\":%.*s}}", string_expand(lsp_java_settings(app, scratch))));
            lsp_scan_buffers(app);
        }
        for (i32 i = 0; i < ArrayCount(lsp_pending); i += 1){
            if (lsp_pending[i].id == id_number && !lsp_pending[i].done){
                lsp_pending[i].done = true;
                lsp_pending[i].response = message;
                keep_message = true;
                break;
            }
        }
    }

    if (!keep_message){
        base_free(get_base_allocator_system(), message);
    }
}

function void
lsp_update(Application_Links *app){
    if (lsp_connection != 0){
        system_mutex_acquire(lsp_connection->mutex);
        Lsp_Message *first = lsp_connection->first;
        b32 closed = lsp_connection->closed;
        lsp_connection->first = 0;
        lsp_connection->last = 0;
        system_mutex_release(lsp_connection->mutex);

        for (Lsp_Message *message = first; message != 0;){
            Lsp_Message *next = message->next;
            lsp_handle_message(app, message);
            message = next;
        }

        if (closed){
            lsp_reap(lsp_connection);
            lsp_connection = 0;
            lsp_initialized = false;
            lsp_unavailable = true;
            lsp_reset_documents();
            lsp_set_status(string_u8_litexpr("stopped"));
            print_message(app, string_u8_litexpr("lsp: jdtls exited, :LspRestart starts it again\n"));
        }
    }

    u64 now = system_now_time();
    if (now - lsp_last_scan_time > 500000){
        lsp_last_scan_time = now;
        lsp_scan_buffers(app);
    }
    if (lsp_initialized){
        for (Lsp_Document *doc = lsp_first_document; doc != 0; doc = doc->next){
            lsp_sync_document(app, doc);
        }
    }

    if (lsp_hover_text.size > 0){
        View_ID view = get_active_view(app, Access_Always);
        if (view != lsp_hover_view || view_get_cursor_pos(app, view) != lsp_hover_pos){
            lsp_hover_text = string_u8_empty;
        }
    }
}

function void
lsp_tick(Application_Links *app, Frame_Info frame_info){
    default_tick(app, frame_info);
    lsp_update(app);
}

////////////////////////////////
// NOTE(lsp): Requests

// NOTE(lsp): Sends a request and waits for its answer while handling other messages.
// Escape cancels. The result lives in arena; returns 0 on failure.
function Json_Value*
lsp_request(Application_Links *app, Arena *arena, char *method, String_Const_u8 params){
    if (lsp_connection == 0 || !lsp_initialized){
        print_message(app, string_u8_litexpr("lsp: jdtls is not ready yet\n"));
        return(0);
    }
    Lsp_Pending *pending = 0;
    for (i32 i = 0; i < ArrayCount(lsp_pending); i += 1){
        if (lsp_pending[i].id == 0){
            pending = &lsp_pending[i];
            break;
        }
    }
    if (pending == 0){
        return(0);
    }
    pending->id = lsp_send_request(app, method, params);
    pending->done = false;
    pending->response = 0;

    u64 start = system_now_time();
    for (;!pending->done;){
        lsp_update(app);
        if (pending->done || lsp_connection == 0){
            break;
        }
        if (system_now_time() - start > 20000000){
            print_message(app, string_u8_litexpr("lsp: request timed out\n"));
            break;
        }
        // NOTE(lsp): Only a zero delay makes the core send the Animate event that wakes us.
        animate_in_n_milliseconds(app, 0);
        User_Input in = get_next_input(app, EventPropertyGroup_Any, EventProperty_Escape);
        if (in.abort){
            break;
        }
    }

    Json_Value *result = 0;
    if (pending->done && pending->response != 0){
        Json_Value *json = json_parse(arena, push_string_copy(arena, pending->response->body));
        Json_Value *error = json_get(json, "error");
        if (error->kind == JsonKind_Object){
            print_message(app, push_u8_stringf(arena, "lsp: %.*s\n", string_expand(json_get(error, "message")->string)));
        }
        else{
            result = json_get(json, "result");
        }
        base_free(get_base_allocator_system(), pending->response);
    }
    block_zero_struct(pending);
    return(result);
}

struct Lsp_Location{
    String_Const_u8 uri;
    i64 line;
    i64 character;
};

function Lsp_Location
lsp_location_from_json(Json_Value *value){
    Lsp_Location result = {};
    Json_Value *uri = json_get(value, "uri");
    Json_Value *range = json_get(value, "range");
    if (uri->kind != JsonKind_String){
        uri = json_get(value, "targetUri");
        range = json_get(value, "targetSelectionRange");
    }
    Json_Value *start = json_get(range, "start");
    result.uri = uri->string;
    result.line = json_i64(json_get(start, "line"));
    result.character = json_i64(json_get(start, "character"));
    return(result);
}

// NOTE(lsp): Library classes come as jdt:// URIs; jdtls returns their source on request.
function Buffer_ID
lsp_buffer_for_uri(Application_Links *app, View_ID view, String_Const_u8 uri){
    Scratch_Block scratch(app);
    Buffer_ID result = 0;
    if (string_match(string_prefix(uri, 6), string_u8_litexpr("jdt://"))){
        String_Const_u8 name = uri;
        u64 query = string_find_first(name, '?');
        name = string_prefix(name, query);
        name = string_front_of_path(name);
        name = lsp_path_from_uri(scratch, name);
        name = push_u8_stringf(scratch, "*jdt %.*s*", string_expand(name));
        result = get_buffer_by_name(app, name, Access_Always);
        if (result == 0){
            Json_Value *contents = lsp_request(app, scratch, "java/classFileContents",
                                               push_u8_stringf(scratch, "{\"uri\":\"%.*s\"}", string_expand(json_escape(scratch, uri))));
            if (contents != 0 && contents->kind == JsonKind_String){
                result = create_buffer(app, name, BufferCreate_AlwaysNew);
                buffer_replace_range(app, result, Ii64((i64)0), contents->string);
                buffer_set_setting(app, result, BufferSetting_Unimportant, true);
                buffer_set_setting(app, result, BufferSetting_ReadOnly, true);
            }
            else{
                print_message(app, push_u8_stringf(scratch, "lsp: no source for %.*s\n", string_expand(uri)));
            }
        }
        if (result != 0){
            view_set_buffer(app, view, result, 0);
        }
    }
    else{
        String_Const_u8 path = lsp_path_from_uri(scratch, uri);
        if (view_open_file(app, view, path, true)){
            result = view_get_buffer(app, view, Access_Always);
        }
    }
    return(result);
}

function void
lsp_jump_to_location(Application_Links *app, View_ID view, Lsp_Location location){
    point_stack_push_view_cursor(app, view);
    Buffer_ID buffer = lsp_buffer_for_uri(app, view, location.uri);
    if (buffer != 0){
        i64 pos = lsp_pos_from_position(app, buffer, location.line, location.character);
        view_set_cursor_and_preferred_x(app, view, seek_pos(pos));
    }
}

function Lsp_Document*
lsp_active_document(Application_Links *app, View_ID *view_out, Buffer_ID *buffer_out){
    View_ID view = get_active_view(app, Access_ReadVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadVisible);
    Lsp_Document *doc = lsp_document_from_buffer(buffer);
    if (doc != 0){
        lsp_sync_document(app, doc);
    }
    *view_out = view;
    *buffer_out = buffer;
    return(doc);
}

function String_Const_u8
lsp_location_line_text(Application_Links *app, Arena *arena, String_Const_u8 path, i64 line){
    String_Const_u8 result = {};
    Buffer_ID buffer = get_buffer_by_file_name(app, path, Access_Always);
    if (buffer != 0){
        result = push_buffer_line(app, arena, buffer, line + 1);
    }
    else{
        String_Const_u8 contents = dump_file(arena, path).data;
        List_String_Const_u8 lines = string_split(arena, contents, (u8*)"\n", 1);
        i64 i = 0;
        for (Node_String_Const_u8 *node = lines.first; node != 0; node = node->next, i += 1){
            if (i == line){
                result = node->string;
                break;
            }
        }
    }
    return(string_skip_chop_whitespace(result));
}

// NOTE(lsp): Shows a list of locations in an fzf style lister and jumps to the choice.
function void
lsp_choose_location(Application_Links *app, View_ID view, Lsp_Location *locations, i32 count, String_Const_u8 query){
    Scratch_Block scratch(app);
    String_Const_u8 root = (lsp_root_count > 0)?lsp_roots[0]:string_u8_empty;
    Lister_Block lister(app, scratch);
    lister_set_query(lister, query);
    lister_set_default_handlers(lister);
    ((Lister*)lister)->fuzzy_filter = true;
    for (i32 i = 0; i < count; i += 1){
        String_Const_u8 path = lsp_path_from_uri(scratch, locations[i].uri);
        String_Const_u8 text = lsp_location_line_text(app, scratch, path, locations[i].line);
        String_Const_u8 item = push_u8_stringf(scratch, "%.*s:%lld: %.*s", string_expand(fuzzy_relative_path(root, path)),
                                               locations[i].line + 1, string_expand(text));
        lister_add_item(lister, item, SCu8(), IntAsPtr(i), 0);
    }
    Lister_Result l_result = run_lister(app, lister);
    if (!l_result.canceled){
        lsp_jump_to_location(app, view, locations[PtrAsInt(l_result.user_data)]);
    }
}

// NOTE(lsp): Plain text for the popup: drops ** emphasis and keeps only the text of
// [text](url) links.
function String_Const_u8
lsp_strip_markdown(Arena *arena, String_Const_u8 line){
    u8 *out = push_array(arena, u8, line.size + 1);
    u64 size = 0;
    for (u64 i = 0; i < line.size; i += 1){
        u8 c = line.str[i];
        if (c == '*' && i + 1 < line.size && line.str[i + 1] == '*'){
            i += 1;
            continue;
        }
        if (c == ']' && i + 1 < line.size && line.str[i + 1] == '('){
            u64 close = string_find_first(string_skip(line, i), ')');
            if (i + close < line.size){
                i += close;
                continue;
            }
        }
        if (c == '['){
            continue;
        }
        out[size++] = c;
    }
    out[size] = 0;
    return(SCu8(out, size));
}

function void
lsp_show_popup(Application_Links *app, View_ID view, String_Const_u8 text){
    if (lsp_hover_arena.base_allocator == 0){
        lsp_hover_arena = make_arena_system();
    }
    linalloc_clear(&lsp_hover_arena);

    // NOTE(lsp): Drop markdown code fences and runs of blank lines.
    List_String_Const_u8 lines = string_split(&lsp_hover_arena, text, (u8*)"\n", 1);
    List_String_Const_u8 kept = {};
    b32 prev_blank = true;
    for (Node_String_Const_u8 *node = lines.first; node != 0; node = node->next){
        String_Const_u8 line = string_chop_whitespace(node->string);
        if (string_match(string_prefix(line, 3), string_u8_litexpr("```"))){
            continue;
        }
        line = lsp_strip_markdown(&lsp_hover_arena, line);
        b32 blank = (line.size == 0);
        if (!(blank && prev_blank)){
            string_list_push(&lsp_hover_arena, &kept, line);
        }
        prev_blank = blank;
    }
    lsp_hover_text = string_list_flatten(&lsp_hover_arena, kept, 0, string_u8_litexpr("\n"), 0, StringFill_NoTerminate);
    lsp_hover_text = string_chop_whitespace(lsp_hover_text);
    lsp_hover_view = view;
    lsp_hover_pos = view_get_cursor_pos(app, view);
}

function void
lsp_close_popup(void){
    lsp_hover_text = string_u8_empty;
}

function String_Const_u8
lsp_hover_string(Arena *arena, Json_Value *contents){
    String_Const_u8 result = {};
    if (contents->kind == JsonKind_String){
        result = contents->string;
    }
    else if (contents->kind == JsonKind_Object){
        result = json_get(contents, "value")->string;
    }
    else if (contents->kind == JsonKind_Array){
        List_String_Const_u8 parts = {};
        for (Json_Value *item = contents->first; item != 0; item = item->next){
            string_list_push(arena, &parts, lsp_hover_string(arena, item));
        }
        result = string_list_flatten(arena, parts, 0, string_u8_litexpr("\n\n"), 0, StringFill_NoTerminate);
    }
    return(result);
}

////////////////////////////////
// NOTE(lsp): Commands

CUSTOM_UI_COMMAND_SIG(lsp_goto_definition)
CUSTOM_DOC("Jumps to the definition of the symbol under the cursor using the language server, or the code index outside Java files.")
{
    View_ID view = 0;
    Buffer_ID buffer = 0;
    Lsp_Document *doc = lsp_active_document(app, &view, &buffer);
    if (doc == 0){
        jump_to_definition_at_cursor(app);
        return;
    }
    Scratch_Block scratch(app);
    String_Const_u8 params = lsp_text_document_position(app, scratch, buffer, doc->uri, view_get_cursor_pos(app, view));
    Json_Value *result = lsp_request(app, scratch, "textDocument/definition", params);
    if (result == 0){
        return;
    }
    if (result->kind == JsonKind_Object){
        lsp_jump_to_location(app, view, lsp_location_from_json(result));
    }
    else if (result->kind == JsonKind_Array && result->count == 1){
        lsp_jump_to_location(app, view, lsp_location_from_json(result->first));
    }
    else if (result->kind == JsonKind_Array && result->count > 1){
        Lsp_Location *locations = push_array(scratch, Lsp_Location, result->count);
        i32 count = 0;
        for (Json_Value *item = result->first; item != 0; item = item->next){
            locations[count++] = lsp_location_from_json(item);
        }
        lsp_choose_location(app, view, locations, count, string_u8_litexpr("Definitions:"));
    }
    else{
        print_message(app, string_u8_litexpr("lsp: no definition found\n"));
    }
}

CUSTOM_UI_COMMAND_SIG(lsp_find_references)
CUSTOM_DOC("Lists the references to the symbol under the cursor using the language server, or all locations of the identifier outside Java files.")
{
    View_ID view = 0;
    Buffer_ID buffer = 0;
    Lsp_Document *doc = lsp_active_document(app, &view, &buffer);
    if (doc == 0){
        list_all_locations_of_identifier(app);
        return;
    }
    Scratch_Block scratch(app);
    String_Const_u8 position = lsp_text_document_position(app, scratch, buffer, doc->uri, view_get_cursor_pos(app, view));
    String_Const_u8 params = push_u8_stringf(scratch, "%.*s,\"context\":{\"includeDeclaration\":true}}",
                                             (i32)(position.size - 1), position.str);
    Json_Value *result = lsp_request(app, scratch, "textDocument/references", params);
    if (result == 0 || result->kind != JsonKind_Array || result->count == 0){
        print_message(app, string_u8_litexpr("lsp: no references found\n"));
        return;
    }
    Lsp_Location *locations = push_array(scratch, Lsp_Location, result->count);
    i32 count = 0;
    for (Json_Value *item = result->first; item != 0; item = item->next){
        locations[count++] = lsp_location_from_json(item);
    }
    lsp_choose_location(app, view, locations, count, push_u8_stringf(scratch, "References (%d):", count));
}

CUSTOM_UI_COMMAND_SIG(lsp_hover)
CUSTOM_DOC("Shows the language server's documentation for the symbol under the cursor in a popup.")
{
    View_ID view = 0;
    Buffer_ID buffer = 0;
    Lsp_Document *doc = lsp_active_document(app, &view, &buffer);
    if (doc == 0){
        return;
    }
    Scratch_Block scratch(app);
    String_Const_u8 params = lsp_text_document_position(app, scratch, buffer, doc->uri, view_get_cursor_pos(app, view));
    Json_Value *result = lsp_request(app, scratch, "textDocument/hover", params);
    if (result != 0 && result->kind == JsonKind_Object){
        String_Const_u8 text = lsp_hover_string(scratch, json_get(result, "contents"));
        if (text.size > 0){
            lsp_show_popup(app, view, text);
        }
    }
}

function void
lsp_goto_diagnostic(Application_Links *app, b32 forward){
    View_ID view = 0;
    Buffer_ID buffer = 0;
    Lsp_Document *doc = lsp_active_document(app, &view, &buffer);
    if (doc == 0 || doc->diagnostic_count == 0){
        print_message(app, string_u8_litexpr("lsp: no diagnostics\n"));
        return;
    }
    i64 pos = view_get_cursor_pos(app, view);
    Lsp_Diagnostic *best = 0;
    Lsp_Diagnostic *wrap = 0;
    for (i32 i = 0; i < doc->diagnostic_count; i += 1){
        Lsp_Diagnostic *d = &doc->diagnostics[i];
        i64 start = d->range.min;
        if (forward){
            if (start > pos && (best == 0 || start < best->range.min)){
                best = d;
            }
            if (wrap == 0 || start < wrap->range.min){
                wrap = d;
            }
        }
        else{
            if (start < pos && (best == 0 || start > best->range.min)){
                best = d;
            }
            if (wrap == 0 || start > wrap->range.min){
                wrap = d;
            }
        }
    }
    if (best == 0){
        best = wrap;
    }
    view_set_cursor_and_preferred_x(app, view, seek_pos(best->range.min));
    lsp_show_popup(app, view, best->message);
}

CUSTOM_COMMAND_SIG(lsp_next_diagnostic)
CUSTOM_DOC("Moves to the next error or warning reported by the language server and shows its message.")
{
    lsp_goto_diagnostic(app, true);
}

CUSTOM_COMMAND_SIG(lsp_prev_diagnostic)
CUSTOM_DOC("Moves to the previous error or warning reported by the language server and shows its message.")
{
    lsp_goto_diagnostic(app, false);
}

CUSTOM_UI_COMMAND_SIG(lsp_list_diagnostics)
CUSTOM_DOC("Lists the errors and warnings of all open Java files in an fzf style lister.")
{
    Scratch_Block scratch(app);
    View_ID view = get_active_view(app, Access_Always);
    String_Const_u8 root = (lsp_root_count > 0)?lsp_roots[0]:string_u8_empty;
    Lister_Block lister(app, scratch);
    lister_set_query(lister, "Diagnostics:");
    lister_set_default_handlers(lister);
    ((Lister*)lister)->fuzzy_filter = true;
    for (Lsp_Document *doc = lsp_first_document; doc != 0; doc = doc->next){
        for (i32 i = 0; i < doc->diagnostic_count; i += 1){
            Lsp_Diagnostic *d = &doc->diagnostics[i];
            String_Const_u8 kind = string_u8_litexpr("info");
            if (d->severity == 1){
                kind = string_u8_litexpr("error");
            }
            else if (d->severity == 2){
                kind = string_u8_litexpr("warning");
            }
            String_Const_u8 item = push_u8_stringf(scratch, "%.*s:%lld: %.*s: %.*s", string_expand(fuzzy_relative_path(root, doc->path)),
                                                   d->line, string_expand(kind), string_expand(d->message));
            lister_add_item(lister, item, SCu8(), d, 0);
        }
    }
    Lister_Result l_result = run_lister(app, lister);
    if (!l_result.canceled){
        Lsp_Diagnostic *d = (Lsp_Diagnostic*)l_result.user_data;
        for (Lsp_Document *doc = lsp_first_document; doc != 0; doc = doc->next){
            if (doc->diagnostics <= d && d < doc->diagnostics + doc->diagnostic_count){
                point_stack_push_view_cursor(app, view);
                view_set_buffer(app, view, doc->buffer, 0);
                view_set_cursor_and_preferred_x(app, view, seek_pos(d->range.min));
                break;
            }
        }
    }
}

CUSTOM_COMMAND_SIG(lsp_restart)
CUSTOM_DOC("Stops the Java language server and starts it again for the open Java files.")
{
    if (lsp_connection != 0){
        lsp_kill(lsp_connection);
    }
    lsp_connection = 0;
    lsp_initialized = false;
    lsp_unavailable = false;
    lsp_reset_documents();
    lsp_set_status(string_u8_empty);
    lsp_scan_buffers(app);
}

////////////////////////////////
// NOTE(lsp): Drawing

// NOTE(lsp): Themes without the lsp colors get the color table's placeholder; use defaults.
function ARGB_Color
lsp_theme_color(Managed_ID id, ARGB_Color fallback){
    ARGB_Color result = fcolor_resolve(fcolor_id(id));
    if (result == 0 || result == 0xFF990099){
        result = fallback;
    }
    return(result);
}

function ARGB_Color
lsp_severity_color(i32 severity){
    ARGB_Color result = 0;
    switch (severity){
        case 1: {result = lsp_theme_color(defcolor_lsp_error, 0xFFF75464);}break;
        case 2: {result = lsp_theme_color(defcolor_lsp_warning, 0xFFF2C55C);}break;
        default: {result = lsp_theme_color(defcolor_lsp_info, 0xFF96A6C8);}break;
    }
    return(result);
}

// NOTE(lsp): Underlines each diagnostic and shows the most severe message of each line
// after its end, like Neovim's virtual text.
function void
lsp_draw_diagnostics(Application_Links *app, Buffer_ID buffer, Text_Layout_ID text_layout_id, Face_ID face_id){
    Lsp_Document *doc = lsp_document_from_buffer(buffer);
    if (doc == 0 || doc->diagnostic_count == 0){
        return;
    }
    Scratch_Block scratch(app);
    Range_i64 visible_range = text_layout_get_visible_range(app, text_layout_id);
    Face_Metrics metrics = get_face_metrics(app, face_id);

    i64 first_line = get_line_number_from_pos(app, buffer, visible_range.min);
    i64 last_line = get_line_number_from_pos(app, buffer, visible_range.max);
    i64 line_span = last_line - first_line + 1;
    Lsp_Diagnostic **per_line = push_array_zero(scratch, Lsp_Diagnostic*, line_span);

    for (i32 i = 0; i < doc->diagnostic_count; i += 1){
        Lsp_Diagnostic *d = &doc->diagnostics[i];
        Range_i64 range = d->range;
        if (range.max <= range.min){
            range.max = range.min + 1;
        }
        if (range.max < visible_range.min || range.min > visible_range.max){
            continue;
        }
        ARGB_Color color = lsp_severity_color(d->severity);
        Range_i64 drawn = Ii64(clamp_bot(visible_range.min, range.min), clamp_top(range.max, visible_range.max));
        for (i64 pos = drawn.min; pos < drawn.max && pos < drawn.min + 2000; pos += 1){
            Rect_f32 rect = text_layout_character_on_screen(app, text_layout_id, pos);
            if (rect.x1 > rect.x0){
                draw_rectangle(app, Rf32(rect.x0, rect.y1 - 2.f, rect.x1, rect.y1), 0.f, color);
            }
        }
        i64 line = get_line_number_from_pos(app, buffer, range.min);
        if (first_line <= line && line <= last_line){
            Lsp_Diagnostic **slot = &per_line[line - first_line];
            if (*slot == 0 || d->severity < (*slot)->severity){
                *slot = d;
            }
        }
    }

    for (i64 i = 0; i < line_span; i += 1){
        Lsp_Diagnostic *d = per_line[i];
        if (d != 0){
            i64 end = get_line_end_pos(app, buffer, first_line + i);
            Rect_f32 rect = text_layout_character_on_screen(app, text_layout_id, end);
            String_Const_u8 message = d->message;
            message = string_prefix(message, string_find_first(message, '\n'));
            message = push_u8_stringf(scratch, "  ■ %.*s", string_expand(message));
            draw_string(app, face_id, message, V2f32(rect.x0 + metrics.normal_advance, rect.y0), lsp_severity_color(d->severity));
        }
    }
}

function void
lsp_draw_popup(Application_Links *app, View_ID view_id, Text_Layout_ID text_layout_id, Face_ID face_id, Rect_f32 region){
    if (lsp_hover_text.size == 0 || view_id != lsp_hover_view){
        return;
    }
    Scratch_Block scratch(app);
    Face_Metrics metrics = get_face_metrics(app, face_id);
    Rect_f32 cursor = text_layout_character_on_screen(app, text_layout_id, lsp_hover_pos);

    List_String_Const_u8 lines = string_split(scratch, lsp_hover_text, (u8*)"\n", 1);
    i32 max_lines = 20;
    i32 max_chars = 100;
    f32 width = 0.f;
    i32 line_count = 0;
    for (Node_String_Const_u8 *node = lines.first; node != 0 && line_count < max_lines; node = node->next, line_count += 1){
        node->string = string_prefix(node->string, max_chars);
        width = Max(width, get_string_advance(app, face_id, node->string));
    }
    f32 pad = metrics.normal_advance;
    f32 height = line_count*metrics.line_height + pad;
    Rect_f32 box = Rf32(cursor.x0, cursor.y1 + 2.f, cursor.x0 + width + 2.f*pad, cursor.y1 + 2.f + height);
    if (box.y1 > region.y1){
        box.y1 = cursor.y0 - 2.f;
        box.y0 = box.y1 - height;
    }
    if (box.x1 > region.x1){
        f32 shift = clamp_top(box.x1 - region.x1, box.x0 - region.x0);
        box.x0 -= shift;
        box.x1 -= shift;
    }

    ARGB_Color back = lsp_theme_color(defcolor_lsp_popup_back, fcolor_resolve(fcolor_id(defcolor_margin)));
    draw_rectangle(app, box, 4.f, back);
    draw_rectangle_outline(app, box, 4.f, 1.f, fcolor_resolve(fcolor_id(defcolor_margin_active)));
    Vec2_f32 p = V2f32(box.x0 + pad, box.y0 + pad*0.5f);
    i32 drawn = 0;
    for (Node_String_Const_u8 *node = lines.first; node != 0 && drawn < max_lines; node = node->next, drawn += 1){
        draw_string(app, face_id, node->string, p, fcolor_resolve(fcolor_id(defcolor_text_default)));
        p.y += metrics.line_height;
    }
}

function String_Const_u8
lsp_status_label(Application_Links *app, Arena *arena, Buffer_ID buffer){
    String_Const_u8 result = {};
    if (lsp_connection != 0 && lsp_buffer_is_java(app, buffer)){
        Lsp_Document *doc = lsp_document_from_buffer(buffer);
        i32 errors = 0;
        i32 warnings = 0;
        for (i32 i = 0; doc != 0 && i < doc->diagnostic_count; i += 1){
            if (doc->diagnostics[i].severity == 1){
                errors += 1;
            }
            else if (doc->diagnostics[i].severity == 2){
                warnings += 1;
            }
        }
        String_Const_u8 status = string_prefix(lsp_status, 40);
        if (errors > 0 || warnings > 0){
            result = push_u8_stringf(arena, " jdtls %.*s E%d W%d ", string_expand(status), errors, warnings);
        }
        else{
            result = push_u8_stringf(arena, " jdtls %.*s ", string_expand(status));
        }
    }
    return(result);
}

////////////////////////////////

function void
lsp_init(Application_Links *app){
    lsp_arena = make_arena_system();
    set_custom_hook(app, HookID_Tick, lsp_tick);
}

#endif //FCODER_LSP_CPP

// BOTTOM
