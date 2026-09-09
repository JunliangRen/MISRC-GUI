/*
 * Exercise the production modal query and real generic-popup lifecycle.
 * Including the UI source gives access to its private toolbar-window state;
 * gui_popup.c is linked separately so no popup ownership logic is mocked.
 * ci_guard_tests.py verifies the main loop uses both frame/current ownership.
 */
#include <stdio.h>

#include "../misrc_gui/ui/gui_ui.c"

static int checks;
static int failures;

static void expect_true(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void)
{
    static gui_app_t app;
    expect_true(!gui_ui_modal_is_open(&app), "the scene owns input with no modal");
    expect_true(!gui_ui_modal_is_open(NULL), "an absent app has no Settings modal");

    bool *windows[] = {
        &app.settings_panel_open,
        &s_record_limit_window_open,
        &s_version_info_window_open,
        &s_metadata_window_open,
    };
    const char *open_messages[] = {
        "Settings owns input even without a mouse click",
        "record timer owns input even without Settings",
        "version information owns input even without Settings",
        "metadata owns input even without Settings",
    };
    for (size_t i = 0; i < sizeof(windows) / sizeof(windows[0]); i++) {
        *windows[i] = true;
        expect_true(gui_ui_modal_is_open(&app), open_messages[i]);
        // Click consumption is transient; modal wheel ownership is not.
        s_ui_consumed_click = false;
        expect_true(gui_ui_modal_is_open(&app), "a frame without clicks retains modal ownership");
        *windows[i] = false;
        expect_true(!gui_ui_modal_is_open(&app), "closing a toolbar modal releases current ownership");
    }

    s_ui_consumed_click = true;
    expect_true(!gui_ui_modal_is_open(&app), "a consumed nonmodal click does not invent modal ownership");
    s_ui_consumed_click = false;

    gui_popup_info("Test", "Information");
    expect_true(gui_ui_modal_is_open(&app), "a real informational popup owns scene input");
    expect_true(gui_ui_modal_is_open(NULL), "generic popup ownership does not depend on app settings");
    gui_popup_dismiss();
    expect_true(!gui_ui_modal_is_open(&app), "dismissed information no longer owns current input");

    gui_popup_confirm("Test", "Confirm", "Yes", "No", &app);
    expect_true(gui_ui_modal_is_open(&app), "a confirmation opened after dismissal owns input again");
    expect_true(gui_popup_get_result() == POPUP_RESULT_NONE,
                "a new confirmation clears the previous dismissal result");
    app.settings_panel_open = true;
    gui_popup_dismiss();
    expect_true(gui_ui_modal_is_open(&app), "closing a nested confirmation leaves Settings ownership");
    app.settings_panel_open = false;
    expect_true(!gui_ui_modal_is_open(&app), "closing the last modal returns ownership to the scene");

    s_version_info_window_open = true;
    s_metadata_window_open = true;
    s_version_info_window_open = false;
    expect_true(gui_ui_modal_is_open(&app), "closing one toolbar window cannot release another's ownership");
    s_metadata_window_open = false;
    expect_true(!gui_ui_modal_is_open(&app), "all toolbar windows closed releases input");

    printf("Modal input: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
