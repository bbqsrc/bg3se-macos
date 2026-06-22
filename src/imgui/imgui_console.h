/**
 * imgui_console.h - In-game ImGui Lua console (output log + input field)
 *
 * A self-contained ImGui console widget. Output is fed from console_send_output
 * (Ext.Print, errors, command results); submitted input is forwarded to a
 * callback (wired to console_queue_lua_command, which runs Lua on the game thread).
 *
 * C API so it can be driven from console.c / main.c (C) and rendered from the
 * Objective-C++ Metal backend.
 */

#ifndef IMGUI_CONSOLE_H
#define IMGUI_CONSOLE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Callback invoked when the user submits a line in the console.
typedef void (*imgui_console_submit_fn)(const char *command);

// Register the submit handler (e.g. console_queue_lua_command). NULL disables submit.
void imgui_console_set_submit_callback(imgui_console_submit_fn cb);

// Append a line to the output log. Thread-safe (callable from any thread).
void imgui_console_add_log(const char *text, bool is_error);

// Clear the output log.
void imgui_console_clear(void);

// Render the console window for this ImGui frame. *p_open is cleared if the user
// closes the window via its title-bar button. Call only between NewFrame/Render.
void imgui_console_draw(bool *p_open);

// Request that the input field grab keyboard focus on the next draw.
void imgui_console_focus_input(void);

#ifdef __cplusplus
}
#endif

#endif // IMGUI_CONSOLE_H
