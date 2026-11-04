#!/bin/bash

# End to end tests for the vim layer. Each case opens a fixture in a fresh 4coder
# window from ../build, types keys with xdotool, saves with :w and compares the file.
# Runs on a private Xvfb display so no key can reach the user's own windows
# (sudo apt install xvfb). VIM_TEST_REAL_DISPLAY=1 uses the current display instead,
# only sending keys while the launched window has focus.
#
#   bin/test-vim-linux.sh            run every case
#   bin/test-vim-linux.sh dot        run cases whose name contains "dot"
#
# Steps: "t:<text>" types text, "k:<keys>" sends xdotool key names (Escape, ctrl+a ...).

ME="$(readlink -f "$0")"
LOCATION="$(dirname "$ME")"
SRC_ROOT="$(dirname "$LOCATION")"
PROJECT_ROOT="$(dirname "$SRC_ROOT")"
BUILD_ROOT="$PROJECT_ROOT/build"

FILTER="$1"
WORK_DIR="$(mktemp -d)"
XVFB_PID=""
trap 'rm -rf "$WORK_DIR"; [ -n "$XVFB_PID" ] && kill $XVFB_PID 2> /dev/null' EXIT

if [ -z "$VIM_TEST_REAL_DISPLAY" ]; then
    if ! command -v Xvfb > /dev/null; then
        echo "test-vim: needs Xvfb so test keys never reach your own windows (sudo apt install xvfb)"
        echo "test-vim: or run with VIM_TEST_REAL_DISPLAY=1 to use the current display"
        exit 2
    fi
    for NUMBER in $(seq 90 99); do
        [ -e "/tmp/.X11-unix/X$NUMBER" ] || break
    done
    Xvfb ":$NUMBER" -screen 0 1920x1080x24 > /dev/null 2>&1 &
    XVFB_PID=$!
    export DISPLAY=":$NUMBER"
    sleep 1
    # NOTE: No window manager here, so focus is set directly on the window.
    focus_window(){ xdotool windowfocus --sync "$1" > /dev/null 2>&1; }
    active_window(){ xdotool getwindowfocus 2> /dev/null; }
else
    focus_window(){ xdotool windowactivate --sync "$1" > /dev/null 2>&1; }
    active_window(){ xdotool getactivewindow 2> /dev/null; }
fi
# NOTE: A git repository makes the fixture's folder the root for the rg/fzf pickers.
git init -q "$WORK_DIR"

PASSED=0
FAILED=0
FAILED_NAMES=""

TXT=$'alpha beta gamma\n    indented line two\nthree (a, b) "str"\nfour 42 end\n'
JAVA=$'public class Hello {\n    public static void main(String[] args) {\n        System.out.println("hello world");\n    }\n}\n'

# run_case <ext> <fixture> <steps...>; prints the saved file to stdout
run_case(){
    local ext="$1"; shift
    local fixture="$1"; shift
    local file="$WORK_DIR/case.$ext"
    printf '%s' "$fixture" > "$file"
    if [[ "$CASE_NAME" == git* ]]; then
        # NOTE: git cases start from a committed fixture, so edits show up as hunks.
        git -C "$WORK_DIR" add "case.$ext"
        git -C "$WORK_DIR" -c user.name=test -c user.email=test@test commit -q -m fixture > /dev/null 2>&1
    fi

    pushd "$BUILD_ROOT" > /dev/null
    ./4ed "$file" > /dev/null 2>&1 &
    local pid=$!
    popd > /dev/null

    local window=""
    for i in $(seq 1 50); do
        window="$(xdotool search --all --onlyvisible --pid $pid --name "" 2>/dev/null | head -1)"
        [ -n "$window" ] && break
        sleep 0.1
    done
    sleep 0.8
    focus_window "$window"
    sleep 0.2

    local ok=1
    for step in "$@"; do
        if [ "$(active_window)" != "$window" ]; then
            # NOTE: Something else took focus; take it back once before giving up.
            focus_window "$window"
            sleep 0.3
        fi
        if [ "$(active_window)" != "$window" ]; then
            ok=0
            break
        fi
        case "$step" in
            t:*) xdotool type --delay 60 -- "${step#t:}";;
            k:*) xdotool key --delay 60 ${step#k:};;
        esac
        sleep 0.1
    done
    if [ $ok = 1 ] && [ "$(active_window)" = "$window" ]; then
        xdotool key Escape
        xdotool type --delay 60 ":w"
        xdotool key Return
        sleep 0.5
    else
        echo "<window lost focus>"
    fi
    kill $pid 2> /dev/null
    wait $pid 2> /dev/null
    cat "$file"
}

