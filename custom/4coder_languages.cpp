/*
4coder_languages.cpp - Keyword highlighting for languages lexed with the C++ lexer.
*/

// TOP

#if !defined(FCODER_LANGUAGES_CPP)
#define FCODER_LANGUAGES_CPP

// NOTE(lang): Java, Kotlin and Zig look enough like C++ (braces, // and /* */ comments,
// strings, numbers) that the C++ lexer gives usable tokens, auto indentation and code
// index. What it gets wrong are the keywords: its identifiers and C++ keywords are
// recolored here from each language's own list. @annotations and Zig's @builtins get the
// preprocessor color. Not handled: Kotlin raw strings ("""), Zig multiline strings (\\).

global char *lang_java_keywords[] = {
    "abstract", "assert", "boolean", "break", "byte", "case", "catch", "char", "class",
    "const", "continue", "default", "do", "double", "else", "enum", "extends", "final",
    "finally", "float", "for", "goto", "if", "implements", "import", "instanceof", "int",
    "interface", "long", "native", "new", "null", "package", "permits", "private",
    "protected", "public", "record", "return", "sealed", "short", "static", "strictfp",
    "super", "switch", "synchronized", "this", "throw", "throws", "transient", "try", "var",
    "void", "volatile", "while", "yield",
};

// NOTE(lang): Hard keywords, plus the soft and modifier keywords that are not commonly
// used as names (value, field, get, set stay plain).
global char *lang_kotlin_keywords[] = {
    "as", "break", "class", "continue", "do", "else", "for", "fun", "if", "in", "interface",
    "is", "null", "object", "package", "return", "super", "this", "throw", "try",
    "typealias", "typeof", "val", "var", "when", "while",
    "by", "catch", "constructor", "finally", "import", "init", "where",
    "abstract", "actual", "annotation", "companion", "const", "crossinline", "data", "enum",
    "expect", "external", "final", "infix", "inline", "inner", "internal", "lateinit",
    "noinline", "open", "operator", "out", "override", "private", "protected", "public",
    "reified", "sealed", "suspend", "tailrec", "vararg",
};

global char *lang_zig_keywords[] = {
    "addrspace", "align", "allowzero", "and", "anyframe", "anytype", "asm", "break",
    "callconv", "catch", "comptime", "const", "continue", "defer", "else", "enum",
    "errdefer", "error", "export", "extern", "fn", "for", "if", "inline", "linksection",
    "noalias", "noinline", "nosuspend", "opaque", "or", "orelse", "packed", "pub", "resume",
    "return", "struct", "suspend", "switch", "test", "threadlocal", "try", "union",
    "unreachable", "var", "volatile", "while",
    "null", "undefined",
    "anyerror", "anyopaque", "bool", "c_char", "c_int", "c_long", "c_longdouble",
    "c_longlong", "c_short", "c_uint", "c_ulong", "c_ulonglong", "c_ushort",
    "comptime_float", "comptime_int", "f16", "f32", "f64", "f80", "f128", "isize",
    "noreturn", "type", "usize", "void",
};

struct Lang_Keywords{
    char *extensions[4];
    char **keywords;
    i32 keyword_count;
    // NOTE(lang): Zig's arbitrary width integers, i0 ... u65535.
    b32 sized_integers;
};

global Lang_Keywords lang_keyword_sets[] = {
    {{"java"}, lang_java_keywords, ArrayCount(lang_java_keywords), false},
    {{"kt", "kts"}, lang_kotlin_keywords, ArrayCount(lang_kotlin_keywords), false},
    {{"zig", "zon"}, lang_zig_keywords, ArrayCount(lang_zig_keywords), true},
};

// NOTE(lang): The keyword set for the buffer's file, or 0 for C and C++ (and anything
// else), which keep the C++ lexer's own colors.
function Lang_Keywords*
lang_keywords_from_buffer(Application_Links *app, Buffer_ID buffer){
    Scratch_Block scratch(app);
    String_Const_u8 ext = string_file_extension(push_buffer_file_name(app, scratch, buffer));
    Lang_Keywords *result = 0;
    for (i32 i = 0; i < ArrayCount(lang_keyword_sets) && result == 0 && ext.size > 0; i += 1){
        for (i32 j = 0; j < ArrayCount(lang_keyword_sets[i].extensions) && lang_keyword_sets[i].extensions[j] != 0; j += 1){
            if (string_match(ext, SCu8(lang_keyword_sets[i].extensions[j]))){
                result = &lang_keyword_sets[i];
                break;
            }
        }
    }
    return(result);
}

function b32
lang_is_keyword(Lang_Keywords *lang, String_Const_u8 word){
    b32 result = false;
    for (i32 i = 0; i < lang->keyword_count; i += 1){
        if (string_match(word, SCu8(lang->keywords[i]))){
            result = true;
            break;
        }
    }
    if (!result && lang->sized_integers && word.size >= 2 && (word.str[0] == 'i' || word.str[0] == 'u')){
        result = true;
        for (u64 i = 1; i < word.size; i += 1){
            if (!character_is_base10(word.str[i])){
                result = false;
                break;
            }
        }
    }
    return(result);
}

// NOTE(lang): draw_cpp_token_colors with the buffer's language keywords.
function void
lang_draw_token_colors(Application_Links *app, Buffer_ID buffer, Text_Layout_ID text_layout_id, Token_Array *array){
    Lang_Keywords *lang = lang_keywords_from_buffer(app, buffer);
    if (lang == 0){
        draw_cpp_token_colors(app, text_layout_id, array);
        return;
    }
    Scratch_Block scratch(app);
    Range_i64 visible_range = text_layout_get_visible_range(app, text_layout_id);
    i64 first_index = token_index_from_pos(array, visible_range.first);
    Token_Iterator_Array it = token_iterator_index(0, array, first_index);
    for (;;){
        Token *token = token_it_read(&it);
        if (token->pos >= visible_range.one_past_last){
            break;
        }
        FColor color = get_token_color_cpp(*token);
        Range_i64 range = Ii64_size(token->pos, token->size);
        if (token->kind == TokenBaseKind_Keyword || token->kind == TokenBaseKind_Identifier){
            Temp_Memory temp = begin_temp(scratch);
            String_Const_u8 word = push_token_lexeme(app, scratch, buffer, token);
            if (string_match(word, string_u8_litexpr("true")) || string_match(word, string_u8_litexpr("false"))){
                color = fcolor_id(defcolor_bool_constant);
            }
            else if (lang_is_keyword(lang, word)){
                color = fcolor_id(defcolor_keyword);
            }
            else{
                color = fcolor_id(defcolor_text_default);
            }
            if (token->pos > 0 && buffer_get_char(app, buffer, token->pos - 1) == '@'){
                color = fcolor_id(defcolor_preproc);
                range.min -= 1;
            }
            end_temp(temp);
        }
        paint_text_color(app, text_layout_id, range, fcolor_resolve(color));
        if (!token_it_inc_all(&it)){
            break;
        }
    }
}

#endif //FCODER_LANGUAGES_CPP

// BOTTOM
