#include "note_app.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    raster::NoteApp app;
    return app.Run(instance, show_command);
}