# check <name> <ext> <fixture> <expected> <steps...>
check(){
    local name="$1" ext="$2" fixture="$3" expected="$4"
    shift 4
    if [ -n "$FILTER" ] && [[ "$name" != *"$FILTER"* ]]; then
        return
    fi
    CASE_NAME="$name"
    local actual
    actual="$(run_case "$ext" "$fixture" "$@"; echo x)"
    actual="${actual%x}"
    if [ "$actual" != "$expected" ]; then
        # NOTE: Retry once; an occasional key gets lost while the window is starting.
        actual="$(run_case "$ext" "$fixture" "$@"; echo x)"
        actual="${actual%x}"
    fi
    if [ "$actual" = "$expected" ]; then
        PASSED=$((PASSED + 1))
        echo "pass  $name"
    else
        FAILED=$((FAILED + 1))
        FAILED_NAMES="$FAILED_NAMES $name"
        echo "FAIL  $name"
        diff <(printf '%s' "$expected") <(printf '%s' "$actual") | sed 's/^/      /'
    fi
}

L1='alpha beta gamma'
L2='    indented line two'
L3='three (a, b) "str"'
L4='four 42 end'

# Basic editing
check "x"                    txt "$TXT" $'lpha beta gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:x
check "xp swaps"             txt "$TXT" $'lapha beta gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:xp
check "dd"                   txt "$TXT" "$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:dd
check "G dd last line"       txt "$TXT" "$L1"$'\n'"$L2"$'\n'"$L3"$'\n'  t:G t:dd
check "ddp swaps lines"      txt "$TXT" "$L2"$'\n'"$L1"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:ddp
check "dw"                   txt "$TXT" $'beta gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:dw
check "cw"                   txt "$TXT" $'alpha XX gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:w t:cwXX
check "cw on last char"      txt "$TXT" $'alphZ beta gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:e t:cwZ
check "ge"                   txt "$TXT" $'alpha bet gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:ww t:ge t:x
check "Y is yy"             txt "$TXT" "$L1"$'\n'"$L1"$'\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:w t:Y t:'$' t:p
check "K is E"               txt "$TXT" $'alph beta gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:K t:x
check "J"                    txt "$TXT" $'alpha beta gamma indented line two\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:J
check "gJ"                   txt "$TXT" "$L1$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:gJ
check "R replace"            txt "$TXT" $'XYpha beta gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:RXY
check "gUiw"                 txt "$TXT" $'ALPHA beta gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:gUiw
check "cc keeps indent"      txt "$TXT" "$L1"$'\n    NEW\n'"$L3"$'\n'"$L4"$'\n'  t:2G t:ccNEW
check "o esc clears indent"  txt "$TXT" "$L1"$'\n'"$L2"$'\n\n'"$L3"$'\n'"$L4"$'\n'  t:2G t:o
check "u undo"               txt "$TXT" "$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:dd t:dd t:u
check "ctrl-a"               txt "$TXT" "$L1"$'\n'"$L2"$'\n'"$L3"$'\n'$'four 43 end\n'  t:G k:ctrl+a
check "count ctrl-a"         txt "$TXT" "$L1"$'\n'"$L2"$'\n'"$L3"$'\n'$'four 47 end\n'  t:G t:5 k:ctrl+a
check "insert ctrl-w"        txt "$TXT" $'alpha beta \n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:A k:ctrl+w

# Text objects
check "ci("                  txt "$TXT" "$L1"$'\n'"$L2"$'\nthree (X) "str"\n'"$L4"$'\n'  t:3G "t:f(" "t:ci(X"
check "di\""                 txt "$TXT" "$L1"$'\n'"$L2"$'\nthree (a, b) ""\n'"$L4"$'\n'  t:3G 't:di"'
check "viwd"                 txt "$TXT" $'alpha  gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:w t:viwd
check "yip P"                txt "$TXT" "$L1"$'\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'"$L1"$'\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:yip t:P

