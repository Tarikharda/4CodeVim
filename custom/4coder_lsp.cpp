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
    String_Const_u8 raw;
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
    u64 start_pos = parser->pos;
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
    value->raw = SCu8(parser->text.str + start_pos, clamp_bot(start_pos, parser->pos) - start_pos);
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
        // NOTE(lsp): Its own process group, so closing 4coder can stop java and its wrappers at once.
        setpgid(0, 0);
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
    kill(-(pid_t)connection->pid, SIGTERM);
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
    String_Const_u8 raw;
};

struct Lsp_Document{
    Lsp_Document *next;
    Buffer_ID buffer;
    String_Const_u8 path;
    String_Const_u8 uri;
    i32 version;
    History_Record_Index synced_history;
    i64 synced_size;
    String_Const_u8 synced_text;
    u64 synced_cap;
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
global b32 lsp_exit_handler_installed = false;

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

// NOTE(lsp): Runs when 4coder exits; stops this session's jdtls (and its java) right away
// instead of leaving it to lsp-watchdog.
function void
lsp_on_exit(void){
    if (lsp_connection != 0){
        lsp_kill(lsp_connection);
    }
}

#if OS_LINUX || OS_MAC
// NOTE(lsp): Being killed or logged out skips atexit, so stop jdtls's process group first
// and then die the way the signal would have.
function void
lsp_on_signal(i32 signal_number){
    if (lsp_connection != 0){
        kill(-(pid_t)lsp_connection->pid, SIGTERM);
    }
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}
#endif

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
    // NOTE(lsp): jdtls keeps signature help off unless the client turns it on.
    string_list_push(arena, &parts, string_u8_litexpr("\"signatureHelp\":{\"enabled\":true,\"description\":{\"enabled\":true}}"));
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

    char *argv[32] = {};
    i32 argc = 0;
    if (lsp_is_executable(scratch, watchdog)){
        argv[argc++] = (char*)push_string_copy(scratch, watchdog).str;
    }
    argv[argc++] = (char*)push_string_copy(scratch, jdtls).str;
    if (file_exists(app, lombok)){
        argv[argc++] = (char*)push_u8_stringf(scratch, "--jvm-arg=-javaagent:%.*s", string_expand(lombok)).str;
    }
    // NOTE(lsp): The JVM flags VS Code's Java extension starts jdtls with, tuned for startup
    // and a responsive heap rather than throughput.
    argv[argc++] = "--jvm-arg=-XX:+UseParallelGC";
    argv[argc++] = "--jvm-arg=-XX:GCTimeRatio=4";
    argv[argc++] = "--jvm-arg=-XX:AdaptiveSizePolicyWeight=90";
    argv[argc++] = "--jvm-arg=-Dsun.zip.disableMemoryMapping=true";
    argv[argc++] = "--jvm-arg=-Xms100m";
    argv[argc++] = "--jvm-arg=-Xmx2G";
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
    if (!lsp_exit_handler_installed){
        lsp_exit_handler_installed = true;
        atexit(lsp_on_exit);
#if OS_LINUX || OS_MAC
        signal(SIGTERM, lsp_on_signal);
        signal(SIGINT, lsp_on_signal);
        signal(SIGHUP, lsp_on_signal);
#endif
    }
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
                                             "\"workspace\":{\"workspaceFolders\":true,\"configuration\":true,\"applyEdit\":true,"
                                             "\"workspaceEdit\":{\"documentChanges\":true},\"symbol\":{}},"
                                             "\"textDocument\":{"
                                             "\"synchronization\":{\"didSave\":false},"
                                             "\"completion\":{\"contextSupport\":true,\"completionItem\":{\"snippetSupport\":true,"
                                             "\"resolveSupport\":{\"properties\":[\"documentation\",\"detail\",\"additionalTextEdits\"]}}},"
                                             "\"signatureHelp\":{\"signatureInformation\":{\"parameterInformation\":{\"labelOffsetSupport\":true}}},"
                                             "\"hover\":{\"contentFormat\":[\"markdown\",\"plaintext\"]},"
                                             "\"definition\":{\"linkSupport\":true},"
                                             "\"declaration\":{\"linkSupport\":true},"
                                             "\"implementation\":{\"linkSupport\":true},"
                                             "\"typeDefinition\":{\"linkSupport\":true},"
                                             "\"references\":{},"
                                             "\"rename\":{},"
                                             "\"formatting\":{},"
                                             "\"documentSymbol\":{\"hierarchicalDocumentSymbolSupport\":true},"
                                             "\"codeAction\":{\"codeActionLiteralSupport\":{\"codeActionKind\":{\"valueSet\":"
                                             "[\"quickfix\",\"refactor\",\"refactor.extract\",\"refactor.inline\",\"refactor.rewrite\","
                                             "\"source\",\"source.organizeImports\"]}}},"
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

function void lsp_document_store_text(Lsp_Document *doc, String_Const_u8 text);

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
    lsp_document_store_text(doc, text);
    lsp_notify(app, "textDocument/didOpen",
               push_u8_stringf(scratch, "{\"textDocument\":{\"uri\":\"%.*s\",\"languageId\":\"java\",\"version\":1,\"text\":\"%.*s\"}}",
                               string_expand(json_escape(scratch, doc->uri)), string_expand(json_escape(scratch, text))));
}

function void
lsp_document_store_text(Lsp_Document *doc, String_Const_u8 text){
    if (text.size > doc->synced_cap){
        Base_Allocator *allocator = get_base_allocator_system();
        if (doc->synced_text.str != 0){
            base_free(allocator, doc->synced_text.str);
        }
        String_Const_u8 memory = base_allocate(allocator, clamp_bot(KB(4), text.size*2));
        doc->synced_text.str = memory.str;
        doc->synced_cap = memory.size;
    }
    block_copy(doc->synced_text.str, text.str, text.size);
    doc->synced_text.size = text.size;
}

// NOTE(lsp): Line and UTF-16 column of a byte offset in a copy of the text.
function void
lsp_position_in_text(String_Const_u8 text, u64 offset, i64 *line_out, i64 *character_out){
    i64 line = 0;
    u64 line_start = 0;
    for (u64 i = 0; i < offset; i += 1){
        if (text.str[i] == '\n'){
            line += 1;
            line_start = i + 1;
        }
    }
    i64 units = 0;
    for (u64 i = line_start; i < offset;){
        Character_Consume_Result consume = utf8_consume(text.str + i, offset - i);
        units += (consume.codepoint >= 0x10000)?2:1;
        i += clamp_bot(1, consume.inc);
    }
    *line_out = line;
    *character_out = units;
}

// NOTE(lsp): Sends only the region between the common prefix and suffix of the old and
// new text, so typing in a large file costs a few bytes instead of the whole file.
function void
lsp_sync_document(Application_Links *app, Lsp_Document *doc){
    History_Record_Index history = buffer_history_get_current_state_index(app, doc->buffer);
    i64 size = buffer_get_size(app, doc->buffer);
    if (history != doc->synced_history || size != doc->synced_size){
        Scratch_Block scratch(app);
        doc->synced_history = history;
        doc->synced_size = size;
        String_Const_u8 text = push_whole_buffer(app, scratch, doc->buffer);
        String_Const_u8 old = doc->synced_text;
        u64 prefix = 0;
        u64 max_prefix = Min(old.size, text.size);
        for (;prefix < max_prefix && old.str[prefix] == text.str[prefix]; prefix += 1);
        // NOTE(lsp): Keep the split on a UTF-8 character boundary.
        for (;prefix > 0 && (old.str[prefix] & 0xC0) == 0x80; prefix -= 1);
        u64 suffix = 0;
        for (;suffix < old.size - prefix && suffix < text.size - prefix &&
             old.str[old.size - 1 - suffix] == text.str[text.size - 1 - suffix]; suffix += 1);
        for (;suffix > 0 && (old.str[old.size - suffix] & 0xC0) == 0x80; suffix -= 1);
        if (prefix == old.size && prefix == text.size){
            return;
        }
        
        i64 start_line = 0;
        i64 start_character = 0;
        i64 end_line = 0;
        i64 end_character = 0;
        lsp_position_in_text(old, prefix, &start_line, &start_character);
        lsp_position_in_text(old, old.size - suffix, &end_line, &end_character);
        String_Const_u8 inserted = SCu8(text.str + prefix, text.size - suffix - prefix);
        
        doc->version += 1;
        lsp_notify(app, "textDocument/didChange",
                   push_u8_stringf(scratch, "{\"textDocument\":{\"uri\":\"%.*s\",\"version\":%d},\"contentChanges\":[{"
                                   "\"range\":{\"start\":{\"line\":%lld,\"character\":%lld},\"end\":{\"line\":%lld,\"character\":%lld}},"
                                   "\"text\":\"%.*s\"}]}",
                                   string_expand(json_escape(scratch, doc->uri)), doc->version,
                                   start_line, start_character, end_line, end_character,
                                   string_expand(json_escape(scratch, inserted))));
        lsp_document_store_text(doc, text);
    }
}

