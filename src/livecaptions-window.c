/* livecaptions-window.c
 *
 * Copyright 2022 abb128
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "livecaptions-config.h"
#include "livecaptions-window.h"
#include "livecaptions-application.h"
#include "audiocap.h"
#include "window-helper.h"
#include "history.h"


G_DEFINE_TYPE(LiveCaptionsWindow, livecaptions_window, GTK_TYPE_APPLICATION_WINDOW)

static void livecaptions_window_class_init (LiveCaptionsWindowClass *klass) {
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(widget_class, "/net/sapples/LiveCaptions/livecaptions-window.ui");
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, main);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, side_box);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, side_box_tiny);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, mic_button);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, translation_switch);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, label);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, translation_label);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, too_slow_warning);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, slow_warning);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, slowest_warning);
}

static void change_button_layout(LiveCaptionsWindow *self, gint text_height){
    int button_height = 29;

    if(text_height > (2 * button_height)) {
        gtk_widget_set_visible(GTK_WIDGET(self->side_box), true);
        gtk_widget_set_visible(GTK_WIDGET(self->side_box_tiny), false);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->side_box_tiny), true);
        gtk_widget_set_visible(GTK_WIDGET(self->side_box), false);
    }
}

const char LINE_WIDTH_TEXT_TEMPLATE[] = "This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.";

static int get_max_caption_width(LiveCaptionsWindow *self) {
    GtkWidget *widget = GTK_WIDGET(self);
    GdkDisplay *display = gtk_widget_get_display(widget);
    GtkNative *native = gtk_widget_get_native(widget);
    GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
    GdkMonitor *monitor = surface ? gdk_display_get_monitor_at_surface(display, surface) : NULL;
    gboolean owns_monitor = FALSE;

    if(monitor == NULL) {
        GListModel *monitors = gdk_display_get_monitors(display);
        if(g_list_model_get_n_items(monitors) > 0) {
            monitor = g_list_model_get_item(monitors, 0);
            owns_monitor = TRUE;
        }
    }
    if(monitor == NULL) return G_MAXINT;

    GdkRectangle workarea;
    gdk_monitor_get_geometry(monitor, &workarea);
    if(owns_monitor) g_object_unref(monitor);

    /* The caption label is only one part of the window.  Derive the space
     * used by the active controls and label margins instead of subtracting a
     * fixed pixel allowance.  The complete window can therefore grow right
     * up to, but never beyond, the current monitor work area. */
    GtkWidget *controls = gtk_widget_get_visible(GTK_WIDGET(self->side_box))
        ? GTK_WIDGET(self->side_box)
        : GTK_WIDGET(self->side_box_tiny);
    int controls_min = 0;
    int controls_natural = 0;
    gtk_widget_measure(controls, GTK_ORIENTATION_HORIZONTAL, -1,
                       &controls_min, &controls_natural, NULL, NULL);

    int label_margins = gtk_widget_get_margin_start(GTK_WIDGET(self->label)) +
                        gtk_widget_get_margin_end(GTK_WIDGET(self->label));
    return MAX(1, workarea.width - controls_natural - label_margins);
}

static void update_line_width(LiveCaptionsWindow *self){
    if(self->font_layout != NULL) g_object_unref(self->font_layout);

    int preferred_width = g_settings_get_int(self->settings, "line-width");
    size_t text_len = sizeof(LINE_WIDTH_TEXT_TEMPLATE);
    if(preferred_width < text_len) text_len = preferred_width;

    int width, height;
    PangoLayout *layout = gtk_label_get_layout(self->label);
    layout = pango_layout_copy(layout);

    pango_layout_set_width(layout, -1);
    pango_layout_set_text(layout, LINE_WIDTH_TEXT_TEMPLATE, text_len);
    pango_layout_get_size(layout, &width, &height);

    /* Reserve two complete lines plus ascent/descent safety room.  The label
     * margins are outside its allocation, so a tiny fixed request clips the
     * second line on some fonts and scale factors. */
    height = (height / PANGO_SCALE) * 2 + 16;
    width  = MIN(width / PANGO_SCALE, get_max_caption_width(self));

    int translation_height;
    PangoLayout *translation_layout = gtk_label_get_layout(self->translation_label);
    translation_layout = pango_layout_copy(translation_layout);
    pango_layout_set_width(translation_layout, -1);
    pango_layout_set_text(translation_layout, LINE_WIDTH_TEXT_TEMPLATE, text_len);
    pango_layout_get_size(translation_layout, NULL, &translation_height);
    translation_height = (translation_height / PANGO_SCALE) * 2 + 16;
    g_object_unref(translation_layout);

    gboolean translation_enabled = g_settings_get_boolean(self->settings, "translation-enabled");
    change_button_layout(self, translation_enabled ? height + translation_height : height);

    gtk_widget_set_size_request(GTK_WIDGET(self->label), width, height);
    gtk_widget_set_size_request(GTK_WIDGET(self->translation_label), width, translation_height);

    self->max_text_width = width;
    self->font_layout = layout;
    self->font_layout_counter++;
}

