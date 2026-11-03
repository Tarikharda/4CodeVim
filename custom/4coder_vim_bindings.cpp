/*
4coder_vim_bindings.cpp - The default 4coder setup with the vim editing layer on top.
*/

// TOP

#if !defined(FCODER_VIM_BINDINGS_CPP)
#define FCODER_VIM_BINDINGS_CPP

// NOTE: System headers for the language server client (process, pipes); included before
// the 4coder headers define macros such as global and function.
#if OS_LINUX || OS_MAC
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#endif
#include <time.h>

#include "4coder_default_include.cpp"
#include "4coder_fuzzy_search.cpp"
#include "4coder_lsp.cpp"
#include "4coder_git.cpp"
#include "4coder_vim.cpp"

#if !defined(META_PASS)
#include "generated/managed_id_metadata.cpp"
#endif

// NOTE: Per frame work of the language server client and the git signs.
function void
vim_tick(Application_Links *app, Frame_Info frame_info){
    default_tick(app, frame_info);
    lsp_update(app);
    git_update(app);
    vim_apply_scrolloff(app);
}

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

    // NOTE(lsp): jdtls for Java buffers
    lsp_init(app);
    set_custom_hook(app, HookID_Tick, vim_tick);
}

#endif //FCODER_VIM_BINDINGS_CPP

// BOTTOM