function void
lsp_reset_documents(void){
    for (Lsp_Document *doc = lsp_first_document; doc != 0; doc = doc->next){
        linalloc_clear(&doc->diagnostic_arena);
        if (doc->synced_text.str != 0){
            base_free(get_base_allocator_system(), doc->synced_text.str);
        }
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
            if (doc->synced_text.str != 0){
                base_free(get_base_allocator_system(), doc->synced_text.str);
            }
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
        diagnostic->raw = push_string_copy(&doc->diagnostic_arena, item->raw);
    }
}

function i32 lsp_apply_workspace_edit(Application_Links *app, Json_Value *edit);
function b32 lsp_handle_async_response(Application_Links *app, i64 id, Json_Value *json);

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
        lsp_apply_workspace_edit(app, json_get(json_get(message, "params"), "edit"));
        result = string_u8_litexpr("{\"applied\":true}");
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
        if (lsp_handle_async_response(app, id_number, json)){
            id_number = 0;
        }
        for (i32 i = 0; id_number != 0 && i < ArrayCount(lsp_pending); i += 1){
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

function void lsp_update_popups(Application_Links *app, View_ID active_view);

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

    View_ID active_view = get_active_view(app, Access_Always);
    if (lsp_hover_text.size > 0){
        if (active_view != lsp_hover_view || view_get_cursor_pos(app, active_view) != lsp_hover_pos){
            lsp_hover_text = string_u8_empty;
        }
    }
    lsp_update_popups(app, active_view);
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
// NOTE(lsp): Text edits

struct Lsp_Edit{
    Range_i64 range;
    String_Const_u8 text;
};

function Range_i64
lsp_range_from_json(Application_Links *app, Buffer_ID buffer, Json_Value *range){
    Json_Value *start = json_get(range, "start");
    Json_Value *end = json_get(range, "end");
    return(Ii64(lsp_pos_from_position(app, buffer, json_i64(json_get(start, "line")), json_i64(json_get(start, "character"))),
                lsp_pos_from_position(app, buffer, json_i64(json_get(end, "line")), json_i64(json_get(end, "character")))));
}

function i32
lsp_collect_edits(Application_Links *app, Buffer_ID buffer, Json_Value *edits, Lsp_Edit *out, i32 max){
    i32 count = 0;
    for (Json_Value *edit = edits->first; edit != 0 && count < max; edit = edit->next){
        out[count].range = lsp_range_from_json(app, buffer, json_get(edit, "range"));
        out[count].text = json_get(edit, "newText")->string;
        count += 1;
    }
    return(count);
}

// NOTE(lsp): Applies edits given in positions of the unedited buffer as one batch, which is
// also a single undo step.
function void
lsp_apply_edits(Application_Links *app, Buffer_ID buffer, Lsp_Edit *edits, i32 count){
    Scratch_Block scratch(app);
    for (i32 i = 1; i < count; i += 1){
        Lsp_Edit edit = edits[i];
        i32 j = i;
        for (;j > 0 && edits[j - 1].range.min > edit.range.min; j -= 1){
            edits[j] = edits[j - 1];
        }
        edits[j] = edit;
    }
    Batch_Edit *first = 0;
    Batch_Edit *last = 0;
    for (i32 i = 0; i < count; i += 1){
        Batch_Edit *batch = push_array_zero(scratch, Batch_Edit, 1);
        batch->edit.text = edits[i].text;
        batch->edit.range = edits[i].range;
        sll_queue_push(first, last, batch);
    }
    if (first != 0){
        buffer_batch_edit(app, buffer, first);
    }
}

function i32
lsp_apply_uri_edits(Application_Links *app, String_Const_u8 uri, Json_Value *edits){
    Scratch_Block scratch(app);
    String_Const_u8 path = lsp_path_from_uri(scratch, uri);
    Buffer_ID buffer = get_buffer_by_file_name(app, path, Access_Always);
    if (buffer == 0){
        buffer = create_buffer(app, path, BufferCreate_NeverNew);
    }
    i32 count = 0;
    if (buffer != 0 && edits->count > 0){
        Lsp_Edit *list = push_array(scratch, Lsp_Edit, edits->count);
        count = lsp_collect_edits(app, buffer, edits, list, edits->count);
        lsp_apply_edits(app, buffer, list, count);
    }
    return(count);
}

function i32
lsp_apply_workspace_edit(Application_Links *app, Json_Value *edit){
    i32 total = 0;
    Json_Value *changes = json_get(edit, "changes");
    for (Json_Value *file = changes->first; file != 0; file = file->next){
        total += lsp_apply_uri_edits(app, file->key, file);
    }
    Json_Value *document_changes = json_get(edit, "documentChanges");
    for (Json_Value *change = document_changes->first; change != 0; change = change->next){
        Json_Value *text_document = json_get(change, "textDocument");
        if (text_document->kind == JsonKind_Object){
            total += lsp_apply_uri_edits(app, json_get(text_document, "uri")->string, json_get(change, "edits"));
        }
        else{
            print_message(app, string_u8_litexpr("lsp: creating, renaming or deleting files from an edit is not supported\n"));
        }
    }
    return(total);
}

////////////////////////////////
// NOTE(lsp): Kinds

global char *lsp_kind_names[] = {
    "", "text", "method", "function", "ctor", "field", "var", "class", "iface", "module",
    "prop", "unit", "value", "enum", "keyword", "snippet", "color", "file", "ref", "folder",
    "member", "const", "struct", "event", "op", "type",
};

global char *lsp_symbol_kind_names[] = {
    "", "file", "module", "namespace", "package", "class", "method", "property", "field",
    "ctor", "enum", "iface", "function", "var", "const", "string", "number", "bool",
    "array", "object", "key", "null", "member", "struct", "event", "op", "type",
};

function char*
lsp_kind_name(char **names, i32 count, i64 kind){
    char *result = names[0];
    if (0 <= kind && kind < count){
        result = names[kind];
    }
    return(result);
}

function b32
lsp_is_identifier_char(u8 c){
    return(character_is_alpha_numeric(c) || c == '_' || c == '$' || c >= 128);
}

////////////////////////////////
// NOTE(lsp): Snippets

// NOTE(lsp): After accepting a snippet the cursor sits on its first placeholder. Typing
// replaces the placeholder text and Tab moves to the next one. The stops are buffer
// markers, so they follow the edits made while filling them in.

struct Lsp_Snippet_Stop{
    i32 number;
    u64 start;
    u64 end;
};

global Managed_Object lsp_snippet_markers = 0;
global Buffer_ID lsp_snippet_buffer = 0;
global i32 lsp_snippet_count = 0;
global i32 lsp_snippet_index = 0;
global b32 lsp_snippet_pending_replace = false;

function u64
lsp_snippet_copy_text(u8 *out, u64 size, String_Const_u8 text){
    for (u64 i = 0; i < text.size; i += 1){
        u8 c = text.str[i];
        if (c == '\\' && i + 1 < text.size){
            out[size++] = text.str[i + 1];
            i += 1;
        }
        else if (c == '$' && i + 1 < text.size && text.str[i + 1] == '{'){
            // NOTE(lsp): Nested placeholders keep only their text.
            u64 j = i + 2;
            for (;j < text.size && character_is_base10(text.str[j]); j += 1);
            if (j < text.size && (text.str[j] == ':' || text.str[j] == '|')){
                j += 1;
            }
            i32 depth = 1;
            u64 inner_start = j;
            for (;j < text.size; j += 1){
                if (text.str[j] == '{'){
                    depth += 1;
                }
                else if (text.str[j] == '}'){
                    depth -= 1;
                    if (depth == 0){
                        break;
                    }
                }
            }
            size = lsp_snippet_copy_text(out, size, SCu8(text.str + inner_start, j - inner_start));
            i = j;
        }
        else if (c == '$' && i + 1 < text.size && character_is_base10(text.str[i + 1])){
            for (;i + 1 < text.size && character_is_base10(text.str[i + 1]); i += 1);
        }
        else{
            out[size++] = c;
        }
    }
    return(size);
}

// NOTE(lsp): Expands ${1:text}, ${1|a,b|}, $1 and $0 into plain text plus stop ranges.
function String_Const_u8
lsp_snippet_expand(Arena *arena, String_Const_u8 snippet, Lsp_Snippet_Stop *stops, i32 *stop_count, i32 max_stops){
    u8 *out = push_array(arena, u8, snippet.size + 1);
    u64 size = 0;
    i32 count = 0;
    for (u64 i = 0; i < snippet.size; i += 1){
        u8 c = snippet.str[i];
        if (c == '\\' && i + 1 < snippet.size){
            out[size++] = snippet.str[i + 1];
            i += 1;
        }
        else if (c == '$' && i + 1 < snippet.size && (snippet.str[i + 1] == '{' || character_is_base10(snippet.str[i + 1]))){
            b32 braced = (snippet.str[i + 1] == '{');
            u64 j = i + (braced?2:1);
            u64 number_start = j;
            for (;j < snippet.size && character_is_base10(snippet.str[j]); j += 1);
            i32 number = (i32)string_to_integer(SCu8(snippet.str + number_start, j - number_start), 10);
            u64 stop_start = size;
            if (braced){
                b32 choice = (j < snippet.size && snippet.str[j] == '|');
                if (j < snippet.size && (snippet.str[j] == ':' || choice)){
                    j += 1;
                }
                i32 depth = 1;
                u64 inner_start = j;
                for (;j < snippet.size; j += 1){
                    if (snippet.str[j] == '{'){
                        depth += 1;
                    }
                    else if (snippet.str[j] == '}'){
                        depth -= 1;
                        if (depth == 0){
                            break;
                        }
                    }
                }
                String_Const_u8 inner = SCu8(snippet.str + inner_start, j - inner_start);
                if (choice){
                    inner = string_prefix(inner, string_find_first(inner, ','));
                    if (inner.size > 0 && inner.str[inner.size - 1] == '|'){
                        inner = string_chop(inner, 1);
                    }
                }
                size = lsp_snippet_copy_text(out, size, inner);
                i = j;
            }
            else{
                i = j - 1;
            }
            b32 seen = false;
            for (i32 k = 0; k < count; k += 1){
                if (stops[k].number == number){
                    seen = true;
                }
            }
            if (!seen && count < max_stops){
                stops[count].number = number;
                stops[count].start = stop_start;
                stops[count].end = size;
                count += 1;
            }
        }
        else{
            out[size++] = c;
        }
    }
    // NOTE(lsp): Without an explicit $0 the end of the snippet is the final stop, so Tab
    // after the last placeholder leaves it, e.g. past the ) of a method call.
    b32 has_final = false;
    for (i32 k = 0; k < count; k += 1){
        if (stops[k].number == 0){
            has_final = true;
        }
    }
    if (!has_final && count < max_stops){
        stops[count].number = 0;
        stops[count].start = size;
        stops[count].end = size;
        count += 1;
    }
    // NOTE(lsp): Visit stops in number order with $0 last.
    for (i32 a = 1; a < count; a += 1){
        Lsp_Snippet_Stop stop = stops[a];
        i32 key = (stop.number == 0)?max_i32:stop.number;
        i32 b = a;
        for (;b > 0 && ((stops[b - 1].number == 0)?max_i32:stops[b - 1].number) > key; b -= 1){
            stops[b] = stops[b - 1];
        }
        stops[b] = stop;
    }
    *stop_count = count;
    return(SCu8(out, size));
}

function void
lsp_snippet_close(Application_Links *app){
    if (lsp_snippet_markers != 0){
        managed_object_free(app, lsp_snippet_markers);
    }
    lsp_snippet_markers = 0;
    lsp_snippet_count = 0;
    lsp_snippet_pending_replace = false;
}

function b32
lsp_snippet_stop_range(Application_Links *app, i32 index, Range_i64 *out){
    Marker markers[2] = {};
    b32 result = (lsp_snippet_markers != 0 &&
                  managed_object_load_data(app, lsp_snippet_markers, index*2, 2, markers));
    if (result){
        *out = Ii64(markers[0].pos, markers[1].pos);
    }
    return(result);
}

function void
lsp_snippet_select(Application_Links *app, View_ID view, i32 index){
    Range_i64 range = {};
    if (lsp_snippet_stop_range(app, index, &range)){
        lsp_snippet_index = index;
        view_set_cursor_and_preferred_x(app, view, seek_pos(range.min));
        lsp_snippet_pending_replace = (range.max > range.min);
    }
}

function void
lsp_snippet_begin(Application_Links *app, View_ID view, Buffer_ID buffer, i64 base, Lsp_Snippet_Stop *stops, i32 count){
    lsp_snippet_close(app);
    if (count == 0){
        return;
    }
    lsp_snippet_markers = alloc_buffer_markers_on_buffer(app, buffer, count*2, 0);
    Scratch_Block scratch(app);
    Marker *markers = push_array_zero(scratch, Marker, count*2);
    for (i32 i = 0; i < count; i += 1){
        markers[i*2].pos = base + stops[i].start;
        markers[i*2].lean_right = false;
        markers[i*2 + 1].pos = base + stops[i].end;
        markers[i*2 + 1].lean_right = true;
    }
    managed_object_store_data(app, lsp_snippet_markers, 0, count*2, markers);
    lsp_snippet_buffer = buffer;
    lsp_snippet_count = count;
    lsp_snippet_select(app, view, 0);
    // NOTE(lsp): A lone $0 just places the cursor.
    if (count == 1 && stops[0].number == 0){
        lsp_snippet_close(app);
    }
}

// NOTE(lsp): Called before typed text is inserted: typing over a placeholder replaces it.
function void
lsp_snippet_before_insert(Application_Links *app){
    if (lsp_snippet_count > 0 && lsp_snippet_pending_replace){
        View_ID view = get_active_view(app, Access_ReadWriteVisible);
        Buffer_ID buffer = view_get_buffer(app, view, Access_ReadWriteVisible);
        Range_i64 range = {};
        if (buffer == lsp_snippet_buffer && lsp_snippet_stop_range(app, lsp_snippet_index, &range) &&
            view_get_cursor_pos(app, view) == range.min){
            buffer_replace_range(app, buffer, range, string_u8_empty);
            view_set_cursor_and_preferred_x(app, view, seek_pos(range.min));
        }
    }
    lsp_snippet_pending_replace = false;
}

// NOTE(lsp): Tab in insert mode; false when there is no snippet to move through.
function b32
lsp_snippet_next(Application_Links *app){
    b32 result = false;
    if (lsp_snippet_count > 0){
        View_ID view = get_active_view(app, Access_ReadWriteVisible);
        if (view_get_buffer(app, view, Access_Always) == lsp_snippet_buffer && lsp_snippet_index + 1 < lsp_snippet_count){
            lsp_snippet_select(app, view, lsp_snippet_index + 1);
            if (lsp_snippet_index + 1 >= lsp_snippet_count && !lsp_snippet_pending_replace){
                lsp_snippet_close(app);
            }
            result = true;
        }
        else{
            lsp_snippet_close(app);
        }
    }
    return(result);
}

////////////////////////////////
// NOTE(lsp): Completion

// NOTE(lsp): Completion never blocks typing. A request goes out when a word starts or
// after '.', and the answer is filtered and sorted locally with the lister's fuzzy scoring
// on every key, so narrowing the list needs no round trip. Incomplete lists (jdtls caps
// them) are requested again as the word grows.

struct Lsp_Completion_Item{
    String_Const_u8 label;
    String_Const_u8 detail;
    String_Const_u8 filter;
    String_Const_u8 sort;
    String_Const_u8 raw;
    i64 kind;
    i64 score;
};

global Arena lsp_completion_arena = {};
global Lsp_Completion_Item *lsp_completion_items = 0;
global i32 lsp_completion_item_count = 0;
global i32 *lsp_completion_order = 0;
global i32 *lsp_completion_sort_space = 0;
global i32 lsp_completion_order_count = 0;
global i32 lsp_completion_selected = 0;
global b32 lsp_completion_wanted = false;
global b32 lsp_completion_visible = false;
global b32 lsp_completion_incomplete = false;
global Buffer_ID lsp_completion_buffer = 0;
global View_ID lsp_completion_view = 0;
global i64 lsp_completion_start = 0;
global i64 lsp_completion_request_id = 0;

function void
lsp_completion_close(void){
    lsp_completion_wanted = false;
    lsp_completion_visible = false;
    lsp_completion_request_id = 0;
}

function b32
lsp_completion_less(i32 a, i32 b){
    Lsp_Completion_Item *x = &lsp_completion_items[a];
    Lsp_Completion_Item *y = &lsp_completion_items[b];
    b32 result = false;
    if (x->score != y->score){
        result = (x->score > y->score);
    }
    else{
        result = (string_compare(x->sort, y->sort) < 0);
    }
    return(result);
}

function void
lsp_completion_sort(i32 *order, i32 *temp, i32 count){
    if (count > 1){
        i32 half = count/2;
        lsp_completion_sort(order, temp, half);
        lsp_completion_sort(order + half, temp, count - half);
        i32 a = 0;
        i32 b = half;
        for (i32 i = 0; i < count; i += 1){
            if (b >= count || (a < half && !lsp_completion_less(order[b], order[a]))){
                temp[i] = order[a++];
            }
            else{
                temp[i] = order[b++];
            }
        }
        block_copy_dynamic_array(order, temp, count);
    }
}

function void
lsp_completion_filter(Application_Links *app){
    View_ID view = get_active_view(app, Access_Always);
    i64 pos = view_get_cursor_pos(app, view);
    if (view != lsp_completion_view || view_get_buffer(app, view, Access_Always) != lsp_completion_buffer ||
        pos < lsp_completion_start){
        lsp_completion_close();
        return;
    }
    Scratch_Block scratch(app);
    String_Const_u8 prefix = push_buffer_range(app, scratch, lsp_completion_buffer, Ii64(lsp_completion_start, pos));
    for (u64 i = 0; i < prefix.size; i += 1){
        if (!lsp_is_identifier_char(prefix.str[i])){
            lsp_completion_close();
            return;
        }
    }
    lsp_completion_order_count = 0;
    for (i32 i = 0; i < lsp_completion_item_count; i += 1){
        Lsp_Completion_Item *item = &lsp_completion_items[i];
        item->score = 0;
        if (prefix.size == 0 || lister_fuzzy_score_term(item->filter, prefix, &item->score)){
            lsp_completion_order[lsp_completion_order_count++] = i;
        }
    }
    lsp_completion_sort(lsp_completion_order, lsp_completion_sort_space, lsp_completion_order_count);
    // NOTE(lsp): Nothing is selected until Tab, like vim's completeopt=noselect.
    lsp_completion_selected = -1;
    lsp_completion_visible = (lsp_completion_order_count > 0);
}

function void
lsp_completion_request(Application_Links *app, View_ID view, Buffer_ID buffer, Lsp_Document *doc, i64 start, u8 trigger){
    Scratch_Block scratch(app);
    lsp_sync_document(app, doc);
    String_Const_u8 position = lsp_text_document_position(app, scratch, buffer, doc->uri, view_get_cursor_pos(app, view));
    String_Const_u8 context = string_u8_litexpr("{\"triggerKind\":1}");
    if (trigger != 0){
        context = push_u8_stringf(scratch, "{\"triggerKind\":2,\"triggerCharacter\":\"%c\"}", trigger);
    }
    String_Const_u8 params = push_u8_stringf(scratch, "%.*s,\"context\":%.*s}", (i32)(position.size - 1), position.str, string_expand(context));
    lsp_completion_request_id = lsp_send_request(app, "textDocument/completion", params);
    lsp_completion_wanted = true;
    lsp_completion_view = view;
    lsp_completion_buffer = buffer;
    lsp_completion_start = start;
}

function void
lsp_completion_handle_result(Application_Links *app, Json_Value *result){
    if (!lsp_completion_wanted){
        return;
    }
    Json_Value *items = result;
    lsp_completion_incomplete = false;
    if (result->kind == JsonKind_Object){
        items = json_get(result, "items");
        lsp_completion_incomplete = json_get(result, "isIncomplete")->boolean;
    }
    if (lsp_completion_arena.base_allocator == 0){
        lsp_completion_arena = make_arena_system();
    }
    linalloc_clear(&lsp_completion_arena);
    i32 count = clamp_top(items->count, 4000);
    lsp_completion_items = push_array_zero(&lsp_completion_arena, Lsp_Completion_Item, count);
    lsp_completion_order = push_array(&lsp_completion_arena, i32, count);
    lsp_completion_sort_space = push_array(&lsp_completion_arena, i32, count);
    lsp_completion_item_count = 0;
    for (Json_Value *item = items->first; item != 0 && lsp_completion_item_count < count; item = item->next){
        Lsp_Completion_Item *out = &lsp_completion_items[lsp_completion_item_count++];
        out->label = push_string_copy(&lsp_completion_arena, json_get(item, "label")->string);
        out->detail = push_string_copy(&lsp_completion_arena, json_get(item, "detail")->string);
        Json_Value *filter = json_get(item, "filterText");
        out->filter = (filter->kind == JsonKind_String)?push_string_copy(&lsp_completion_arena, filter->string):out->label;
        Json_Value *sort = json_get(item, "sortText");
        out->sort = (sort->kind == JsonKind_String)?push_string_copy(&lsp_completion_arena, sort->string):out->label;
        out->raw = push_string_copy(&lsp_completion_arena, item->raw);
        out->kind = json_i64(json_get(item, "kind"));
    }
    lsp_completion_filter(app);
}

// NOTE(lsp): Called by the editing layer after typed text was inserted.
function void
lsp_completion_on_insert(Application_Links *app, String_Const_u8 text);

function void lsp_signature_request(Application_Links *app, View_ID view, Buffer_ID buffer, Lsp_Document *doc);
function void lsp_signature_close(void);

function void
lsp_completion_on_insert(Application_Links *app, String_Const_u8 text){
    View_ID view = get_active_view(app, Access_ReadVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadVisible);
    Lsp_Document *doc = lsp_document_from_buffer(buffer);
    if (doc == 0 || !lsp_initialized || text.size == 0){
        lsp_completion_close();
        return;
    }
    u8 c = text.str[text.size - 1];
    i64 pos = view_get_cursor_pos(app, view);
    if (lsp_is_identifier_char(c)){
        if (lsp_completion_wanted && buffer == lsp_completion_buffer && lsp_completion_start <= pos){
            lsp_completion_filter(app);
            if (lsp_completion_wanted && lsp_completion_incomplete){
                lsp_completion_request(app, view, buffer, doc, lsp_completion_start, 0);
            }
        }
        else{
            i64 start = pos;
            for (;start > 0 && lsp_is_identifier_char(buffer_get_char(app, buffer, start - 1)); start -= 1);
            lsp_completion_request(app, view, buffer, doc, start, 0);
        }
    }
    else if (c == '.'){
        lsp_completion_request(app, view, buffer, doc, pos, '.');
    }
    else{
        lsp_completion_close();
    }
    
    if (c == '(' || c == ','){
        lsp_signature_request(app, view, buffer, doc);
    }
    else if (c == ')' || c == ';'){
        lsp_signature_close();
    }
}

function void
lsp_completion_on_backspace(Application_Links *app){
    if (lsp_completion_wanted){
        lsp_completion_filter(app);
    }
}

// NOTE(lsp): Ctrl-Space and friends: shows the menu for the word before the cursor.
function b32
lsp_completion_trigger(Application_Links *app){
    View_ID view = get_active_view(app, Access_ReadVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadVisible);
    Lsp_Document *doc = lsp_document_from_buffer(buffer);
    b32 result = false;
    if (doc != 0 && lsp_initialized){
        i64 start = view_get_cursor_pos(app, view);
        for (;start > 0 && lsp_is_identifier_char(buffer_get_char(app, buffer, start - 1)); start -= 1);
        lsp_completion_request(app, view, buffer, doc, start, 0);
        result = true;
    }
    return(result);
}

function void
lsp_completion_move(i32 delta){
    if (lsp_completion_visible && lsp_completion_order_count > 0){
        if (lsp_completion_selected < 0){
            lsp_completion_selected = (delta > 0)?0:(lsp_completion_order_count - 1);
        }
        else{
            lsp_completion_selected = (lsp_completion_selected + delta + lsp_completion_order_count) % lsp_completion_order_count;
        }
    }
}

function b32
lsp_completion_accept(Application_Links *app){
    if (!lsp_completion_visible){
        return(false);
    }
    View_ID view = get_active_view(app, Access_ReadWriteVisible);
    Buffer_ID buffer = view_get_buffer(app, view, Access_ReadWriteVisible);
    if (lsp_completion_selected < 0){
        return(false);
    }
    if (buffer != lsp_completion_buffer || lsp_completion_order_count == 0){
        lsp_completion_close();
        return(false);
    }
    Scratch_Block scratch(app);
    Lsp_Completion_Item *item = &lsp_completion_items[lsp_completion_order[lsp_completion_selected]];
    String_Const_u8 raw = push_string_copy(scratch, item->raw);
    i64 start = lsp_completion_start;
    lsp_completion_close();
    
    // NOTE(lsp): Resolving fills in the auto-import edits of the item.
    Json_Value *json_item = lsp_request(app, scratch, "completionItem/resolve", raw);
    if (json_item == 0 || json_item->kind != JsonKind_Object){
        json_item = json_parse(scratch, raw);
    }
    
    i64 cursor = view_get_cursor_pos(app, view);
    String_Const_u8 new_text = json_get(json_item, "label")->string;
    Json_Value *insert_text = json_get(json_item, "insertText");
    if (insert_text->kind == JsonKind_String){
        new_text = insert_text->string;
    }
    Json_Value *text_edit = json_get(json_item, "textEdit");
    if (text_edit->kind == JsonKind_Object){
        new_text = json_get(text_edit, "newText")->string;
        Json_Value *range = json_get(text_edit, "range");
        if (range->kind != JsonKind_Object){
            range = json_get(text_edit, "insert");
        }
        start = Min(lsp_range_from_json(app, buffer, range).min, cursor);
    }
    
    Lsp_Snippet_Stop stops[32];
    i32 stop_count = 0;
    if (json_i64(json_get(json_item, "insertTextFormat")) == 2){
        new_text = lsp_snippet_expand(scratch, new_text, stops, &stop_count, ArrayCount(stops));
    }
    
    Json_Value *additional = json_get(json_item, "additionalTextEdits");
    Lsp_Edit *edits = push_array(scratch, Lsp_Edit, additional->count + 1);
    i32 edit_count = lsp_collect_edits(app, buffer, additional, edits, additional->count);
    i64 shift = 0;
    for (i32 i = 0; i < edit_count; i += 1){
        if (edits[i].range.max <= start){
            shift += (i64)edits[i].text.size - range_size(edits[i].range);
        }
    }
    edits[edit_count].range = Ii64(start, cursor);
    edits[edit_count].text = new_text;
    edit_count += 1;
    lsp_apply_edits(app, buffer, edits, edit_count);
    
    i64 inserted_at = start + shift;
    view_set_cursor_and_preferred_x(app, view, seek_pos(inserted_at + new_text.size));
    lsp_snippet_begin(app, view, buffer, inserted_at, stops, stop_count);
    return(true);
}

////////////////////////////////
// NOTE(lsp): Signature help

global Arena lsp_signature_arena = {};
global String_Const_u8 lsp_signature_label = {};
global Range_i64 lsp_signature_active = {};
global View_ID lsp_signature_view = 0;
global i64 lsp_signature_line = 0;
global i64 lsp_signature_pos = 0;
global i64 lsp_signature_request_id = 0;

function void
lsp_signature_close(void){
    lsp_signature_label = string_u8_empty;
    lsp_signature_request_id = 0;
}

function void
lsp_signature_request(Application_Links *app, View_ID view, Buffer_ID buffer, Lsp_Document *doc){
    Scratch_Block scratch(app);
    lsp_sync_document(app, doc);
    i64 pos = view_get_cursor_pos(app, view);
    lsp_signature_request_id = lsp_send_request(app, "textDocument/signatureHelp",
                                                lsp_text_document_position(app, scratch, buffer, doc->uri, pos));
    lsp_signature_view = view;
    lsp_signature_pos = pos;
    lsp_signature_line = get_line_number_from_pos(app, buffer, pos);
}

function void
lsp_signature_handle_result(Application_Links *app, Json_Value *result){
    Json_Value *signatures = json_get(result, "signatures");
    if (signatures->count == 0){
        lsp_signature_close();
        return;
    }
    i64 active_signature = clamp(0, json_i64(json_get(result, "activeSignature")), signatures->count - 1);
    Json_Value *signature = signatures->first;
    for (i64 i = 0; i < active_signature; i += 1){
        signature = signature->next;
    }
    if (lsp_signature_arena.base_allocator == 0){
        lsp_signature_arena = make_arena_system();
    }
    linalloc_clear(&lsp_signature_arena);
    lsp_signature_label = push_string_copy(&lsp_signature_arena, json_get(signature, "label")->string);
    lsp_signature_active = Ii64((i64)0);
    
    Json_Value *active_value = json_get(signature, "activeParameter");
    if (active_value->kind != JsonKind_Number){
        active_value = json_get(result, "activeParameter");
    }
    i64 active = json_i64(active_value);
    Json_Value *parameter = json_get(signature, "parameters")->first;
    for (i64 i = 0; parameter != 0 && i < active; i += 1){
        parameter = parameter->next;
    }
    if (parameter != 0){
        Json_Value *label = json_get(parameter, "label");
        if (label->kind == JsonKind_String){
            u64 at = string_find_first(lsp_signature_label, label->string);
            if (at < lsp_signature_label.size){
                lsp_signature_active = Ii64_size(at, label->string.size);
            }
        }
        else if (label->kind == JsonKind_Array && label->count == 2){
            lsp_signature_active = Ii64(json_i64(label->first), json_i64(label->first->next));
        }
    }
}

function b32
lsp_handle_async_response(Application_Links *app, i64 id, Json_Value *json){
    b32 result = false;
    if (id != 0 && id == lsp_completion_request_id){
        lsp_completion_request_id = 0;
        lsp_completion_handle_result(app, json_get(json, "result"));
        result = true;
    }
    else if (id != 0 && id == lsp_signature_request_id){
        lsp_signature_request_id = 0;
        lsp_signature_handle_result(app, json_get(json, "result"));
        result = true;
    }
    return(result);
}

// NOTE(lsp): Closes signature help and completion once the cursor leaves where they apply.
function void
lsp_update_popups(Application_Links *app, View_ID active_view){
    if (lsp_signature_label.size > 0){
        Buffer_ID buffer = view_get_buffer(app, active_view, Access_Always);
        if (active_view != lsp_signature_view ||
            get_line_number_from_pos(app, buffer, view_get_cursor_pos(app, active_view)) != lsp_signature_line){
            lsp_signature_close();
        }
    }
    if (lsp_completion_wanted && (active_view != lsp_completion_view ||
                                  view_get_buffer(app, active_view, Access_Always) != lsp_completion_buffer)){
        lsp_completion_close();
    }
}

////////////////////////////////
// NOTE(lsp): More commands

function void
lsp_goto(Application_Links *app, char *method, char *title){
    View_ID view = 0;
    Buffer_ID buffer = 0;
    Lsp_Document *doc = lsp_active_document(app, &view, &buffer);
    if (doc == 0){
        jump_to_definition_at_cursor(app);
        return;
    }
    Scratch_Block scratch(app);
    String_Const_u8 params = lsp_text_document_position(app, scratch, buffer, doc->uri, view_get_cursor_pos(app, view));
    Json_Value *result = lsp_request(app, scratch, method, params);
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
        lsp_choose_location(app, view, locations, count, SCu8(title));
    }
    else{
        print_message(app, push_u8_stringf(scratch, "lsp: nothing found (%s)\n", method));
    }
}

