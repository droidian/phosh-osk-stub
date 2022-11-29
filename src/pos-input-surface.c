/*
 * Copyright (C) 2021 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "pos-input-surface"

#include "pos-config.h"

#include "pos-debug-widget.h"
#include "pos-input-method.h"
#include "pos-input-surface.h"
#include "pos-osk-widget.h"
#include "pos-vk-driver.h"
#include "pos-virtual-keyboard.h"
#include "pos-vk-driver.h"
#include "util.h"

#include <handy.h>
#define LIBFEEDBACK_USE_UNSTABLE_API
#include <libfeedback.h>

#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <libgnome-desktop/gnome-xkb-info.h>

enum {
  PROP_0,
  PROP_INPUT_METHOD,
  PROP_SCREEN_KEYBOARD_ENABLED,
  PROP_KEYBOARD_DRIVER,
  PROP_SURFACE_VISIBLE,
  PROP_LAST_PROP
};
static GParamSpec *props[PROP_LAST_PROP];

typedef struct {
  gboolean show;
  double   progress;
  gint64   last_frame;
} PosInputSurfaceAnimation;

/**
 * PosInputSurface:
 *
 * Main surface that has all the widgets. Should not bother
 * how the OSK is driven.
 *
 * As toplevel widget it also implements #GActionMap so one
 * can easily add and remove actions.
 */
struct _PosInputSurface {
  PhoshLayerSurface        parent;

  gboolean                 surface_visible;
  PosInputSurfaceAnimation animation;

  /* GNOME settings */
  gboolean                 screen_keyboard_enabled;
  GSettings               *a11y_settings;
  GSettings               *input_settings;
  GnomeXkbInfo            *xkbinfo;

  /* wayland input-method */
  PosInputMethod          *input_method;

  /* OSK */
  GHashTable              *osks;
  HdyDeck                 *deck;
  GtkWidget               *osk_terminal;

  /* The debug surface */
  PosDebugWidget          *debug_widget;

  /* TODO: this should be an interface for different keyboard drivers */
  PosVkDriver             *keyboard_driver;

  GtkCssProvider          *css_provider;
  char                    *theme_name;

  GtkBox                  *menu_box_layouts;
  GtkPopover              *menu_popup;
  GSimpleActionGroup      *action_map;
};


static void pos_input_surface_action_group_iface_init (GActionGroupInterface *iface);
static void pos_input_surface_action_map_iface_init (GActionMapInterface *iface);

G_DEFINE_TYPE_WITH_CODE (PosInputSurface, pos_input_surface, PHOSH_TYPE_LAYER_SURFACE,
                         G_IMPLEMENT_INTERFACE (G_TYPE_ACTION_GROUP, pos_input_surface_action_group_iface_init)
                         G_IMPLEMENT_INTERFACE (G_TYPE_ACTION_MAP, pos_input_surface_action_map_iface_init)
  )

