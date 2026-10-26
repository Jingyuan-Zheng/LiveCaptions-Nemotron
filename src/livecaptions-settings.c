/* LiveCaptions preferences with installer-managed Nemotron model. */
#include <glib/gi18n.h>

#include "common.h"
#include "history.h"
#include "livecaptions-config.h"
#include "livecaptions-history-window.h"
#include "livecaptions-settings.h"
#include "window-helper.h"

G_DEFINE_TYPE(LiveCaptionsSettings, livecaptions_settings, ADW_TYPE_PREFERENCES_WINDOW)

static void rerun_benchmark_cb(LiveCaptionsSettings *self) {
    gtk_window_close(GTK_WINDOW(self));
    gtk_widget_activate_action(GTK_WIDGET(self), "app.setup", NULL);
    gtk_window_destroy(GTK_WINDOW(self));
}

static void about_cb(LiveCaptionsSettings *self) {
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    const char *developers[] = {
        "abb128 (original LiveCaptions)",
        "Jingyuan-Zheng (Nemotron fork)",
        NULL
    };
    GtkWidget *about = g_object_new(
        ADW_TYPE_ABOUT_WINDOW,
        "transient-for", root,
        "application-icon", "net.sapples.LiveCaptions",
        "application-name", _("Live Captions"),
        "developer-name", _("LiveCaptions contributors"),
        "version", LIVECAPTIONS_VERSION,
        "website", "https://github.com/Jingyuan-Zheng/LiveCaptions-Nemotron",
        "issue-url", "https://github.com/Jingyuan-Zheng/LiveCaptions-Nemotron/issues",
        "copyright", "Copyright © 2022 abb128; modifications © 2026 contributors",
        "license-type", GTK_LICENSE_GPL_3_0,
        "developers", developers,
        "translator-credits", _("translator-credits"),
        NULL);

    adw_about_window_add_legal_section(ADW_ABOUT_WINDOW(about),
                                       "NeMo-Speech.cpp",
                                       "Copyright © NVIDIA Corporation",
                                       GTK_LICENSE_APACHE_2_0,
                                       NULL);
    gtk_window_present(GTK_WINDOW(about));
}

static void report_cb(LiveCaptionsSettings *self) {
    gtk_show_uri(GTK_WINDOW(self),
                 "https://github.com/Jingyuan-Zheng/LiveCaptions-Nemotron/issues",
                 GDK_CURRENT_TIME);
}

static void open_history(LiveCaptionsSettings *self) {
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    LiveCaptionsHistoryWindow *window = g_object_new(
        LIVECAPTIONS_TYPE_HISTORY_WINDOW, "transient-for", root, NULL);
    gtk_window_present(GTK_WINDOW(window));
    gtk_window_close(GTK_WINDOW(self));
    gtk_window_destroy(GTK_WINDOW(self));
}

static void translation_model_changed(GtkDropDown *dropdown,
                                      GParamSpec *pspec,
                                      LiveCaptionsSettings *self) {
    (void)pspec;
    guint selected = gtk_drop_down_get_selected(dropdown);
    g_settings_set_string(self->settings, "translation-model", selected == 0
        ? "kaelri/hy-mt2:1.8b-q4_K_M"
        : "translategemma:4b");
}

static const char *get_always_on_top_tip_text(void) {
    const char *desktop = getenv("XDG_CURRENT_DESKTOP");
    if(desktop == NULL) return NULL;
    if(strstr(desktop, "GNOME"))
        return _("Right-click the captions window and enable \"Always on Top\" to keep the captions on top.");
    if(strstr(desktop, "KDE"))
        return _("Right-click the captions window (or hit ALT+F3) and enable \"More Actions\" -> \"Keep Above Others\" to keep the captions on top.");
    return NULL;
}

