#define MAJOR 4
#define MINOR 1
#define PATCH 8

// NOTE: The name the window title, the macOS menu and app bundle, and the Linux desktop
// entry show.
#define APP_NAME "My Editor"
#define L_APP_NAME L"My Editor"

// string
#define VN__(a,b,c) #a "." #b "." #c
#define VN_(a,b,c) VN__(a,b,c)
#define VERSION_NUMBER VN_(MAJOR,MINOR,PATCH)
#define VERSION_STRING VERSION_NUMBER

#define ST__(s) #s
#define ST_(s) ST__(s)
#define MAJOR_STR ST_(MAJOR)
#define MINOR_STR ST_(MINOR)
#define PATCH_STR ST_(PATCH)

#define VERSION_TYPE

#define VERSION VERSION_STRING VERSION_TYPE

#define WINDOW_NAME APP_NAME

// long string
#define L_VN__(a,b,c) L#a L"." L#b L"." L#c
#define L_VN_(a,b,c) L_VN__(a,b,c)
#define L_VERSION_NUMBER L_VN_(MAJOR,MINOR,PATCH)
#define L_VERSION_STRING L_VERSION_NUMBER

#define L_VERSION_TYPE

#define L_VERSION L_VERSION_STRING L_VERSION_TYPE

#define L_WINDOW_NAME L_APP_NAME