/* Select proper style sheet in case of high contrast */
static void
on_gtk_theme_name_changed (PosInputSurface *self, GParamSpec *pspec, GtkSettings *settings)
{
  const char *style;
  g_autofree char *name = NULL;

  g_autoptr (GtkCssProvider) provider = gtk_css_provider_new ();

  g_object_get (settings, "gtk-theme-name", &name, NULL);

  if (g_strcmp0 (self->theme_name, name) == 0)
    return;

  self->theme_name = g_steal_pointer (&name);
  g_debug ("GTK theme: %s", self->theme_name);

  if (self->css_provider) {
    gtk_style_context_remove_provider_for_screen (gdk_screen_get_default (),
                                                  GTK_STYLE_PROVIDER (self->css_provider));
  }

  if (g_strcmp0 (self->theme_name, "HighContrast") == 0)
    style = "/sm/puri/phosh/osk-stub/stylesheet/adwaita-hc-light.css";
  else
    style = "/sm/puri/phosh/osk-stub/stylesheet/adwaita-dark.css";

  gtk_css_provider_load_from_resource (provider, style);
  gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
                                             GTK_STYLE_PROVIDER (provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_set_object (&self->css_provider, provider);
}


static void
on_osk_key_down (PosInputSurface *self, const char *symbol, GtkWidget *osk_widget)
{
  g_autoptr (LfbEvent) event = NULL;

  g_return_if_fail (POS_IS_INPUT_SURFACE (self));
  g_return_if_fail (POS_IS_OSK_WIDGET (osk_widget));

  g_debug ("Key: '%s' down", symbol);

  event = lfb_event_new ("button-pressed");
  lfb_event_trigger_feedback_async (event, NULL, NULL, NULL);
}


static void
on_osk_key_symbol (PosInputSurface *self, const char *symbol, GtkWidget *osk_widget)
{
  g_return_if_fail (POS_IS_INPUT_SURFACE (self));
  g_return_if_fail (POS_IS_OSK_WIDGET (osk_widget));

  g_debug ("Key: '%s' symbol", symbol);

  if (g_str_has_prefix (symbol, "KEY_") ||
      !pos_input_method_get_active (self->input_method)) {
    pos_vk_driver_key_down (self->keyboard_driver, symbol);
    pos_vk_driver_key_up (self->keyboard_driver, symbol);
  } else {
    pos_input_method_send_string (self->input_method, symbol, TRUE);
  }
}


static void
clipboard_copy_activated (GSimpleAction *action,
                          GVariant      *parameter,
                          gpointer       data)
{
  PosInputSurface *self = POS_INPUT_SURFACE (data);

  pos_vk_driver_key_down (self->keyboard_driver, "KEY_COPY");
  pos_vk_driver_key_up (self->keyboard_driver, "KEY_COPY");
}


static void
clipboard_paste_activated (GSimpleAction *action,
                           GVariant      *parameter,
                           gpointer       data)
{
  PosInputSurface *self = POS_INPUT_SURFACE (data);

  pos_vk_driver_key_down (self->keyboard_driver, "KEY_PASTE");
  pos_vk_driver_key_up (self->keyboard_driver, "KEY_PASTE");
}


static void
menu_add_layout (gpointer key, gpointer value, gpointer data)
{
  GtkWidget *button;
  const char *name = key;
  PosOskWidget *osk_widget = POS_OSK_WIDGET (value);
  PosInputSurface *self = POS_INPUT_SURFACE (data);
  const char *display_name = pos_osk_widget_get_display_name (osk_widget);

  button = g_object_new (GTK_TYPE_MODEL_BUTTON,
                         "visible", TRUE,
                         "label", display_name,
                         "action-name", "win.select-layout",
                         "action-target", g_variant_new_string (name),
                         NULL);
  gtk_box_pack_start (self->menu_box_layouts, button, FALSE, FALSE, 0);
}


static void
menu_activated (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  PosInputSurface *self = POS_INPUT_SURFACE (data);
  GdkRectangle rect = {};
  GtkWidget *osk_widget;
  GAction *layout_action;
  const char *osk_name;

  osk_widget = hdy_deck_get_visible_child (self->deck);
  osk_name = pos_osk_widget_get_name (POS_OSK_WIDGET (osk_widget));
  g_variant_get (parameter, "(ii)", &rect.x, &rect.y);
  g_debug ("Menu popoup activated at %d %d, current: '%s'", rect.x, rect.y, osk_name);

  layout_action = g_action_map_lookup_action (G_ACTION_MAP (self->action_map), "select-layout");
  g_simple_action_set_state (G_SIMPLE_ACTION (layout_action),
                             g_variant_new ("s", osk_name));

  gtk_container_foreach (GTK_CONTAINER (self->menu_box_layouts),
                         (GtkCallback) gtk_widget_destroy,
                         NULL);

  g_hash_table_foreach (self->osks, menu_add_layout, self);
  menu_add_layout ((gpointer)pos_osk_widget_get_name (POS_OSK_WIDGET (self->osk_terminal)),
                   self->osk_terminal,
                   self);

  gtk_popover_set_relative_to (self->menu_popup, osk_widget);
  gtk_popover_set_pointing_to (self->menu_popup, &rect);

  gtk_popover_popup (self->menu_popup);
}


static void
select_layout_change_state (GSimpleAction *action,
                            GVariant      *parameter,
                            gpointer       data)
{
  PosInputSurface *self = POS_INPUT_SURFACE (data);
  const char *layout = NULL;
  GtkWidget *osk_widget;

  /* popdown popover right away to avoid flicker when switching layouts */
  gtk_popover_popdown (self->menu_popup);

  g_variant_get (parameter, "&s", &layout);
  g_debug ("Layout '%s' selected", layout);

  osk_widget = g_hash_table_lookup (self->osks, layout);
  if (osk_widget == NULL) {
    if (g_str_equal (layout, "terminal")) {
      osk_widget = self->osk_terminal;
    } else {
      g_warning ("Failed to find layout '%s'", layout);
      return;
    }
  }

  hdy_deck_set_visible_child (self->deck, GTK_WIDGET (osk_widget));
  g_simple_action_set_state (action, parameter);
}



static void
on_visible_child_changed (PosInputSurface *self)
{
  GtkWidget *child;
  PosOskWidget *osk;

  child = hdy_deck_get_visible_child (self->deck);
  if (!POS_IS_OSK_WIDGET (child))
    return;

  osk = POS_OSK_WIDGET (child);
  g_debug ("Switched to layout '%s'", pos_osk_widget_get_display_name (osk));
  pos_osk_widget_set_layer (osk, POS_OSK_WIDGET_LAYER_NORMAL);
}

static void
pos_screen_keyboard_set_enabled (PosInputSurface *self, gboolean enable)
{
  const char *msg = enable ? "enabled" : "disabled";

  g_debug ("Screen keyboard enable: %s", msg);

  if (enable == self->screen_keyboard_enabled)
    return;

  self->screen_keyboard_enabled = enable;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SCREEN_KEYBOARD_ENABLED]);
}