static void livecaptions_settings_class_init(LiveCaptionsSettingsClass *klass) {
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    gtk_widget_class_set_template_from_resource(
        widget_class, "/net/sapples/LiveCaptions/livecaptions-settings.ui");

    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, font_button);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, font_button_ar);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, translation_font_button);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, translation_font_button_ar);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, text_upper_switch);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, fade_text_switch);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, filter_profanity_switch);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, filter_slurs_switch);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, save_history_switch);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, keep_above_switch);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, text_stream_switch);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, translation_settings_switch);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, translation_language_entry);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, translation_model_entry);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, translation_model_dropdown);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, line_width_scale);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, line_width_adjustment);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, window_transparency_scale);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, window_transparency_adjustment);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, benchmark_label);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsSettings, keep_above_instructions);

    gtk_widget_class_bind_template_callback(widget_class, report_cb);
    gtk_widget_class_bind_template_callback(widget_class, about_cb);
    gtk_widget_class_bind_template_callback(widget_class, rerun_benchmark_cb);
    gtk_widget_class_bind_template_callback(widget_class, open_history);
}

static gboolean deferred_update_keep_above(gpointer userdata) {
    LiveCaptionsSettings *self = userdata;
    set_window_keep_above(GTK_WINDOW(self),
                          g_settings_get_boolean(self->settings, "keep-on-top"));
    return G_SOURCE_REMOVE;
}

static int get_settings_monitor_width(GtkWidget *widget) {
    GdkDisplay *display = gtk_widget_get_display(widget);
    GtkNative *native = gtk_widget_get_native(widget);
    GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
    GdkMonitor *monitor = surface
        ? gdk_display_get_monitor_at_surface(display, surface)
        : NULL;
    gboolean owns_monitor = FALSE;

    if(monitor == NULL) {
        GListModel *monitors = gdk_display_get_monitors(display);
        if(g_list_model_get_n_items(monitors) > 0) {
            monitor = g_list_model_get_item(monitors, 0);
            owns_monitor = TRUE;
        }
    }
    if(monitor == NULL) return 0;

    GdkRectangle workarea;
    gdk_monitor_get_geometry(monitor, &workarea);
    if(owns_monitor) g_object_unref(monitor);
    return workarea.width;
}

static void update_line_width_limit(LiveCaptionsSettings *self) {
    int monitor_width = get_settings_monitor_width(GTK_WIDGET(self));
    if(monitor_width <= 0) return;

    g_autofree char *font_name = g_settings_get_string(self->settings, "font-name");
    PangoFontDescription *font = pango_font_description_from_string(font_name);
    PangoContext *context = gtk_widget_get_pango_context(GTK_WIDGET(self));
    PangoFontMetrics *metrics = pango_context_get_metrics(context, font, NULL);
    int average_char_width = pango_font_metrics_get_approximate_char_width(metrics);

    /* line-width is stored as an approximate character count.  Convert the
     * current monitor width using the selected caption font, so the slider's
     * end always means the physical edge of the monitor rather than an
     * arbitrary compile-time value. */
    int maximum_chars = average_char_width > 0
        ? (monitor_width * PANGO_SCALE + average_char_width - 1) / average_char_width
        : 20;
    gtk_adjustment_set_lower(self->line_width_adjustment, 20.0);
    gtk_adjustment_set_upper(self->line_width_adjustment,
                             MAX(20, maximum_chars));

    pango_font_metrics_unref(metrics);
    pango_font_description_free(font);
}

static void settings_value_changed(G_GNUC_UNUSED GSettings *settings,
                                   char *key,
                                   LiveCaptionsSettings *self) {
    if(g_str_equal(key, "font-name"))
        update_line_width_limit(self);
}

static void on_settings_surface_enter_monitor(G_GNUC_UNUSED GdkSurface *surface,
                                              G_GNUC_UNUSED GdkMonitor *monitor,
                                              LiveCaptionsSettings *self) {
    update_line_width_limit(self);
}

static void on_settings_realize(GtkWidget *widget,
                                G_GNUC_UNUSED gpointer user_data) {
    LiveCaptionsSettings *self = LIVECAPTIONS_SETTINGS(widget);
    GtkNative *native = gtk_widget_get_native(widget);
    GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
    if(surface != NULL) {
        g_signal_connect_object(surface, "enter-monitor",
                                G_CALLBACK(on_settings_surface_enter_monitor),
                                self, 0);
    }
    update_line_width_limit(self);
}