static void on_caption_surface_enter_monitor(G_GNUC_UNUSED GdkSurface *surface,
                                             G_GNUC_UNUSED GdkMonitor *monitor,
                                             LiveCaptionsWindow *self) {
    update_line_width(self);
}

static void on_caption_window_realize(GtkWidget *widget,
                                      G_GNUC_UNUSED gpointer user_data) {
    LiveCaptionsWindow *self = LIVECAPTIONS_WINDOW(widget);
    GtkNative *native = gtk_widget_get_native(widget);
    GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
    if(surface != NULL) {
        g_signal_connect_object(surface, "enter-monitor",
                                G_CALLBACK(on_caption_surface_enter_monitor),
                                self, 0);
    }
    update_line_width(self);
}

static void update_font(LiveCaptionsWindow *self) {
    g_autofree char *font_name = g_settings_get_string(self->settings, "font-name");
    g_autofree char *translation_font_name = g_settings_get_string(self->settings, "translation-font-name");
    PangoFontDescription *desc = pango_font_description_from_string(font_name);
    PangoFontDescription *translation_desc = pango_font_description_from_string(translation_font_name);

    PangoAttribute *attr_font = pango_attr_font_desc_new(desc);

    PangoAttrList *attr = gtk_label_get_attributes(self->label);
    if(attr == NULL){
        attr = pango_attr_list_new();
    }
    pango_attr_list_change(attr, attr_font);

    gtk_label_set_attributes(self->label, attr);

    PangoAttrList *translation_attr = pango_attr_list_new();
    pango_attr_list_change(translation_attr, pango_attr_font_desc_new(translation_desc));
    gtk_label_set_attributes(self->translation_label, translation_attr);
    pango_attr_list_unref(translation_attr);

    pango_font_description_free(desc);
    pango_font_description_free(translation_desc);

    update_line_width(self);
}

static void update_window_transparency(LiveCaptionsWindow *self) {
    bool use_transparency = g_settings_get_double(self->settings, "window-transparency") > 0.01;

    if(use_transparency){
        gtk_widget_add_css_class(GTK_WIDGET(self), "transparent-mode");

        int transparency = (int)((1.0 - g_settings_get_double(self->settings, "window-transparency")) * 255.0);

        char css_data[256];
        snprintf(css_data, 256, ".transparent-mode {\nbackground-color: #000000%02X;\n}", transparency);
        gtk_css_provider_load_from_data(self->css_provider, css_data, -1);
    }else{
        gtk_widget_remove_css_class(GTK_WIDGET(self), "transparent-mode");
    }
}

static void update_keep_above(LiveCaptionsWindow *self) {
    gboolean keep_above = g_settings_get_boolean(self->settings, "keep-on-top");
    if(!set_window_keep_above(GTK_WINDOW(self), keep_above)){
        printf("Failed to set keep above\n");
    }
}

static void update_audio_mode_visual(GtkToggleButton *button,
                                     G_GNUC_UNUSED GParamSpec *pspec,
                                     G_GNUC_UNUSED gpointer user_data) {
    gboolean microphone = gtk_toggle_button_get_active(button);

    gtk_button_set_icon_name(
        GTK_BUTTON(button),
        microphone ? "audio-input-microphone-symbolic" : "audio-speakers-symbolic"
    );
    gtk_widget_set_tooltip_text(
        GTK_WIDGET(button),
        microphone ? "Microphone input mode" : "System audio input mode"
    );
}

