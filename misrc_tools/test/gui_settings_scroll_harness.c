/* Production scroll helpers with the real Clay layout/scroll lifecycle. */
#define CLAY_IMPLEMENTATION
#include <stdio.h>
#include <stdlib.h>
#include "../misrc_gui/ui/gui_ui.c"

static int checks;
static int failures;
static gui_app_t app;

static void expect_true(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void layout(bool extra, int before, int after, bool show_anchor)
{
    Clay_BeginLayout();
    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(500), CLAY_SIZING_FIXED(400) }, .layoutDirection = CLAY_TOP_TO_BOTTOM }
    }) {
        if (extra) {
            CLAY(CLAY_ID("ChangingStatus"), { .layout = { .sizing = { CLAY_SIZING_FIXED(10), CLAY_SIZING_FIXED(10) } } }) {}
        }
        if (app.settings_panel_open) {
            CLAY(CLAY_ID("SettingsScroll"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(400), CLAY_SIZING_FIXED(200) }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
                .clip = { .vertical = true, .childOffset = s_settings_scroll_offset }
            }) {
                CLAY(CLAY_ID("Before"), { .layout = { .sizing = { CLAY_SIZING_FIXED(400), CLAY_SIZING_FIXED(before) } } }) {}
                if (show_anchor) {
                    CLAY(CLAY_ID("RtlsdrOutputBox"), { .layout = { .sizing = { CLAY_SIZING_FIXED(200), CLAY_SIZING_FIXED(28) } } }) {}
                }
                CLAY(CLAY_ID("After"), { .layout = { .sizing = { CLAY_SIZING_FIXED(400), CLAY_SIZING_FIXED(after) } } }) {}
            }
        }
    }
    Clay_EndLayout();
}

static float anchor_y(void)
{
    return Clay_GetElementData(CLAY_ID("RtlsdrOutputBox")).boundingBox.y;
}

static float scroll_y(void)
{
    Clay_ScrollContainerData data = Clay_GetScrollContainerData(CLAY_ID("SettingsScroll"));
    return data.found ? data.scrollPosition->y : 0;
}

static bool near(float a, float b) { return fabsf(a - b) < 0.01f; }

static void frame(bool extra, int before, int after, bool show_anchor, bool held)
{
    Clay_SetPointerState((Clay_Vector2){20, 100}, held);
    Clay_UpdateScrollContainers(!gui_ui_settings_scroll_is_anchoring(), (Clay_Vector2){0}, 0.016f);
    gui_ui_prepare_settings_scroll(&app);
    layout(extra, before, after, show_anchor);
    if (gui_ui_restore_settings_scroll(&app)) {
        layout(extra, before, after, show_anchor);
        Clay_SetPointerState((Clay_Vector2){20, 100}, held);
    }
}

int main(void)
{
    uint32_t size = Clay_MinMemorySize();
    void *memory = malloc(size);
    if (!memory) return 1;
    Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(size, memory),
                    (Clay_Dimensions){500, 400}, (Clay_ErrorHandler){0});
    app.settings_panel_open = true;
    frame(false, 250, 222, true, false);
    Clay_ScrollContainerData data = Clay_GetScrollContainerData(CLAY_ID("SettingsScroll"));
    *data.scrollPosition = (Clay_Vector2){0, -150};
    for (int i = 0; i < 8; i++) {
        bool extra = i % 2;
        frame(extra, 250, 222, true, false);
        expect_true(near(scroll_y(), -150), "preceding element changes preserve stored scroll");
        expect_true(near(anchor_y(), extra ? 110 : 100), "preceding element changes never draw Settings at the top");
    }

    // Remove/add rows above the clicked button while keeping its screen anchor.
    float old_y = anchor_y();
    gui_ui_anchor_settings_scroll();
    expect_true(gui_ui_settings_scroll_is_anchoring(), "a scrolled mode switch requests anchoring");
    frame(false, 200, 280, true, true);
    expect_true(near(anchor_y(), old_y), "mode change preserves the clicked button's screen position");
    expect_true(!gui_ui_settings_scroll_is_anchoring(), "the anchor resolves in one frame");
    float anchored_scroll = scroll_y();
    frame(false, 200, 280, true, true);
    frame(false, 200, 280, true, false);
    expect_true(near(scroll_y(), anchored_scroll), "held/released click cannot restore the pre-anchor drag offset");
    for (int i = 0; i < 6; i++) {
        gui_ui_anchor_settings_scroll();
        frame(i % 2, i % 2 ? 250 : 200, 280, true, false);
        expect_true(near(anchor_y(), old_y), "repeated mode switches do not accumulate drift");
    }

    gui_ui_anchor_settings_scroll();
    frame(false, 30, 190, true, false);
    expect_true(near(scroll_y(), 0), "an anchor above the scrollable range clamps to the top");
    gui_ui_anchor_settings_scroll();
    expect_true(!gui_ui_settings_scroll_is_anchoring(), "an unscrolled page stays unscrolled on mode changes");

    frame(false, 250, 400, true, false);
    data = Clay_GetScrollContainerData(CLAY_ID("SettingsScroll"));
    data.scrollPosition->y = -200;
    frame(false, 250, 400, true, false);
    gui_ui_anchor_settings_scroll();
    frame(false, 250, 0, true, false);
    expect_true(near(scroll_y(), -78), "shorter content clamps to the nearest valid bottom offset");
    gui_ui_anchor_settings_scroll();
    frame(false, 30, 40, true, false);
    expect_true(near(scroll_y(), 0), "nonoverflowing content cannot keep an invalid scroll offset");

    frame(false, 250, 400, true, false);
    data = Clay_GetScrollContainerData(CLAY_ID("SettingsScroll"));
    data.scrollPosition->y = -150;
    frame(false, 250, 400, true, false);
    gui_ui_anchor_settings_scroll();
    frame(false, 250, 400, false, false);
    expect_true(!gui_ui_settings_scroll_is_anchoring(), "a removed anchor cancels restoration");
    frame(false, 250, 400, true, false);
    gui_ui_anchor_settings_scroll();
    app.settings_panel_open = false;
    frame(false, 250, 400, true, false);
    expect_true(!gui_ui_settings_scroll_is_anchoring(), "closing Settings cancels a pending anchor");
    app.settings_panel_open = true;
    frame(false, 250, 400, true, false);
    expect_true(near(scroll_y(), 0), "reopening Settings does not reuse a stale anchor");

    free(memory);
    printf("Settings scroll: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