# Repeat
check "dot dw"               txt "$TXT" $'gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:dw t:.
check "dot cw"               txt "$TXT" $'ONE ONE gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:cwONE k:Escape t:w t:.
check "dot count"            txt "$TXT" $'a beta gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:x t:3.
check "dot A"                txt "$TXT" $'alpha beta gamma!\n'"$L2"$'!\n'"$L3"$'\n'"$L4"$'\n'  t:gg 't:A!' k:Escape t:j t:.
check "dot o"                txt "$TXT" "$L1"$'\nNEW\nNEW\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:oNEW k:Escape t:.
check "insert count"         txt "$TXT" $'abababalpha beta gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:3iab

# Registers, marks, macros
check "named register"       txt "$TXT" "$L1"$'\n'"$L2"$'\n'"$L1"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg 't:"ayy' t:j 't:"ap'
check "black hole register"  txt "$TXT" "$L1"$'\n'"$L3"$'\n'"$L1"$'\n'"$L4"$'\n'  t:gg t:yy t:j 't:"_dd' t:p
check "yank register 0"      txt "$TXT" "$L1"$'\nalpha three (a, b) "str"\n'"$L4"$'\n'  t:gg t:yw t:j t:dd 't:"0P'
check "YY clipboard"         txt "$TXT" "$L1"$'\n'"$L2"$'\n'"$L1"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:YY t:j 't:"+p'
check "visual Y clipboard"   txt "$TXT" "$L1"$'\nalpha'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:viwY t:j 't:"+P'
check "visual p"             txt "$TXT" $'alpha alpha gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:yiw t:w t:viwp
check "marks"                txt "$TXT" "$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:ma t:G "t:'a" t:dd
check "macro"                txt "$TXT" "$L1"$';\n'"$L2"$';\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:qa 't:A;' k:Escape t:j t:q t:@a
check "macro count"          txt "$TXT" "$L1"$';\n'"$L2"$';\n'"$L3"$';\n'"$L4"$'\n'  t:gg t:qa 't:A;' k:Escape t:j t:q t:2@a

# Search and ex
check "search"               txt "$TXT" "$L1"$'\n    indented \n'"$L3"$'\n'"$L4"$'\n'  t:gg t:/line k:Return t:D
check "smartcase"            txt "$TXT" $'lpha beta gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:/BETA k:Return t:x
check "search n"             txt "$TXT" $'alph beta gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:/a k:Return t:x
check "search ctrl-w"        txt "$TXT" $'alpha eta gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg "t:/beta zzz" k:ctrl+w k:Return t:x
check "ex ctrl-u"            txt "$TXT" $'Xlpha beta gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg "t::foo bar" k:ctrl+u t:s/a/X/ k:Return
check ":s first only"        txt "$TXT" $'Xlpha beta gamma\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t::s/a/X/ k:Return
check ":%s global"           txt "$TXT" $'XlphX betX gXmmX\n'"$L2"$'\nthree (X, b) "str"\n'"$L4"$'\n'  t::%s/a/X/g k:Return
check ":2,3d"                txt "$TXT" "$L1"$'\n'"$L4"$'\n'  t::2,3d k:Return
check "visual :s"            txt "$TXT" $'Xlpha beta gamma\n'"$L2"$'\nthree (X, b) "str"\n'"$L4"$'\n'  t:gg t:Vjj t::s/a/X/ k:Return

# Git signs
check "git ]h next hunk"     txt "$TXT" "$L1"$'\n'"$L2"$'\n'"$L3"$'\n'"$L4"$'\nEW\n'  t:G t:oNEW k:Escape t:gg t:]h t:x
check "git hunk reset"       txt "$TXT" "$TXT"  t:gg t:x t:j t:x t:k "t: ghr"

# Pickers
check ":Rg fuzzy jump"       txt "$TXT" "$L1"$'\n'"$L2"$'\n'"$L3"$'\n'  t::Rg\ e k:Return "t:fo end" k:Return t:dd

# Indentation
check "V j >"                txt "$TXT" "  $L1"$'\n'"  $L2"$'\n'"$L3"$'\n'"$L4"$'\n'  t:gg t:Vj t:'>'
check "java o indent"        java "$JAVA" $'public class Hello {\n    public static void main(String[] args) {\n    int y = 1;\n        System.out.println("hello world");\n    }\n}\n'  t:gg t:j "t:oint y = 1;"

echo
echo "$PASSED passed, $FAILED failed"
if [ $FAILED -gt 0 ]; then
    echo "failed:$FAILED_NAMES"
    exit 1
fi