static void livecaptions_settings_init(LiveCaptionsSettings *self) {
    gtk_widget_init_template(GTK_WIDGET(self));
    g_signal_connect(self, "realize", G_CALLBACK(on_settings_realize), NULL);
    adw_action_row_set_activatable_widget(self->font_button_ar,
                                          GTK_WIDGET(self->font_button));
    adw_action_row_set_activatable_widget(self->translation_font_button_ar,
                                          GTK_WIDGET(self->translation_font_button));
    self->settings = g_settings_new("net.sapples.LiveCaptions.Nemotron");
    g_signal_connect(self->settings, "changed",
                     G_CALLBACK(settings_value_changed), self);
    update_line_width_limit(self);

    g_settings_bind(self->settings, "text-uppercase", self->text_upper_switch, "active", G_SETTINGS_BIND_DEFAULT);
    g_settings_bind(self->settings, "fade-text", self->fade_text_switch, "active", G_SETTINGS_BIND_DEFAULT);
    g_settings_bind(self->settings, "filter-profanity", self->filter_profanity_switch, "active", G_SETTINGS_BIND_DEFAULT);
    g_settings_bind(self->settings, "filter-slurs", self->filter_slurs_switch, "active", G_SETTINGS_BIND_DEFAULT);
    g_settings_bind(self->settings, "save-history", self->save_history_switch, "active", G_SETTINGS_BIND_DEFAULT);
    g_settings_bind(self->settings, "line-width", self->line_width_adjustment, "value", G_SETTINGS_BIND_DEFAULT);
    g_settings_bind(self->settings, "window-transparency", self->window_transparency_adjustment, "value", G_SETTINGS_BIND_DEFAULT);
    g_settings_bind(self->settings, "text-stream-active", self->text_stream_switch, "active", G_SETTINGS_BIND_DEFAULT);
    g_settings_bind(self->settings, "translation-enabled", self->translation_settings_switch, "active", G_SETTINGS_BIND_DEFAULT);
    g_settings_bind(self->settings, "translation-target-language", self->translation_language_entry, "text", G_SETTINGS_BIND_DEFAULT);
    g_settings_bind(self->settings, "translation-model", self->translation_model_entry, "text", G_SETTINGS_BIND_DEFAULT);
    g_settings_bind(self->settings, "font-name", self->font_button, "font", G_SETTINGS_BIND_DEFAULT);
    g_settings_bind(self->settings, "translation-font-name", self->translation_font_button, "font", G_SETTINGS_BIND_DEFAULT);
    g_autofree char *translation_model = g_settings_get_string(self->settings, "translation-model");
    gtk_drop_down_set_selected(self->translation_model_dropdown,
        g_strcmp0(translation_model, "translategemma:4b") == 0 ? 1 : 0);
    g_signal_connect(self->translation_model_dropdown, "notify::selected",
                     G_CALLBACK(translation_model_changed), self);

    gtk_scale_add_mark(self->line_width_scale, 50.0, GTK_POS_TOP, NULL);
    gtk_scale_add_mark(self->window_transparency_scale, 0.25, GTK_POS_TOP, NULL);
    char benchmark_result[32];
    g_snprintf(benchmark_result, sizeof(benchmark_result), "%.2f",
               g_settings_get_double(self->settings, "benchmark"));
    gtk_label_set_text(self->benchmark_label, benchmark_result);

    if(is_keep_above_supported(GTK_WINDOW(self))) {
        g_settings_bind(self->settings, "keep-on-top", self->keep_above_switch,
                        "active", G_SETTINGS_BIND_DEFAULT);
        gtk_widget_set_sensitive(GTK_WIDGET(self->keep_above_switch), true);
        g_idle_add(deferred_update_keep_above, self);
        gtk_label_set_label(self->keep_above_instructions, "");
    } else {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(self->keep_above_switch),
            _("Your compositor does not support this setting. Read below for manual instructions"));
        gtk_widget_set_sensitive(GTK_WIDGET(self->keep_above_switch), false);
        adw_switch_row_set_active(self->keep_above_switch, false);
        gtk_label_set_label(self->keep_above_instructions,
                            get_always_on_top_tip_text());
    }
}