static void
pos_input_surface_move (PosInputSurface *self)
{
  int margin;
  int height;
  double progress = hdy_ease_out_cubic (self->animation.progress);

  if (self->animation.show)
    progress = 1.0 - progress;

  g_object_get (self, "configured-height", &height, NULL);
  margin = -height * progress;

  phosh_layer_surface_set_margins (PHOSH_LAYER_SURFACE (self), 0, 0, margin, 0);

  if (self->animation.progress >= 1.0 &&  self->animation.show) {
    /* On unfold adjust the exclusive zone at the very end to avoid flickering */
    phosh_layer_surface_set_exclusive_zone (PHOSH_LAYER_SURFACE (self), height);
  } else if (self->animation.progress < 1.0 && !self->animation.show) {
    /* On fold adjust the exclusive zone at the start to avoid flickering */
    phosh_layer_surface_set_exclusive_zone (PHOSH_LAYER_SURFACE (self), 0);
  }

  if (self->animation.show) {
    gtk_widget_show (GTK_WIDGET (self));
  } else if (self->animation.progress >= 1.0 && !self->animation.show) {
    GtkWidget *widget;

    gtk_widget_hide (GTK_WIDGET (self));
    widget = hdy_deck_get_visible_child (self->deck);
    if (POS_IS_OSK_WIDGET (widget))
      pos_osk_widget_set_layer (POS_OSK_WIDGET (widget), POS_OSK_WIDGET_LAYER_NORMAL);
  }

  phosh_layer_surface_wl_surface_commit (PHOSH_LAYER_SURFACE (self));
}


static gboolean
animate_cb (GtkWidget     *widget,
            GdkFrameClock *frame_clock,
            gpointer       user_data)
{
  PosInputSurface *self = POS_INPUT_SURFACE (widget);
  gint64 time;
  gboolean finished = FALSE;

  time = gdk_frame_clock_get_frame_time (frame_clock) - self->animation.last_frame;
  if (self->animation.last_frame < 0)
    time = 0;

  self->animation.progress += 0.06666 * time / 16666.00;
  self->animation.last_frame = gdk_frame_clock_get_frame_time (frame_clock);

  if (self->animation.progress >= 1.0) {
    finished = TRUE;
    self->animation.progress = 1.0;
  }

  pos_input_surface_move (self);

  if (finished)
    return G_SOURCE_REMOVE;

  return G_SOURCE_CONTINUE;
}


static double
reverse_ease_out_cubic (double t)
{
  return cbrt (t - 1) + 1;
}