CUSTOM_UI_COMMAND_SIG(lsp_goto_definition)
CUSTOM_DOC("Jumps to the definition of the symbol under the cursor using the language server, or the code index outside Java files.")
{
    lsp_goto(app, "textDocument/definition", "Definitions:");
}

CUSTOM_UI_COMMAND_SIG(lsp_goto_declaration)
CUSTOM_DOC("Jumps to the declaration of the symbol under the cursor using the language server.")
{
    lsp_goto(app, "textDocument/declaration", "Declarations:");
}

CUSTOM_UI_COMMAND_SIG(lsp_goto_implementation)
CUSTOM_DOC("Jumps to the implementations of the symbol under the cursor using the language server.")
{
    lsp_goto(app, "textDocument/implementation", "Implementations:");
}

CUSTOM_UI_COMMAND_SIG(lsp_goto_type_definition)
CUSTOM_DOC("Jumps to the type of the symbol under the cursor using the language server.")
{
    lsp_goto(app, "textDocument/typeDefinition", "Type definitions:");
}

CUSTOM_COMMAND_SIG(lsp_signature_help)
CUSTOM_DOC("Shows the signature of the call around the cursor using the language server.")
{
    View_ID view = 0;
    Buffer_ID buffer = 0;
    Lsp_Document *doc = lsp_active_document(app, &view, &buffer);
    if (doc != 0 && lsp_initialized){
        lsp_signature_request(app, view, buffer, doc);
    }
}

