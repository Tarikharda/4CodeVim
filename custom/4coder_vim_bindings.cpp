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
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#endif
#include <time.h>

#include "4coder_default_include.cpp"
#include "4coder_fuzzy_search.cpp"
#include "4coder_lsp.cpp"
#include "4coder_git.cpp"
#include "4coder_junit.cpp"
#include "4coder_languages.cpp"
#include "4coder_vim_keys.cpp"
#include "4coder_vim.cpp"

#if !defined(META_PASS)
#include "generated/managed_id_metadata.cpp"
#endif

#if OS_LINUX || OS_MAC
// NOTE: Apps started from the Dock or Finder get launchd's PATH (/usr/bin:/bin:/usr/sbin:
// /sbin), without Homebrew or the user's own folders, where rg, lazygit, jdtls and java
// live. Like VS Code, take PATH and JAVA_HOME from a login shell (any shell: env prints
// plain NAME=value lines), then make sure Homebrew's folders are there.
function void
import_login_shell_environment(void){
    char *shell = getenv("SHELL");
    if (shell == 0 || shell[0] == 0){
        shell = "/bin/zsh";
    }
    char command[1024];
    snprintf(command, sizeof(command), "'%s' -l -c env < /dev/null 2> /dev/null", shell);
    FILE *pipe = popen(command, "r");
    if (pipe != 0){
        char line[16384];
        for (;fgets(line, sizeof(line), pipe) != 0;){
            size_t length = strlen(line);
            if (length > 0 && line[length - 1] == '\n'){
                line[length - 1] = 0;
            }
            if (strncmp(line, "PATH=", 5) == 0 && line[5] != 0){
                setenv("PATH", line + 5, 1);
            }
            else if (strncmp(line, "JAVA_HOME=", 10) == 0 && line[10] != 0){
                setenv("JAVA_HOME", line + 10, 1);
            }
        }
        pclose(pipe);
    }
    char *extra[] = {"/opt/homebrew/bin", "/opt/homebrew/sbin", "/usr/local/bin"};
    for (i32 i = 0; i < ArrayCount(extra); i += 1){
        char *path = getenv("PATH");
        char padded[16384];
        snprintf(padded, sizeof(padded), ":%s:", (path != 0)?path:"");
        char needle[256];
        snprintf(needle, sizeof(needle), ":%s:", extra[i]);
        if (strstr(padded, needle) == 0){
            char joined[16384];
            snprintf(joined, sizeof(joined), "%s%s%s", (path != 0)?path:"", (path != 0 && path[0] != 0)?":":"", extra[i]);
            setenv("PATH", joined, 1);
        }
    }
}
#endif

// NOTE: Per frame work of the language server client, the git signs and the test runner.
function void
vim_tick(Application_Links *app, Frame_Info frame_info){
    default_tick(app, frame_info);
    lsp_update(app);
    git_update(app);
    junit_update(app);
    vim_apply_scrolloff(app);
}

void
custom_layer_init(Application_Links *app){
    Thread_Context *tctx = get_thread_context(app);

#if OS_MAC
    import_login_shell_environment();
#endif

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