static void
pos_input_surface_set_property (GObject      *object,
                                guint         property_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  PosInputSurface *self = POS_INPUT_SURFACE (object);

  switch (property_id) {
  case PROP_INPUT_METHOD:
    self->input_method = g_value_dup_object (value);
    pos_debug_widget_set_input_method (self->debug_widget, self->input_method);
    break;
  case PROP_SCREEN_KEYBOARD_ENABLED:
    pos_screen_keyboard_set_enabled (self, g_value_get_boolean (value));
    break;
  case PROP_KEYBOARD_DRIVER:
    self->keyboard_driver = g_value_dup_object (value);
    break;
  case PROP_SURFACE_VISIBLE:
    pos_input_surface_set_visible (self, g_value_get_boolean (value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
pos_input_surface_get_property (GObject    *object,
                                guint       property_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  PosInputSurface *self = POS_INPUT_SURFACE (object);

  switch (property_id) {
  case PROP_SCREEN_KEYBOARD_ENABLED:
    g_value_set_boolean (value, pos_input_surface_get_screen_keyboard_enabled (self));
    break;
  case PROP_SURFACE_VISIBLE:
    g_value_set_boolean (value, pos_input_surface_get_visible (self));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
on_im_purpose_changed (PosInputSurface *self, GParamSpec *pspec, PosInputMethod *im)
{
  g_assert (POS_IS_INPUT_SURFACE (self));
  g_assert (POS_IS_INPUT_METHOD (im));
}


static void
on_im_text_change_cause_changed (PosInputSurface *self, GParamSpec *pspec, PosInputMethod *im)
{
  g_assert (POS_IS_INPUT_SURFACE (self));
  g_assert (POS_IS_INPUT_METHOD (im));
}


static void
on_im_surrounding_text_changed (PosInputSurface *self, GParamSpec *pspec, PosInputMethod *im)
{
  g_assert (POS_IS_INPUT_SURFACE (self));
  g_assert (POS_IS_INPUT_METHOD (im));
}


static void
on_im_active_changed (PosInputSurface *self, GParamSpec *pspec, PosInputMethod *im)
{
  g_assert (POS_IS_INPUT_SURFACE (self));
  g_assert (POS_IS_INPUT_METHOD (im));
}


static void
pos_input_surface_constructed (GObject *object)
{
  PosInputSurface *self = POS_INPUT_SURFACE (object);

  G_OBJECT_CLASS (pos_input_surface_parent_class)->constructed (object);

  g_assert (POS_IS_INPUT_METHOD (self->input_method));
  g_object_connect (self->input_method,
                    "swapped-object-signal::notify::active", on_im_active_changed, self,
                    "swapped-object-signal::notify::purpose", on_im_purpose_changed, self,
                    "swapped-object-signal::notify::text-change-cause",
                    on_im_text_change_cause_changed, self,
                    "swapped-object-signal::notify::surrounding-text",
                    on_im_surrounding_text_changed, self,
                    NULL);
}


static void
pos_input_surface_dispose (GObject *object)
{
  PosInputSurface *self = POS_INPUT_SURFACE (object);

  /* Remove hash table early since this also destroys the osks in the deck */
  g_clear_pointer (&self->osks, g_hash_table_destroy);

  G_OBJECT_CLASS (pos_input_surface_parent_class)->dispose (object);
}


static void
pos_input_surface_finalize (GObject *object)
{
  PosInputSurface *self = POS_INPUT_SURFACE (object);

  g_clear_object (&self->input_method);
  g_clear_object (&self->a11y_settings);
  g_clear_object (&self->input_settings);
  g_clear_object (&self->xkbinfo);
  g_clear_object (&self->css_provider);
  g_clear_pointer (&self->theme_name, g_free);
  g_clear_pointer (&self->osks, g_hash_table_destroy);
  g_clear_object (&self->action_map);

  G_OBJECT_CLASS (pos_input_surface_parent_class)->finalize (object);
}


static gchar **
pos_input_surface_list_actions (GActionGroup *group)
{
  PosInputSurface *self = POS_INPUT_SURFACE (group);

  /* may be NULL after dispose has run */
  if (!self->action_map)
    return g_new0 (char *, 0 + 1);

  return g_action_group_list_actions (G_ACTION_GROUP (self->action_map));
}

static gboolean
pos_input_surface_query_action (GActionGroup        *group,
                                const gchar         *action_name,
                                gboolean            *enabled,
                                const GVariantType **parameter_type,
                                const GVariantType **state_type,
                                GVariant           **state_hint,
                                GVariant           **state)
{
  PosInputSurface *self = POS_INPUT_SURFACE (group);

  if (!self->action_map)
    return FALSE;

  return g_action_group_query_action (G_ACTION_GROUP (self->action_map),
                                      action_name, enabled, parameter_type, state_type, state_hint, state);
}


static void
pos_input_surface_activate_action (GActionGroup *group,
                                   const gchar  *action_name,
                                   GVariant     *parameter)
{
  PosInputSurface *self = POS_INPUT_SURFACE (group);

  if (!self->action_map)
    return;

  g_action_group_activate_action (G_ACTION_GROUP (self->action_map), action_name, parameter);
}


static void
pos_input_surface_change_action_state (GActionGroup *group,
                                       const gchar  *action_name,
                                       GVariant     *state)
{
  PosInputSurface *self = POS_INPUT_SURFACE (group);

  if (!self->action_map)
    return;

  g_action_group_change_action_state (G_ACTION_GROUP (self->action_map), action_name, state);
}


static void
pos_input_surface_action_group_iface_init (GActionGroupInterface *iface)
{
  iface->list_actions = pos_input_surface_list_actions;
  iface->query_action = pos_input_surface_query_action;
  iface->activate_action = pos_input_surface_activate_action;
  iface->change_action_state = pos_input_surface_change_action_state;
}


static GAction *
pos_input_surface_lookup_action (GActionMap  *action_map,
                                      const gchar *action_name)
{
  PosInputSurface *self = POS_INPUT_SURFACE (action_map);

  if (!self->action_map)
    return NULL;

  return g_action_map_lookup_action (G_ACTION_MAP (self->action_map), action_name);
}

static void
pos_input_surface_add_action (GActionMap *action_map,
                                   GAction    *action)
{
  PosInputSurface *self = POS_INPUT_SURFACE (action_map);

  if (!self->action_map)
    return;

  g_action_map_add_action (G_ACTION_MAP (self->action_map), action);
}

static void
pos_input_surface_remove_action (GActionMap  *action_map,
                                      const gchar *action_name)
{
  PosInputSurface *self = POS_INPUT_SURFACE (action_map);

  if (!self->action_map)
    return;

  g_action_map_remove_action (G_ACTION_MAP (self->action_map), action_name);
}


static void pos_input_surface_action_map_iface_init (GActionMapInterface *iface)
{
  iface->lookup_action = pos_input_surface_lookup_action;
  iface->add_action = pos_input_surface_add_action;
  iface->remove_action = pos_input_surface_remove_action;
}


static void
pos_input_surface_class_init (PosInputSurfaceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = pos_input_surface_get_property;
  object_class->set_property = pos_input_surface_set_property;
  object_class->constructed = pos_input_surface_constructed;
  object_class->dispose = pos_input_surface_dispose;
  object_class->finalize = pos_input_surface_finalize;

  g_type_ensure (POS_TYPE_OSK_WIDGET);
  g_type_ensure (POS_TYPE_DEBUG_WIDGET);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/sm/puri/phosh/osk-stub/ui/input-surface.ui");
  gtk_widget_class_bind_template_child (widget_class, PosInputSurface, debug_widget);
  gtk_widget_class_bind_template_child (widget_class, PosInputSurface, deck);
  gtk_widget_class_bind_template_child (widget_class, PosInputSurface, osk_terminal);
  gtk_widget_class_bind_template_child (widget_class, PosInputSurface, menu_box_layouts);
  gtk_widget_class_bind_template_child (widget_class, PosInputSurface, menu_popup);
  gtk_widget_class_bind_template_callback (widget_class, on_visible_child_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_osk_key_down);
  gtk_widget_class_bind_template_callback (widget_class, on_osk_key_symbol);

  /**
   * PosInputSurface:input-method:
   *
   * A zwp_input_method_v2
   */
  props[PROP_INPUT_METHOD] =
    g_param_spec_object ("input-method", "", "",
                         POS_TYPE_INPUT_METHOD,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  /**
   * PosInputSurface:enable
   *
   * Whether an on screen keyboard should be enabled. This is the
   * global toggle that enables screen keyboards and maps the a11y
   * settings.
   */
  props[PROP_SCREEN_KEYBOARD_ENABLED] =
    g_param_spec_boolean ("screen-keyboard-enabled", "", "", FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  /**
   * PosInputSurface:surface-visible
   *
   * If this is %TRUE the input surface will be shown, otherwise hidden.
   */
  props[PROP_SURFACE_VISIBLE] =
    g_param_spec_boolean ("surface-visible", "", "", FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  props[PROP_KEYBOARD_DRIVER] =
    g_param_spec_object ("keyboard-driver", "", "",
                         /* TODO: should be an interface */
                         POS_TYPE_VK_DRIVER,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);
}


static PosOskWidget *
insert_osk (PosInputSurface *self,
            const char      *name,
            const char      *display_name,
            const char      *layout,
            const char      *variant)
{
  g_autoptr (GError) err = NULL;
  PosOskWidget *osk_widget;

  osk_widget = g_hash_table_lookup (self->osks, name);
  if (osk_widget)
    return osk_widget;

  osk_widget = pos_osk_widget_new ();
  if (!pos_osk_widget_set_layout (POS_OSK_WIDGET (osk_widget),
                                  display_name, layout, variant, &err)) {
    g_warning ("Failed to load osk layout for %s: %s", name, err->message);

    gtk_widget_destroy (g_object_ref_sink (GTK_WIDGET (osk_widget)));
    return NULL;
  }

  g_debug ("Adding osk for layout '%s'", name);
  gtk_widget_set_visible (GTK_WIDGET (osk_widget), TRUE);
  g_object_connect (osk_widget,
                    "swapped-signal::key-down", G_CALLBACK (on_osk_key_down), self,
                    "swapped-signal::key-symbol", G_CALLBACK (on_osk_key_symbol), self,
                    NULL);

  hdy_deck_insert_child_after (self->deck, GTK_WIDGET (osk_widget), NULL);
  g_hash_table_insert (self->osks, g_strdup (name), osk_widget);

  return osk_widget;
}


static PosOskWidget *
insert_layout (PosInputSurface *self, const char *type, const char *id)
{
  g_autofree char *name = NULL;
  const gchar *layout = NULL;
  const gchar *variant = NULL;
  const gchar *display_name = NULL;

  if (g_strcmp0 (type, "xkb")) {
    g_debug ("Not a xkb layout: '%s' - ignoring", id);
    return NULL;
  }

  if (!gnome_xkb_info_get_layout_info (self->xkbinfo, id, &display_name, NULL,
                                       &layout, &variant)) {
    g_warning ("Failed to get layout info for %s", id);
    return NULL;
  }
  if (STR_IS_NULL_OR_EMPTY (variant))
    name = g_strdup (layout);
  else
    name = g_strdup_printf ("%s+%s", layout, variant);

  return insert_osk (self, name, display_name, layout, variant);
}


static void
on_input_setting_changed (PosInputSurface *self, const char *key, GSettings *settings)
{
  g_autoptr (GVariant) sources = NULL;
  g_autoptr (GHashTable) new = NULL;
  g_autofree GStrv old = NULL;
  g_autofree GStrv old_keys = NULL;
  GVariantIter iter;
  const char *id = NULL;
  const char *type = NULL;
  gboolean first_set = FALSE;

  g_debug ("Setting changed, reloading input settings");

  sources = g_settings_get_value (settings, "sources");
  g_variant_iter_init (&iter, sources);

  /* Get us a copy of the keys since we remove elements while iterating */
  old_keys = (GStrv)g_hash_table_get_keys_as_array (self->osks, NULL);
  old = g_strdupv (old_keys);
  new = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  while (g_variant_iter_next (&iter, "(&s&s)", &type, &id)) {
    PosOskWidget *osk_widget;

    osk_widget = insert_layout (self, type, id);
    if (osk_widget == NULL)
      continue;

    g_hash_table_add (new, g_strdup (pos_osk_widget_get_name (osk_widget)));
    if (!first_set) {
      first_set = TRUE;
      hdy_deck_set_visible_child (self->deck, GTK_WIDGET (osk_widget));
    }
  }

  if (old) {
    /* Drop remove layouts */
    for (int i = 0; old[i]; i++) {
      if (!g_hash_table_contains (new, old[i])) {
        g_debug ("Removing layout %s", old[i]);
        g_hash_table_remove (self->osks, old[i]);
      }
    }
  }

  /* If nothing is left add a default */
  if (g_hash_table_size (self->osks) == 0) {
    insert_osk (self, "us", "English (USA)", "us", NULL);
  }
}


static GActionEntry entries[] =
{
  { .name = "clipboard-copy", .activate = clipboard_copy_activated },
  { .name = "clipboard-paste", .activate = clipboard_paste_activated },
  { .name = "select-layout", .parameter_type = "s", .state = "\"terminal\"",
    .change_state = select_layout_change_state },
  { .name = "menu", .parameter_type = "(ii)", .activate = menu_activated },
};


static void
pos_input_surface_init (PosInputSurface *self)
{
  GtkSettings *gtk_settings;
  const char *test_layout = g_getenv ("POS_TEST_LAYOUT");

  gtk_widget_init_template (GTK_WIDGET (self));

  self->action_map = g_simple_action_group_new ();
  g_action_map_add_action_entries (G_ACTION_MAP (self->action_map),
                                   entries,
                                   G_N_ELEMENTS (entries),
                                   self);
  gtk_widget_insert_action_group (GTK_WIDGET (self),
                                  "win",
                                  G_ACTION_GROUP (self->action_map));

  /* Ensure initial sync */
  self->screen_keyboard_enabled = -1;
  self->surface_visible = -1;
  self->animation.progress = 1.0;

  self->a11y_settings = g_settings_new ("org.gnome.desktop.a11y.applications");
  g_settings_bind (self->a11y_settings, "screen-keyboard-enabled",
                   self, "screen-keyboard-enabled", G_SETTINGS_BIND_GET);

  self->osks = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)gtk_widget_destroy);
  self->xkbinfo = gnome_xkb_info_new ();
  self->input_settings = g_settings_new ("org.gnome.desktop.input-sources");

  if (test_layout) {
    insert_layout (self, "xkb", test_layout);
  } else {
    g_object_connect (self->input_settings,
                      "swapped-signal::changed::sources",
                      G_CALLBACK (on_input_setting_changed), self,
                      "swapped-signal::changed::xkb-options",
                      G_CALLBACK (on_input_setting_changed), self,
                      NULL);
    on_input_setting_changed (self, NULL, self->input_settings);
  }

  gtk_settings = gtk_settings_get_default ();
  g_object_set (G_OBJECT (gtk_settings), "gtk-application-prefer-dark-theme", TRUE, NULL);

  g_signal_connect_swapped (gtk_settings, "notify::gtk-theme-name",
                            G_CALLBACK (on_gtk_theme_name_changed), self);
  on_gtk_theme_name_changed (self, NULL, gtk_settings);

  pos_osk_widget_set_layout (POS_OSK_WIDGET (self->osk_terminal), "Terminal", "terminal", NULL, NULL);
}


gboolean
pos_input_surface_get_active (PosInputSurface *self)
{
  g_return_val_if_fail (POS_IS_INPUT_SURFACE (self), FALSE);

  return pos_input_method_get_active (self->input_method);
}


void
pos_input_surface_set_visible (PosInputSurface *self, gboolean visible)
{
  g_return_if_fail (POS_IS_INPUT_SURFACE (self));

  if (visible == self->surface_visible)
    return;

  self->surface_visible = visible;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SURFACE_VISIBLE]);

  self->animation.show = visible;
  self->animation.last_frame = -1;
  self->animation.progress =
    reverse_ease_out_cubic (1.0 - hdy_ease_out_cubic (self->animation.progress));

  gtk_widget_add_tick_callback (GTK_WIDGET (self), animate_cb, NULL, NULL);
}


gboolean
pos_input_surface_get_visible (PosInputSurface *self)
{
  g_return_val_if_fail (POS_IS_INPUT_SURFACE (self), FALSE);

  return self->surface_visible;
}


gboolean
pos_input_surface_get_screen_keyboard_enabled (PosInputSurface *self)
{
  g_return_val_if_fail (POS_IS_INPUT_SURFACE (self), FALSE);

  return self->screen_keyboard_enabled;
}