// NOTE(lsp): severity 0 matches every diagnostic; first and last pick the ends of the file.
function void
lsp_goto_diagnostic(Application_Links *app, b32 forward, i32 severity, b32 to_end){
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
        if (severity != 0 && d->severity != severity){
            continue;
        }
        i64 start = d->range.min;
        if (forward){
            if (!to_end && start > pos && (best == 0 || start < best->range.min)){
                best = d;
            }
            if (wrap == 0 || start < wrap->range.min){
                wrap = d;
            }
        }
        else{
            if (!to_end && start < pos && (best == 0 || start > best->range.min)){
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
    if (best == 0){
        print_message(app, string_u8_litexpr("lsp: no diagnostics of that kind\n"));
        return;
    }
    view_set_cursor_and_preferred_x(app, view, seek_pos(best->range.min));
    lsp_show_popup(app, view, best->message);
}

CUSTOM_COMMAND_SIG(lsp_next_diagnostic)
CUSTOM_DOC("Moves to the next error or warning reported by the language server and shows its message.")
{
    lsp_goto_diagnostic(app, true, 0, false);
}

CUSTOM_COMMAND_SIG(lsp_prev_diagnostic)
CUSTOM_DOC("Moves to the previous error or warning reported by the language server and shows its message.")
{
    lsp_goto_diagnostic(app, false, 0, false);
}

CUSTOM_COMMAND_SIG(lsp_show_line_diagnostics)
CUSTOM_DOC("Shows the language server messages for the cursor line in a popup.")
{
    View_ID view = 0;
    Buffer_ID buffer = 0;
    Lsp_Document *doc = lsp_active_document(app, &view, &buffer);
    if (doc == 0){
        return;
    }
    Scratch_Block scratch(app);
    i64 line = get_line_number_from_pos(app, buffer, view_get_cursor_pos(app, view));
    List_String_Const_u8 messages = {};
    for (i32 i = 0; i < doc->diagnostic_count; i += 1){
        if (doc->diagnostics[i].line == line){
            string_list_push(scratch, &messages, doc->diagnostics[i].message);
        }
    }
    if (messages.node_count > 0){
        lsp_show_popup(app, view, string_list_flatten(scratch, messages, 0, string_u8_litexpr("\n"), 0, StringFill_NoTerminate));
    }
}

CUSTOM_UI_COMMAND_SIG(lsp_rename)
CUSTOM_DOC("Renames the symbol under the cursor everywhere using the language server.")
{
    View_ID view = 0;
    Buffer_ID buffer = 0;
    Lsp_Document *doc = lsp_active_document(app, &view, &buffer);
    if (doc == 0){
        return;
    }
    Scratch_Block scratch(app);
    i64 pos = view_get_cursor_pos(app, view);
    String_Const_u8 word = push_buffer_range(app, scratch, buffer, enclose_pos_alpha_numeric_underscore(app, buffer, pos));
    Query_Bar_Group group(app);
    u8 space[256];
    Query_Bar bar = {};
    bar.prompt = string_u8_litexpr("Rename to: ");
    bar.string = SCu8(space, (u64)0);
    bar.string_capacity = sizeof(space);
    if (!query_user_general(app, &bar, false, word) || bar.string.size == 0){
        return;
    }
    String_Const_u8 position = lsp_text_document_position(app, scratch, buffer, doc->uri, pos);
    String_Const_u8 params = push_u8_stringf(scratch, "%.*s,\"newName\":\"%.*s\"}", (i32)(position.size - 1), position.str,
                                             string_expand(json_escape(scratch, bar.string)));
    Json_Value *result = lsp_request(app, scratch, "textDocument/rename", params);
    if (result != 0 && result->kind == JsonKind_Object){
        i32 count = lsp_apply_workspace_edit(app, result);
        print_message(app, push_u8_stringf(scratch, "lsp: renamed %d occurrences\n", count));
    }
}

function void
lsp_run_command(Application_Links *app, Arena *arena, Json_Value *command){
    String_Const_u8 name = json_get(command, "command")->string;
    Json_Value *arguments = json_get(command, "arguments");
    if (string_match(name, string_u8_litexpr("java.apply.workspaceEdit"))){
        for (Json_Value *argument = arguments->first; argument != 0; argument = argument->next){
            lsp_apply_workspace_edit(app, argument);
        }
    }
    else if (name.size > 0){
        String_Const_u8 args = (arguments->kind == JsonKind_Array)?arguments->raw:string_u8_litexpr("[]");
        Json_Value *result = lsp_request(app, arena, "workspace/executeCommand",
                                         push_u8_stringf(arena, "{\"command\":\"%.*s\",\"arguments\":%.*s}",
                                                         string_expand(json_escape(arena, name)), string_expand(args)));
        if (result != 0 && result->kind == JsonKind_Object &&
            (json_get(result, "changes")->kind == JsonKind_Object || json_get(result, "documentChanges")->kind == JsonKind_Array)){
            lsp_apply_workspace_edit(app, result);
        }
    }
}

function void
lsp_run_code_action(Application_Links *app, Arena *arena, Json_Value *action){
    Json_Value *edit = json_get(action, "edit");
    if (edit->kind == JsonKind_Object){
        lsp_apply_workspace_edit(app, edit);
    }
    Json_Value *command = json_get(action, "command");
    if (command->kind == JsonKind_Object){
        lsp_run_command(app, arena, command);
    }
    else if (command->kind == JsonKind_String){
        lsp_run_command(app, arena, action);
    }
}

// NOTE(lsp): Quick fixes and refactorings for the cursor line. only limits the kind, e.g.
// "source" or "source.organizeImports"; apply_first skips the list.
function void
lsp_code_action(Application_Links *app, char *only, b32 apply_first){
    View_ID view = 0;
    Buffer_ID buffer = 0;
    Lsp_Document *doc = lsp_active_document(app, &view, &buffer);
    if (doc == 0){
        return;
    }
    Scratch_Block scratch(app);
    i64 pos = view_get_cursor_pos(app, view);
    i64 line = get_line_number_from_pos(app, buffer, pos);
    i64 line_end_char = 0;
    i64 line_index = 0;
    lsp_position_from_pos(app, buffer, get_line_end_pos(app, buffer, line), &line_index, &line_end_char);
    List_String_Const_u8 diagnostics = {};
    for (i32 i = 0; i < doc->diagnostic_count; i += 1){
        if (doc->diagnostics[i].line == line){
            string_list_push(scratch, &diagnostics, doc->diagnostics[i].raw);
        }
    }
    String_Const_u8 only_part = string_u8_empty;
    if (only != 0){
        only_part = push_u8_stringf(scratch, ",\"only\":[\"%s\"]", only);
    }
    String_Const_u8 params = push_u8_stringf(scratch,
                                             "{\"textDocument\":{\"uri\":\"%.*s\"},"
                                             "\"range\":{\"start\":{\"line\":%lld,\"character\":0},\"end\":{\"line\":%lld,\"character\":%lld}},"
                                             "\"context\":{\"diagnostics\":[%.*s]%.*s}}",
                                             string_expand(json_escape(scratch, doc->uri)), line_index, line_index, line_end_char,
                                             string_expand(string_list_flatten(scratch, diagnostics, 0, string_u8_litexpr(","), 0, StringFill_NoTerminate)),
                                             string_expand(only_part));
    Json_Value *result = lsp_request(app, scratch, "textDocument/codeAction", params);
    if (result == 0 || result->kind != JsonKind_Array || result->count == 0){
        print_message(app, string_u8_litexpr("lsp: no code actions here\n"));
        return;
    }
    if (apply_first){
        lsp_run_code_action(app, scratch, result->first);
        return;
    }
    Json_Value **actions = push_array(scratch, Json_Value*, result->count);
    Lister_Block lister(app, scratch);
    lister_set_query(lister, "Code actions:");
    lister_set_default_handlers(lister);
    ((Lister*)lister)->fuzzy_filter = true;
    i32 count = 0;
    for (Json_Value *item = result->first; item != 0; item = item->next){
        actions[count] = item;
        lister_add_item(lister, json_get(item, "title")->string, json_get(item, "kind")->string, IntAsPtr(count), 0);
        count += 1;
    }
    Lister_Result l_result = run_lister(app, lister);
    if (!l_result.canceled){
        lsp_run_code_action(app, scratch, actions[PtrAsInt(l_result.user_data)]);
    }
}

CUSTOM_UI_COMMAND_SIG(lsp_code_actions)
CUSTOM_DOC("Lists the language server's quick fixes and refactorings for the cursor line.")
{
    lsp_code_action(app, 0, false);
}

CUSTOM_UI_COMMAND_SIG(lsp_source_actions)
CUSTOM_DOC("Lists the language server's source actions (generate, organize imports ...) for the file.")
{
    lsp_code_action(app, "source", false);
}

CUSTOM_COMMAND_SIG(lsp_organize_imports)
CUSTOM_DOC("Adds missing imports and removes unused ones using the language server.")
{
    lsp_code_action(app, "source.organizeImports", true);
}

CUSTOM_COMMAND_SIG(lsp_format)
CUSTOM_DOC("Formats the file with the language server.")
{
    View_ID view = 0;
    Buffer_ID buffer = 0;
    Lsp_Document *doc = lsp_active_document(app, &view, &buffer);
    if (doc == 0){
        return;
    }
    Scratch_Block scratch(app);
    i32 indent_width = (i32)def_get_config_u64(app, vars_save_string_lit("indent_width"));
    b32 indent_with_tabs = def_get_config_b32(vars_save_string_lit("indent_with_tabs"));
    String_Const_u8 params = push_u8_stringf(scratch, "{\"textDocument\":{\"uri\":\"%.*s\"},\"options\":{\"tabSize\":%d,\"insertSpaces\":%s}}",
                                             string_expand(json_escape(scratch, doc->uri)), clamp_bot(1, indent_width),
                                             indent_with_tabs?"false":"true");
    Json_Value *result = lsp_request(app, scratch, "textDocument/formatting", params);
    if (result != 0 && result->kind == JsonKind_Array && result->count > 0){
        Lsp_Edit *edits = push_array(scratch, Lsp_Edit, result->count);
        i32 count = lsp_collect_edits(app, buffer, result, edits, result->count);
        lsp_apply_edits(app, buffer, edits, count);
    }
}

struct Lsp_Symbol{
    String_Const_u8 name;
    Lsp_Location location;
};

function void
lsp_collect_document_symbols(Arena *arena, Json_Value *list, String_Const_u8 container, String_Const_u8 uri,
                             Lister *lister, Lsp_Symbol *symbols, i32 *count, i32 max){
    for (Json_Value *item = list->first; item != 0 && *count < max; item = item->next){
        String_Const_u8 name = json_get(item, "name")->string;
        String_Const_u8 full_name = name;
        if (container.size > 0){
            full_name = push_u8_stringf(arena, "%.*s.%.*s", string_expand(container), string_expand(name));
        }
        Lsp_Symbol *symbol = &symbols[*count];
        symbol->name = full_name;
        Json_Value *location = json_get(item, "location");
        if (location->kind == JsonKind_Object){
            symbol->location = lsp_location_from_json(location);
        }
        else{
            Json_Value *start = json_get(json_get(item, "selectionRange"), "start");
            symbol->location.uri = uri;
            symbol->location.line = json_i64(json_get(start, "line"));
            symbol->location.character = json_i64(json_get(start, "character"));
        }
        char *kind = lsp_kind_name(lsp_symbol_kind_names, ArrayCount(lsp_symbol_kind_names), json_i64(json_get(item, "kind")));
        String_Const_u8 detail = json_get(item, "containerName")->string;
        if (detail.size == 0){
            detail = json_get(item, "detail")->string;
        }
        lister_add_item(lister, full_name, push_u8_stringf(arena, "%s %.*s", kind, string_expand(detail)), IntAsPtr(*count), 0);
        *count += 1;
        Json_Value *children = json_get(item, "children");
        if (children->count > 0){
            lsp_collect_document_symbols(arena, children, full_name, uri, lister, symbols, count, max);
        }
    }
}

function void
lsp_symbols(Application_Links *app, b32 workspace){
    View_ID view = 0;
    Buffer_ID buffer = 0;
    Lsp_Document *doc = lsp_active_document(app, &view, &buffer);
    if (doc == 0 && !(workspace && lsp_initialized)){
        list_all_functions_current_buffer_lister(app);
        return;
    }
    Scratch_Block scratch(app);
    Json_Value *result = 0;
    if (workspace){
        Query_Bar_Group group(app);
        u8 space[256];
        Query_Bar bar = {};
        bar.prompt = string_u8_litexpr("Workspace symbol: ");
        bar.string = SCu8(space, (u64)0);
        bar.string_capacity = sizeof(space);
        if (!query_user_string(app, &bar)){
            return;
        }
        result = lsp_request(app, scratch, "workspace/symbol",
                             push_u8_stringf(scratch, "{\"query\":\"%.*s\"}", string_expand(json_escape(scratch, bar.string))));
    }
    else{
        result = lsp_request(app, scratch, "textDocument/documentSymbol",
                             push_u8_stringf(scratch, "{\"textDocument\":{\"uri\":\"%.*s\"}}", string_expand(json_escape(scratch, doc->uri))));
    }
    if (result == 0 || result->kind != JsonKind_Array || result->count == 0){
        print_message(app, string_u8_litexpr("lsp: no symbols\n"));
        return;
    }
    i32 max = 20000;
    Lsp_Symbol *symbols = push_array_zero(scratch, Lsp_Symbol, max);
    i32 count = 0;
    Lister_Block lister(app, scratch);
    lister_set_query(lister, workspace?string_u8_litexpr("Workspace symbols:"):string_u8_litexpr("Symbols:"));
    lister_set_default_handlers(lister);
    ((Lister*)lister)->fuzzy_filter = true;
    lsp_collect_document_symbols(scratch, result, string_u8_empty, (doc != 0)?doc->uri:string_u8_empty, lister, symbols, &count, max);
    Lister_Result l_result = run_lister(app, lister);
    if (!l_result.canceled){
        lsp_jump_to_location(app, view, symbols[PtrAsInt(l_result.user_data)].location);
    }
}

CUSTOM_UI_COMMAND_SIG(lsp_document_symbols)
CUSTOM_DOC("Lists the classes, methods and fields of this file with fzf style filtering.")
{
    lsp_symbols(app, false);
}

CUSTOM_UI_COMMAND_SIG(lsp_workspace_symbols)
CUSTOM_DOC("Searches the symbols of the whole workspace using the language server.")
{
    lsp_symbols(app, true);
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

function void
lsp_draw_snippet(Application_Links *app, Buffer_ID buffer, Text_Layout_ID text_layout_id, f32 roundness){
    Range_i64 range = {};
    if (lsp_snippet_count > 0 && lsp_snippet_pending_replace && buffer == lsp_snippet_buffer &&
        lsp_snippet_stop_range(app, lsp_snippet_index, &range) && range.max > range.min){
        draw_character_block(app, text_layout_id, range, roundness, fcolor_id(defcolor_highlight));
        paint_text_color_fcolor(app, text_layout_id, range, fcolor_id(defcolor_at_highlight));
    }
}

// NOTE(lsp): The menu sits under the start of the word being completed.
function void
lsp_draw_completion(Application_Links *app, View_ID view_id, Text_Layout_ID text_layout_id, Face_ID face_id, Rect_f32 region){
    if (!lsp_completion_visible || view_id != lsp_completion_view || lsp_completion_order_count == 0){
        return;
    }
    Scratch_Block scratch(app);
    Face_Metrics metrics = get_face_metrics(app, face_id);
    Rect_f32 anchor = text_layout_character_on_screen(app, text_layout_id, lsp_completion_start);
    i32 rows = clamp_top(lsp_completion_order_count, 12);
    i32 first = clamp_bot(0, lsp_completion_selected - rows + 1);
    
    f32 kind_width = metrics.normal_advance*8.f;
    f32 label_width = 0.f;
    f32 detail_width = 0.f;
    for (i32 i = 0; i < rows; i += 1){
        Lsp_Completion_Item *item = &lsp_completion_items[lsp_completion_order[first + i]];
        label_width = Max(label_width, get_string_advance(app, face_id, string_prefix(item->label, 60)));
        detail_width = Max(detail_width, get_string_advance(app, face_id, string_prefix(item->detail, 50)));
    }
    f32 pad = metrics.normal_advance;
    f32 width = kind_width + label_width + 2.f*pad + detail_width + pad;
    f32 height = rows*metrics.line_height + pad*0.5f;
    Rect_f32 box = Rf32(anchor.x0 - pad, anchor.y1 + 2.f, anchor.x0 - pad + width, anchor.y1 + 2.f + height);
    if (box.y1 > region.y1){
        box.y1 = anchor.y0 - 2.f;
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
    ARGB_Color text_color = fcolor_resolve(fcolor_id(defcolor_text_default));
    ARGB_Color dim_color = fcolor_resolve(fcolor_id(defcolor_comment));
    ARGB_Color kind_color = fcolor_resolve(fcolor_id(defcolor_keyword));
    for (i32 i = 0; i < rows; i += 1){
        i32 index = first + i;
        Lsp_Completion_Item *item = &lsp_completion_items[lsp_completion_order[index]];
        f32 y = box.y0 + pad*0.25f + i*metrics.line_height;
        if (index == lsp_completion_selected){
            draw_rectangle(app, Rf32(box.x0 + 1.f, y, box.x1 - 1.f, y + metrics.line_height), 2.f,
                           fcolor_resolve(fcolor_id(defcolor_list_item_active)));
        }
        char *kind = lsp_kind_name(lsp_kind_names, ArrayCount(lsp_kind_names), item->kind);
        draw_string(app, face_id, SCu8(kind), V2f32(box.x0 + pad, y), kind_color);
        draw_string(app, face_id, string_prefix(item->label, 60), V2f32(box.x0 + pad + kind_width, y), text_color);
        draw_string(app, face_id, string_prefix(item->detail, 50), V2f32(box.x0 + pad + kind_width + label_width + pad, y), dim_color);
    }
}

// NOTE(lsp): The signature sits above the cursor line with the current parameter lit.
function void
lsp_draw_signature(Application_Links *app, View_ID view_id, Text_Layout_ID text_layout_id, Face_ID face_id, Rect_f32 region){
    if (lsp_signature_label.size == 0 || view_id != lsp_signature_view){
        return;
    }
    Face_Metrics metrics = get_face_metrics(app, face_id);
    Rect_f32 anchor = text_layout_character_on_screen(app, text_layout_id, lsp_signature_pos);
    String_Const_u8 label = string_prefix(lsp_signature_label, 200);
    f32 pad = metrics.normal_advance;
    f32 width = get_string_advance(app, face_id, label) + 2.f*pad;
    Rect_f32 box = Rf32(anchor.x0 - pad, anchor.y0 - 2.f - metrics.line_height - pad*0.5f, anchor.x0 - pad + width, anchor.y0 - 2.f);
    if (box.y0 < region.y0){
        box.y0 = anchor.y1 + 2.f;
        box.y1 = box.y0 + metrics.line_height + pad*0.5f;
    }
    if (box.x1 > region.x1){
        f32 shift = clamp_top(box.x1 - region.x1, box.x0 - region.x0);
        box.x0 -= shift;
        box.x1 -= shift;
    }
    draw_rectangle(app, box, 4.f, lsp_theme_color(defcolor_lsp_popup_back, fcolor_resolve(fcolor_id(defcolor_margin))));
    draw_rectangle_outline(app, box, 4.f, 1.f, fcolor_resolve(fcolor_id(defcolor_margin_active)));
    Vec2_f32 p = V2f32(box.x0 + pad, box.y0 + pad*0.25f);
    Range_i64 active = Ii64(clamp_top(lsp_signature_active.min, (i64)label.size), clamp_top(lsp_signature_active.max, (i64)label.size));
    String_Const_u8 before = string_prefix(label, active.min);
    String_Const_u8 middle = string_substring(label, active);
    String_Const_u8 after = string_skip(label, active.max);
    ARGB_Color text_color = fcolor_resolve(fcolor_id(defcolor_text_default));
    p = draw_string(app, face_id, before, p, text_color);
    p = draw_string(app, face_id, middle, p, fcolor_resolve(fcolor_id(defcolor_keyword)));
    draw_string(app, face_id, after, p, text_color);
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
}

#endif //FCODER_LSP_CPP

// BOTTOM
