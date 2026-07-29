#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int WINAPI WinMain(
    HINSTANCE instance,
    HINSTANCE previous,
    LPSTR command_line,
    int show_command
) {
    (void)instance;
    (void)previous;
    (void)command_line;
    (void)show_command;

    MessageBoxA(
        NULL,
        "The XboxWine x86 engine launched a 32-bit Windows program.",
        "XboxWine x86 smoke test",
        MB_OK | MB_ICONINFORMATION
    );

    return 0;
}