static void on_settings_change(G_GNUC_UNUSED GSettings *settings,
                               char      *key,
                               gpointer   user_data){

    LiveCaptionsWindow *self = user_data;
    if(g_str_equal(key, "font-name") || g_str_equal(key, "translation-font-name")) {
        update_font(self);
    }else if(g_str_equal(key, "line-width")) {
        update_line_width(self);
    }else if(g_str_equal(key, "transparent-window") || g_str_equal(key, "window-transparency")) {
        update_window_transparency(self);
    }else if(g_str_equal(key, "keep-on-top")) {
        update_keep_above(self);
    }else if(g_str_equal(key, "translation-enabled")) {
        update_line_width(self);
    }
}


static gboolean deferred_update_keep_above(void *userdata) {
    LiveCaptionsWindow *self = userdata;

    update_keep_above(self);

    return G_SOURCE_REMOVE;
}

static gboolean show_relevant_slow_warning(void *userdata) {
    LiveCaptionsWindow *self = userdata;

    GtkApplication *curr_app = gtk_window_get_application(GTK_WINDOW(self));
    LiveCaptionsApplication *app = LIVECAPTIONS_APPLICATION(curr_app);

    asr_thread asr = app->asr;

    if(asr_thread_is_errored(asr)){
        gtk_label_set_text(self->label, "[Model Error]");
        self->was_errored = true;
        return G_SOURCE_CONTINUE;
    }else if(self->was_errored) {
        self->was_errored = false;
        gtk_label_set_text(self->label, "");
    }

    gtk_widget_set_visible(GTK_WIDGET(self->slow_warning), false);
    gtk_widget_set_visible(GTK_WIDGET(self->slowest_warning), false);
    gtk_widget_set_visible(GTK_WIDGET(self->too_slow_warning), false);

    return G_SOURCE_CONTINUE;
}

static void livecaptions_window_init(LiveCaptionsWindow *self) {
    gtk_widget_init_template(GTK_WIDGET(self));
    g_signal_connect(self, "realize", G_CALLBACK(on_caption_window_realize), NULL);

    self->settings = g_settings_new("net.sapples.LiveCaptions.Nemotron");

    self->css_provider = gtk_css_provider_new();
    GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(self));
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(self->css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    g_signal_connect(self->settings, "changed", G_CALLBACK(on_settings_change), self);
    
    g_settings_bind(self->settings, "microphone", self->mic_button, "active", G_SETTINGS_BIND_DEFAULT);
    g_signal_connect(self->mic_button, "notify::active",
                     G_CALLBACK(update_audio_mode_visual), self);
    update_audio_mode_visual(self->mic_button, NULL, self);
    g_settings_bind(self->settings, "translation-enabled", self->translation_switch, "active", G_SETTINGS_BIND_DEFAULT);
    g_settings_bind(self->settings, "translation-enabled", self->translation_label, "visible", G_SETTINGS_BIND_DEFAULT);

    self->font_layout = NULL;
    self->font_layout_counter = 0;

    update_font(self);
    update_window_transparency(self);

    self->slow_warning_shown = false;
    self->was_errored = false;

    g_idle_add(deferred_update_keep_above, self);
    g_timeout_add_seconds(5, show_relevant_slow_warning, self);

    // GTK adds solid-csd class when the compositor does not support window shadows
    // This adds a very ugly thick border, so we remove it
    gtk_widget_remove_css_class(GTK_WIDGET(self), "solid-csd");
}

static gboolean hide_slow_warning_after_some_time(void *userdata) {
    LiveCaptionsWindow *self = userdata;

    time_t current_time = time(NULL);

    if(difftime(current_time, self->slow_time) > 4.0) {
        self->slow_warning_shown = false;
        gtk_widget_set_visible(GTK_WIDGET(self->too_slow_warning), false);
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

void livecaptions_window_warn_slow(LiveCaptionsWindow *self) {
    self->slow_time = time(NULL);
    if(self->slow_warning_shown) return;

    gtk_widget_set_visible(GTK_WIDGET(self->too_slow_warning), true);
    self->slow_warning_shown = true;

    g_timeout_add_seconds(1, hide_slow_warning_after_some_time, self);
}
