/*
4coder_vim_bindings.cpp - The default 4coder setup with the vim editing layer on top.
*/

// TOP

#if !defined(FCODER_VIM_BINDINGS_CPP)
#define FCODER_VIM_BINDINGS_CPP

#include "4coder_default_include.cpp"
#include "4coder_fuzzy_search.cpp"
#include "4coder_vim.cpp"

#if !defined(META_PASS)
#include "generated/managed_id_metadata.cpp"
#endif

void
custom_layer_init(Application_Links *app){
    Thread_Context *tctx = get_thread_context(app);

    // NOTE(allen): setup for default framework
    default_framework_init(app);

    // NOTE(allen): default hooks and command maps
    set_all_default_hooks(app);
    mapping_init(tctx, &framework_mapping);
    String_ID global_map_id = vars_save_string_lit("keys_global");
    String_ID file_map_id = vars_save_string_lit("keys_file");
    String_ID code_map_id = vars_save_string_lit("keys_code");
#if OS_MAC
    setup_mac_mapping(&framework_mapping, global_map_id, file_map_id, code_map_id);
#else
    setup_default_mapping(&framework_mapping, global_map_id, file_map_id, code_map_id);
#endif
	setup_essential_mapping(&framework_mapping, global_map_id, file_map_id, code_map_id);

    // NOTE(vim): installs the modal key handling and the mode indicator
    vim_init(app);
}

#endif //FCODER_VIM_BINDINGS_CPP

// BOTTOM
