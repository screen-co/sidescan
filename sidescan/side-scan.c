#include <hyscan-gtk-area.h>
#include <hyscan-sonar-driver.h>
#include <hyscan-sonar-client.h>
#include <hyscan-sonar-control.h>

#include <hyscan-gtk-waterfall.h>
#include <hyscan-gtk-waterfall-grid.h>
#include <hyscan-gtk-waterfall-control.h>
#include <hyscan-tile-color.h>
#include <hyscan-db-info.h>
#include <hyscan-cached.h>

#include "sonar-configure.h"

#define SIDE_SCAN_MAX_DISTANCE         150.0
#define MAX_COLOR_MAPS                 3

enum
{
  DATE_SORT_COLUMN,
  TRACK_COLUMN,
  DATE_COLUMN,
  HAS_RAW_DATA_COLUMN,
  N_COLUMNS
};

typedef struct
{
  HyScanDB                            *db;
  HyScanDBInfo                        *db_info;

  gchar                               *project_name;
  gchar                               *track_prefix;
  gchar                               *track_name;
  gboolean                             new_track;

  HyScanCache                         *cache;

  gboolean                             full_screen;

  GArray                              *color_maps[MAX_COLOR_MAPS];
  guint                                cur_color_map;
  gdouble                              cur_brightness;

  struct
  {
    HyScanSonarControl                *sonar;
    HyScanTVGControl                  *tvg;
    HyScanGeneratorControl            *gen;

    gdouble                            cur_distance;
    guint                              cur_signal;
    gdouble                            cur_tvg_level;
    gdouble                            cur_tvg_sensitivity;

    struct
    {
      HyScanDataSchemaEnumValue      **signals;
      guint                            n_signals;
    } starboard;
    struct
    {
      HyScanDataSchemaEnumValue      **signals;
      guint                            n_signals;
    } port;
  } sonar;

  GtkLabel                            *brightness_value;
  GtkLabel                            *color_map_value;
  GtkLabel                            *scale_value;

  GtkLabel                            *distance_value;
  GtkLabel                            *tvg_level_value;
  GtkLabel                            *tvg_sensitivity_value;
  GtkLabel                            *signal_value;

  GtkWidget                           *window;
  GtkTreeView                         *track_view;
  GtkTreeModel                        *track_list;
  GtkAdjustment                       *track_range;

  HyScanGtkWaterfall                  *wf;
  HyScanGtkWaterfallState             *wf_state;
  HyScanGtkWaterfallGrid              *wf_grid;
  HyScanGtkWaterfallControl           *wf_control;
  GtkSwitch                           *live_view;

  GtkSwitch                           *start_stop;

} Global;

static gboolean scale_set (Global *global);

/* Функция изменяет режим окна full screen. */
static gboolean
key_press (GtkWidget   *widget,
           GdkEventKey *event,
           Global      *global)
{
  if (event->keyval == GDK_KEY_F11)
    {
      if (global->full_screen)
        gtk_window_unfullscreen (GTK_WINDOW (global->window));
      else
        gtk_window_fullscreen (GTK_WINDOW (global->window));

      global->full_screen = !global->full_screen;
    }

  return FALSE;
}

/* Функция вызывается при изменении списка проектов. */
static void
projects_changed (HyScanDBInfo *db_info,
                  Global       *global)
{
  GHashTable *projects = hyscan_db_info_get_projects (db_info);

  /* Если рабочий проект есть в списке, мониторим его. */
  if (g_hash_table_lookup (projects, global->project_name))
    hyscan_db_info_set_project (db_info, global->project_name);

  g_hash_table_unref (projects);
}

/* Функция вызывается при изменении списка галсов. */
static void
tracks_changed (HyScanDBInfo *db_info,
                Global       *global)
{
  GtkTreePath *null_path;
  GtkTreeIter tree_iter;
  GHashTable *tracks;
  GHashTableIter hash_iter;
  gpointer key, value;
  gchar *cur_track_name;

  cur_track_name = g_strdup (global->track_name);

  null_path = gtk_tree_path_new ();
  gtk_tree_view_set_cursor (global->track_view, null_path, NULL, FALSE);
  gtk_tree_path_free (null_path);

  gtk_list_store_clear (GTK_LIST_STORE (global->track_list));

  tracks = hyscan_db_info_get_tracks (db_info);
  g_hash_table_iter_init (&hash_iter, tracks);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      HyScanTrackInfo *track_info;
      HyScanSourceInfo *starboard_info;
      HyScanSourceInfo *port_info;
      gboolean has_computed_data;
      gboolean has_raw_data;

      /* Проверяем что галс содержит данные ГБО. */
      track_info = value;
      starboard_info = g_hash_table_lookup (track_info->sources, GINT_TO_POINTER (HYSCAN_SOURCE_SIDE_SCAN_STARBOARD));
      port_info = g_hash_table_lookup (track_info->sources, GINT_TO_POINTER (HYSCAN_SOURCE_SIDE_SCAN_PORT));
      if (!starboard_info || !port_info)
        continue;

      /* Проверяем наличие обработанных и сырых данных. */
      has_computed_data = starboard_info->computed && port_info->computed;
      has_raw_data = starboard_info->raw && port_info->raw;
      if (!has_computed_data && !has_raw_data)
        continue;

      /* Добавляем в список галсов. */
      gtk_list_store_append (GTK_LIST_STORE (global->track_list), &tree_iter);
      gtk_list_store_set (GTK_LIST_STORE (global->track_list), &tree_iter,
                          DATE_SORT_COLUMN, g_date_time_to_unix (track_info->ctime),
                          TRACK_COLUMN, g_strdup (track_info->name),
                          DATE_COLUMN, g_date_time_format (track_info->ctime, "%d/%m/%Y %H:%M"),
                          HAS_RAW_DATA_COLUMN, has_raw_data,
                          -1);

      /* Подсвечиваем текущий галс. */
      if (g_strcmp0 (cur_track_name, track_info->name) == 0)
        {
          GtkTreePath *tree_path = gtk_tree_model_get_path (global->track_list, &tree_iter);
          gtk_tree_view_set_cursor (global->track_view, tree_path, NULL, FALSE);
          gtk_tree_path_free (tree_path);
        }
    }

  g_hash_table_unref (tracks);
  g_free (cur_track_name);
}

/* Функция прокручивает список галсов. */
static gboolean
track_scroll (GtkWidget *widget,
              GdkEvent  *event,
              Global    *global)
{
  gdouble position;
  gdouble step_x, step_y;
  gint size_x, size_y;

  position = gtk_adjustment_get_value (global->track_range);
  gdk_event_get_scroll_deltas (event, &step_x, &step_y);
  gtk_tree_view_convert_bin_window_to_widget_coords (global->track_view, 0, 0, &size_x, &size_y);
  position += step_y * size_y;

  gtk_adjustment_set_value (global->track_range, position);

  return TRUE;
}

/* Обработчик изменения галса. */
static void
track_changed (GtkTreeView *list,
               Global      *global)
{
  GValue value = G_VALUE_INIT;
  GtkTreePath *path = NULL;
  GtkTreeIter iter;

  gboolean has_raw_data;
  gchar *track_name;

  /* Определяем название нового галса. */
  gtk_tree_view_get_cursor (list, &path, NULL);
  if (path == NULL)
    return;

  if (!gtk_tree_model_get_iter (global->track_list, &iter, path))
    return;

  gtk_tree_model_get_value (global->track_list, &iter, HAS_RAW_DATA_COLUMN, &value);
  has_raw_data = g_value_get_boolean (&value);
  g_value_unset (&value);

  gtk_tree_model_get_value (global->track_list, &iter, TRACK_COLUMN, &value);
  track_name = g_value_dup_string (&value);
  g_value_unset (&value);

  /* Если создан новый галс или названия текущего и выбранного галса не совпадают, открываем этот галс. */
  if (global->new_track || (g_strcmp0 (global->track_name, track_name) != 0))
    {
      g_clear_pointer(&global->track_name, g_free);
      global->track_name = track_name;
      global->new_track = FALSE;

      hyscan_gtk_waterfall_state_set_track (global->wf_state, global->db, global->project_name, global->track_name, has_raw_data);
      hyscan_gtk_waterfall_automove (global->wf, TRUE);
      scale_set (global);
    }
  else
    {
      g_free (track_name);
    }
}

/* Функция устанавливает яркость отображения. */
static gboolean
brightness_set (Global  *global,
                gdouble  cur_brightness)
{
  gchar *text;
  gdouble black;
  gdouble gamma;
  gdouble white;

  if (cur_brightness < 0.0)
    return FALSE;
  if (cur_brightness > 100.0)
    return FALSE;

  black = 0.0;
  gamma = 1.25 - 0.5 * (cur_brightness / 100.0);
  white = 1.0 - (cur_brightness / 100.0) * 0.99;

  hyscan_gtk_waterfall_set_levels_for_all (global->wf, black, gamma, white);

  text = g_strdup_printf ("<small><b>%.0f%%</b></small>", cur_brightness);
  gtk_label_set_markup (global->brightness_value, text);
  g_free (text);

  return TRUE;
}

/* Функция устанавливает новую палитру. */
static gboolean
color_map_set (Global *global,
               guint   cur_color_map)
{
  gchar *text;
  const gchar *color_map_name;

  if (cur_color_map >= MAX_COLOR_MAPS)
    return FALSE;

  switch (cur_color_map)
    {
    case 0:
      color_map_name = "БЕЛАЯ";
      break;

    case 1:
      color_map_name = "ЖЁЛТАЯ";
      break;

    case 2:
      color_map_name = "ЗЕЛЁНАЯ";
      break;

    default:
      return FALSE;
    }

  hyscan_gtk_waterfall_set_colormap_for_all (global->wf,
                                             (guint32*)global->color_maps[cur_color_map]->data,
                                             global->color_maps[cur_color_map]->len,
                                             0xff000000);

  text = g_strdup_printf ("<small><b>%s</b></small>", color_map_name);
  gtk_label_set_markup (global->color_map_value, text);
  g_free (text);

  return TRUE;
}

/* Функция устанавливает масштаб отображения. */
static gboolean
scale_set (Global *global)
{
  const gdouble *scales;
  gint n_scales;
  gint i_scale;
  gchar *text;

  i_scale = hyscan_gtk_waterfall_get_scale (global->wf, &scales, &n_scales);

  text = g_strdup_printf ("<small><b>1:%.0f</b></small>", scales[i_scale]);
  gtk_label_set_markup (global->scale_value, text);
  g_free (text);

  return TRUE;
}

/* Функция устанавливает излучаемый сигнал. */
static gboolean
signal_set (Global *global,
            guint   cur_signal)
{
  gchar *text;
  gint is_equal;
  gboolean status;

  if (cur_signal == 0)
    return FALSE;
  if (cur_signal >= global->sonar.starboard.n_signals)
    return FALSE;
  if (cur_signal >= global->sonar.port.n_signals)
    return FALSE;

  status = hyscan_generator_control_set_preset (global->sonar.gen,
                                                HYSCAN_SOURCE_SIDE_SCAN_STARBOARD,
                                                global->sonar.starboard.signals[cur_signal]->value);
  if (!status)
    return FALSE;

  status = hyscan_generator_control_set_preset (global->sonar.gen,
                                                HYSCAN_SOURCE_SIDE_SCAN_PORT,
                                                global->sonar.port.signals[cur_signal]->value);
  if (!status)
    return FALSE;

  is_equal = g_strcmp0 (global->sonar.starboard.signals[cur_signal]->name,
                        global->sonar.port.signals[cur_signal]->name);

  if (is_equal == 0)
    {
      text = g_strdup_printf ("<small><b>%s</b></small>",
                              global->sonar.starboard.signals[cur_signal]->name);
    }
  else
    {
      text = g_strdup_printf ("<small><b>%s, %s</b></small>",
                              global->sonar.starboard.signals[cur_signal]->name,
                              global->sonar.port.signals[cur_signal]->name);
    }

  gtk_label_set_markup (global->signal_value, text);
  g_free (text);

  return TRUE;
}

/* Функция устанавливает параметры ВАРУ. */
static gboolean
tvg_set (Global  *global,
         gdouble  level,
         gdouble  sensitivity)
{
  gchar *text;
  gboolean status;
  gdouble min_gain;
  gdouble max_gain;

  if ((level < 0.0) || (level > 1.0) || (sensitivity < 0.0) || (sensitivity > 1.0))
    return FALSE;

  hyscan_tvg_control_get_gain_range (global->sonar.tvg, HYSCAN_SOURCE_SIDE_SCAN_STARBOARD,
                                     &min_gain, &max_gain);

  status = hyscan_tvg_control_set_auto (global->sonar.tvg,
                                        HYSCAN_SOURCE_SIDE_SCAN_STARBOARD,
                                        level, sensitivity);
  if (!status)
    return FALSE;

  hyscan_tvg_control_get_gain_range (global->sonar.tvg, HYSCAN_SOURCE_SIDE_SCAN_PORT,
                                     &min_gain, &max_gain);

  status = hyscan_tvg_control_set_auto (global->sonar.tvg,
                                        HYSCAN_SOURCE_SIDE_SCAN_PORT,
                                        level, sensitivity);
  if (!status)
    return FALSE;

  text = g_strdup_printf ("<small><b>%.1f</b></small>", level);
  gtk_label_set_markup (global->tvg_level_value, text);
  g_free (text);
  text = g_strdup_printf ("<small><b>%.1f</b></small>", sensitivity);
  gtk_label_set_markup (global->tvg_sensitivity_value, text);
  g_free (text);

  return TRUE;
}

/* Функция устанавливает рабочую дистанцию. */
static gboolean
distance_set (Global  *global,
              gdouble  cur_distance)
{
  gchar *text;
  gboolean status;

  if (cur_distance < 1.0)
    return FALSE;
  if (cur_distance > SIDE_SCAN_MAX_DISTANCE)
    return FALSE;

  status = hyscan_sonar_control_set_receive_time (global->sonar.sonar,
                                                  HYSCAN_SOURCE_SIDE_SCAN_STARBOARD,
                                                  cur_distance / 750.0);
  if (!status)
    return FALSE;

  status = hyscan_sonar_control_set_receive_time (global->sonar.sonar,
                                                  HYSCAN_SOURCE_SIDE_SCAN_PORT,
                                                  cur_distance / 750.0);
  if (!status)
    return FALSE;

  text = g_strdup_printf ("<small><b>%.0f m</b></small>", cur_distance);
  gtk_label_set_markup (global->distance_value, text);
  g_free (text);

  return TRUE;
}

static void
brightness_up (GtkWidget *widget,
               Global    *global)
{
  gdouble cur_brightness;

  if (global->cur_brightness < 50.0)
    cur_brightness = global->cur_brightness + 10.0;
  else if (global->cur_brightness < 90.0)
    cur_brightness = global->cur_brightness + 5.0;
  else
    cur_brightness = global->cur_brightness + 1.0;

  if (brightness_set (global, cur_brightness))
    global->cur_brightness = cur_brightness;
}

static void
brightness_down (GtkWidget *widget,
                 Global    *global)
{
  gdouble cur_brightness;

  if (global->cur_brightness > 90.0)
    cur_brightness = global->cur_brightness - 1.0;
  else if (global->cur_brightness > 50.0)
    cur_brightness = global->cur_brightness - 5.0;
  else
    cur_brightness = global->cur_brightness - 10.0;

  if (brightness_set (global, cur_brightness))
    global->cur_brightness = cur_brightness;
}

static void
color_map_up (GtkWidget *widget,
              Global    *global)
{
  guint cur_color_map = global->cur_color_map + 1;

  if (color_map_set (global, cur_color_map))
    global->cur_color_map = cur_color_map;
}

static void
color_map_down (GtkWidget *widget,
                Global    *global)
{
  guint cur_color_map = global->cur_color_map - 1;

  if (color_map_set (global, cur_color_map))
    global->cur_color_map = cur_color_map;
}

static void
scale_up (GtkWidget *widget,
          Global    *global)
{
  hyscan_gtk_waterfall_control_zoom (global->wf_control, TRUE);
  scale_set (global);
}

static void
scale_down (GtkWidget *widget,
            Global    *global)
{
  hyscan_gtk_waterfall_control_zoom (global->wf_control, FALSE);
  scale_set (global);
}

static gboolean
live_view (GtkWidget  *widget,
           gboolean    state,
           Global     *global)
{
  if (state)
    {
      if (!gtk_switch_get_state (global->live_view))
        hyscan_gtk_waterfall_automove (global->wf, state);

      gtk_switch_set_state (global->live_view, TRUE);
    }
  else
    {
      if (gtk_switch_get_state (global->live_view))
        hyscan_gtk_waterfall_automove (global->wf, state);

      gtk_switch_set_state (global->live_view, FALSE);
    }

  return TRUE;
}

static void
live_view_off (GtkWidget  *widget,
               gboolean    state,
               Global     *global)
{
  if (!state)
    gtk_switch_set_active (global->live_view, FALSE);
}

static void
distance_up (GtkWidget *widget,
             Global    *global)
{
  gdouble cur_distance = global->sonar.cur_distance + 10.0;

  if (distance_set (global, cur_distance))
    global->sonar.cur_distance = cur_distance;
}

static void
distance_down (GtkWidget *widget,
               Global    *global)
{
  gdouble cur_distance = global->sonar.cur_distance - 10.0;

  if (distance_set (global, cur_distance))
    global->sonar.cur_distance = cur_distance;
}

static void
tvg_level_up (GtkWidget *widget,
              Global    *global)
{
  gdouble cur_tvg_level = global->sonar.cur_tvg_level + 0.1;

  cur_tvg_level = CLAMP (cur_tvg_level, 0.0, 1.0);

  if (tvg_set (global, cur_tvg_level, global->sonar.cur_tvg_sensitivity))
    global->sonar.cur_tvg_level = cur_tvg_level;
}

static void
tvg_level_down (GtkWidget *widget,
                Global    *global)
{
  gdouble cur_tvg_level = global->sonar.cur_tvg_level - 0.1;

  cur_tvg_level = CLAMP (cur_tvg_level, 0.0, 1.0);

  if (tvg_set (global, cur_tvg_level, global->sonar.cur_tvg_sensitivity))
    global->sonar.cur_tvg_level = cur_tvg_level;
}

static void
tvg_sensitivity_up (GtkWidget *widget,
                    Global    *global)
{
  gdouble cur_tvg_sensitivity = global->sonar.cur_tvg_sensitivity + 0.2;

  cur_tvg_sensitivity = CLAMP (cur_tvg_sensitivity, 0.0, 1.0);

  if (tvg_set (global, global->sonar.cur_tvg_level, cur_tvg_sensitivity))
    global->sonar.cur_tvg_sensitivity = cur_tvg_sensitivity;
}

static void
tvg_sensitivity_down (GtkWidget *widget,
                      Global    *global)
{
  gdouble cur_tvg_sensitivity = global->sonar.cur_tvg_sensitivity - 0.2;

  cur_tvg_sensitivity = CLAMP (cur_tvg_sensitivity, 0.0, 1.0);

  if (tvg_set (global, global->sonar.cur_tvg_level, cur_tvg_sensitivity))
    global->sonar.cur_tvg_sensitivity = cur_tvg_sensitivity;
}

static void
signal_up (GtkWidget *widget,
           Global    *global)
{
  guint cur_signal = global->sonar.cur_signal + 1;

  if (signal_set (global, cur_signal))
    global->sonar.cur_signal = cur_signal;
}

static void
signal_down (GtkWidget *widget,
             Global    *global)
{
  guint cur_signal = global->sonar.cur_signal - 1;

  if (signal_set (global, cur_signal))
    global->sonar.cur_signal = cur_signal;
}

static gboolean
start_stop (GtkWidget  *widget,
            gboolean    state,
            Global     *global)
{
  if (state)
    {
      static gint n_tracks = -1;
      gboolean status;

      /* Закрываем текущий открытый галс. */
      hyscan_gtk_waterfall_state_set_track (global->wf_state, NULL, NULL, NULL, FALSE);
      g_clear_pointer(&global->track_name, g_free);

      /* Параметры гидролокатора. */
      if (!signal_set (global, global->sonar.cur_signal) ||
          !tvg_set (global, global->sonar.cur_tvg_level, global->sonar.cur_tvg_sensitivity) ||
          !distance_set (global, global->sonar.cur_distance))
        {
          gtk_switch_set_active (GTK_SWITCH (widget), FALSE);
          return TRUE;
        }

      /* Число галсов в проекте. */
      if (n_tracks < 0)
        {
          gint32 project_id;
          gchar **tracks;

          project_id = hyscan_db_project_open (global->db, global->project_name);
          tracks = hyscan_db_track_list (global->db, project_id);
          n_tracks = (tracks == NULL) ? 0 : g_strv_length (tracks);

          hyscan_db_close (global->db, project_id);
          g_free (tracks);
        }

      /* Включаем запись нового галса. */
      global->track_name = g_strdup_printf ("%s%d", global->track_prefix, ++n_tracks);
      status = hyscan_sonar_control_start (global->sonar.sonar, global->track_name, HYSCAN_TRACK_SURVEY);

      /* Если локатор включён, открываем галс и переходим в режим онлайн. */
      if (status)
        {
          gtk_widget_set_sensitive (GTK_WIDGET (global->track_view), FALSE);
          gtk_switch_set_active (global->live_view, TRUE);
          gtk_switch_set_state (GTK_SWITCH (widget), TRUE);

          global->new_track = TRUE;
        }
      else
        {
          gtk_switch_set_active (GTK_SWITCH (widget), FALSE);
        }
    }
  else
    {
      if (gtk_switch_get_state (GTK_SWITCH (widget)))
        {
          hyscan_sonar_control_stop (global->sonar.sonar);

          gtk_switch_set_state (GTK_SWITCH (widget), FALSE);
          gtk_switch_set_active (global->live_view, FALSE);
          gtk_widget_set_sensitive (GTK_WIDGET (global->track_view), TRUE);
        }
    }

  return TRUE;
}

Global global = {0};

int
main (int    argc,
      char **argv)
{
  gint                 cache_size = 256;         /* Размер кэша по умолчанию. */
  gchar               *driver_path = NULL;       /* Путь к драйверам гидролокатора. */
  gchar               *driver_name = NULL;       /* Название драйвера гидролокатора. */
  gchar               *sonar_uri = NULL;         /* Адрес гидролокатора. */
  gchar               *db_uri = NULL;            /* Адрес базы данных. */
  gchar               *project_name = NULL;      /* Название проекта. */
  gchar               *track_prefix = NULL;      /* Префикс названия галсов. */
  gdouble              sound_velocity = 1500.0;  /* Скорость звука по умолчанию. */
  gdouble              ship_speed = 1.8;         /* Скорость движения судна. */
  gboolean             full_screen = FALSE;      /* Признак полноэкранного режима. */
  gchar               *config_file = NULL;       /* Название файла конфигурации. */

  HyScanSonarDriver   *driver = NULL;            /* Драйвер гидролокатора. */
  HyScanParam         *sonar = NULL;             /* Интерфейс управления локатором. */
  GArray              *svp = NULL;               /* Скорость звука. */
  HyScanSoundVelocity  svp_val;

  GtkBuilder          *builder = NULL;
  GtkWidget           *header = NULL;
  GtkWidget           *container = NULL;
  GtkWidget           *control = NULL;
  GtkWidget           *view_control = NULL;
  GtkWidget           *sonar_control = NULL;
  GtkWidget           *track_control = NULL;

  gtk_init (&argc, &argv);

  /* Разбор командной строки. */
  {
    gchar **args;
    GError *error = NULL;
    GOptionContext *context;

    GOptionEntry common_entries[] =
      {
        { "cache-size", 'c', 0, G_OPTION_ARG_INT, &cache_size, "Cache size, Mb", NULL },
        { "driver-path", 'a', 0, G_OPTION_ARG_STRING, &driver_path, "Path to sonar drivers", NULL },
        { "driver-name", 'n', 0, G_OPTION_ARG_STRING, &driver_name, "Sonar driver name", NULL },
        { "sonar-uri", 's', 0, G_OPTION_ARG_STRING, &sonar_uri, "Sonar uri", NULL },
        { "db-uri", 'd', 0, G_OPTION_ARG_STRING, &db_uri, "HyScan DB uri", NULL },
        { "project-name", 'p', 0, G_OPTION_ARG_STRING, &project_name, "Project name", NULL },
        { "track-prefix", 't', 0, G_OPTION_ARG_STRING, &track_prefix, "Track name prefix", NULL },
        { "sound-velocity", 'v', 0, G_OPTION_ARG_DOUBLE, &sound_velocity, "Sound velocity, m/s", NULL },
        { "ship-speed", 'e', 0, G_OPTION_ARG_DOUBLE, &ship_speed, "Ship speed, m/s", NULL },
        { "full-screen", 'f', 0, G_OPTION_ARG_NONE, &full_screen, "Full screen mode", NULL },
        { NULL }
      };

#ifdef G_OS_WIN32
    args = g_win32_get_command_line ();
#else
    args = g_strdupv (argv);
#endif

    context = g_option_context_new ("[config-file]");
    g_option_context_set_help_enabled (context, TRUE);
    g_option_context_set_ignore_unknown_options (context, FALSE);
    g_option_context_add_main_entries (context, common_entries, NULL);
    if (!g_option_context_parse_strv (context, &args, &error))
      {
        g_print ("%s\n", error->message);
        return -1;
      }

    if ((db_uri == NULL) || (project_name == NULL))
      {
        g_print ("%s", g_option_context_get_help (context, FALSE, NULL));
        return 0;
      }

    if (args[1] != NULL)
      config_file = g_strdup (args[1]);

    g_option_context_free (context);
    g_strfreev (args);
  }

  /* Путь к драйверам по умолчанию. */
  if ((driver_path == NULL) && (driver_name != NULL))
    driver_path = g_strdup (SONAR_DRIVERS_PATH);

  /* Префикс имени галса по умолчанию. */
  if (track_prefix == NULL)
    track_prefix = g_strdup ("SS");

  /* Конфигурация. */
  global.full_screen = full_screen;
  global.project_name = project_name;
  global.track_prefix = track_prefix;

  /* Кэш. */
  if (cache_size <= 0)
    cache_size = 256;
  global.cache = HYSCAN_CACHE (hyscan_cached_new (cache_size));

  /* Подключение к базе данных. */
  global.db = hyscan_db_new (db_uri);
  if (global.db == NULL)
    {
      g_message ("can't connect to db '%s'", db_uri);
      goto exit;
    }

  /* Монитор базы данных. */
  global.db_info = hyscan_db_info_new (global.db);

  /* Подключение к гидролокатору. */
  if (sonar_uri != NULL)
    {
      HyScanGeneratorModeType gen_cap;
      HyScanTVGModeType tvg_cap;
      HyScanSonarClient *client;
      GKeyFile *config;
      gboolean status;
      guint i;

      /* Подключение к гидролокатору с помощью HyScanSonarClient */
      if (driver_name == NULL)
        {

          client = hyscan_sonar_client_new (sonar_uri);
          if (client == NULL)
            {
              g_message ("can't connect to sonar '%s'", sonar_uri);
              goto exit;
            }

          if (!hyscan_sonar_client_set_master (client))
            {
              g_message ("can't set master mode on sonar '%s'", sonar_uri);
              g_object_unref (client);
              goto exit;
            }

          sonar = HYSCAN_PARAM (client);
        }

      /* Подключение с помощью драйвера гидролокатора. */
      else
        {
          driver = hyscan_sonar_driver_new (driver_path, driver_name);
          if (driver == NULL)
            {
              g_message ("can't load sonar driver '%s'", driver_name);
              goto exit;
            }

          sonar = hyscan_sonar_discover_connect (HYSCAN_SONAR_DISCOVER (driver), sonar_uri, NULL);
          if (sonar == NULL)
            {
              g_message ("can't connect to sonar '%s'", sonar_uri);
              goto exit;
            }
        }

      /* Управление локатором. */
      global.sonar.sonar = hyscan_sonar_control_new (sonar, 4, 4, global.db);
      if (global.sonar.sonar == NULL)
        {
          g_message ("unsupported sonar '%s'", sonar_uri);
          goto exit;
        }

      global.sonar.gen = HYSCAN_GENERATOR_CONTROL (global.sonar.sonar);
      global.sonar.tvg = HYSCAN_TVG_CONTROL (global.sonar.sonar);

      /* Параметры локатора - только сырые данные. */
      hyscan_param_set_enum (HYSCAN_PARAM (sonar), "/parameters/data-type", 0);
      hyscan_param_set_double (HYSCAN_PARAM (sonar), "/parameters/auto-tvg-max-cpu", 25.0);

      /* Параметры генераторов. */
      gen_cap = hyscan_generator_control_get_capabilities (global.sonar.gen,
                                                           HYSCAN_SOURCE_SIDE_SCAN_STARBOARD);
      if (!(gen_cap | HYSCAN_GENERATOR_MODE_PRESET))
        {
          g_message ("starboard: unsupported generator mode");
          status = FALSE;
          goto exit;
        }

      gen_cap = hyscan_generator_control_get_capabilities (global.sonar.gen,
                                                           HYSCAN_SOURCE_SIDE_SCAN_PORT);
      if (!(gen_cap | HYSCAN_GENERATOR_MODE_PRESET))
        {
          g_message ("port: unsupported generator mode");
          status = FALSE;
          goto exit;
        }

      status = hyscan_generator_control_set_enable (global.sonar.gen,
                                                    HYSCAN_SOURCE_SIDE_SCAN_STARBOARD,
                                                    TRUE);
      if (!status)
        {
          g_message ("starboard: can't enable generator");
          goto exit;
        }

      status = hyscan_generator_control_set_enable (global.sonar.gen,
                                                    HYSCAN_SOURCE_SIDE_SCAN_PORT,
                                                    TRUE);
      if (!status)
        {
          g_message ("port: can't enable generator");
          goto exit;
        }

      /* Параметры ВАРУ. */
      tvg_cap = hyscan_tvg_control_get_capabilities (global.sonar.tvg,
                                                     HYSCAN_SOURCE_SIDE_SCAN_STARBOARD);
      if (!(tvg_cap | HYSCAN_TVG_MODE_AUTO))
        {
          g_message ("starboard: unsupported tvg mode");
          status = FALSE;
          goto exit;
        }

      tvg_cap = hyscan_tvg_control_get_capabilities (global.sonar.tvg,
                                                     HYSCAN_SOURCE_SIDE_SCAN_PORT);
      if (!(tvg_cap | HYSCAN_TVG_MODE_AUTO))
        {
          g_message ("port: unsupported tvg mode");
          status = FALSE;
          goto exit;
        }

      status = hyscan_tvg_control_set_enable (global.sonar.tvg,
                                              HYSCAN_SOURCE_SIDE_SCAN_STARBOARD,
                                              TRUE);
      if (!status)
        {
          g_message ("starboard: can't enable tvg");
          goto exit;
        }

      status = hyscan_tvg_control_set_enable (global.sonar.tvg,
                                              HYSCAN_SOURCE_SIDE_SCAN_PORT,
                                              TRUE);
      if (!status)
        {
          g_message ("port: can't enable tvg");
          goto exit;
        }

      /* Сигналы зондирования. */
      global.sonar.starboard.signals = hyscan_generator_control_list_presets (global.sonar.gen,
                                                                              HYSCAN_SOURCE_SIDE_SCAN_STARBOARD);
      global.sonar.port.signals      = hyscan_generator_control_list_presets (global.sonar.gen,
                                                                              HYSCAN_SOURCE_SIDE_SCAN_PORT);

      if ((global.sonar.starboard.signals == NULL) || (global.sonar.port.signals == NULL))
        {
          g_message ("can't load signal presets");
          goto exit;
        }

      for (i = 0; global.sonar.starboard.signals[i] != NULL; i++);
      global.sonar.starboard.n_signals = i;

      for (i = 0; global.sonar.port.signals[i] != NULL; i++);
      global.sonar.port.n_signals = i;

      /* Настройка датчиков и антенн. */
      if (config_file != NULL)
        {
          config = g_key_file_new ();
          g_key_file_load_from_file (config, config_file, G_KEY_FILE_NONE, NULL);

          if (!setup_sensors (HYSCAN_SENSOR_CONTROL (global.sonar.sonar), config) ||
              !setup_sonar_antenna (global.sonar.sonar, HYSCAN_SOURCE_SIDE_SCAN_STARBOARD, config) ||
              !setup_sonar_antenna (global.sonar.sonar, HYSCAN_SOURCE_SIDE_SCAN_PORT, config))
            {
              status = FALSE;
            }
          else
            {
              status = TRUE;
            }

          g_key_file_unref (config);

          if (!status)
            goto exit;
        }

      /* Рабочий проект. */
      if (!hyscan_data_writer_set_project (HYSCAN_DATA_WRITER (global.sonar.sonar), project_name))
        {
          g_message ("can't set working project");
          goto exit;
        }
    }

  /* Элементы управления. */
  builder = gtk_builder_new_from_resource ("/org/hyscan/gtk/side-scan.ui");

  /* Управление просмотром. */
  view_control = GTK_WIDGET (gtk_builder_get_object (builder, "view_control"));
  if (view_control == NULL)
    {
      g_message ("can't load view control ui");
      goto exit;
    }

  global.brightness_value = GTK_LABEL (gtk_builder_get_object (builder, "brightness_value"));
  global.scale_value = GTK_LABEL (gtk_builder_get_object (builder, "scale_value"));
  global.color_map_value = GTK_LABEL (gtk_builder_get_object (builder, "color_map_value"));
  global.live_view = GTK_SWITCH (gtk_builder_get_object (builder, "live_view"));
  if ((global.brightness_value == NULL) ||
      (global.scale_value == NULL) ||
      (global.color_map_value == NULL) ||
      (global.live_view == NULL))
    {
      g_message ("incorrect view control ui");
      goto exit;
    }

  /* Управление локатором. */
  if (sonar != NULL)
    {
      sonar_control = GTK_WIDGET (gtk_builder_get_object (builder, "sonar_control"));
      if (sonar_control == NULL)
        {
          g_message ("can't load sonar control ui");
          goto exit;
        }

      global.start_stop = GTK_SWITCH (gtk_builder_get_object (builder, "start_stop"));
      global.distance_value = GTK_LABEL (gtk_builder_get_object (builder, "distance_value"));
      global.tvg_level_value = GTK_LABEL (gtk_builder_get_object (builder, "tvg_level_value"));
      global.tvg_sensitivity_value = GTK_LABEL (gtk_builder_get_object (builder, "tvg_sensitivity_value"));
      global.signal_value = GTK_LABEL (gtk_builder_get_object (builder, "signal_value"));

      if ((global.start_stop == NULL) ||
          (global.distance_value == NULL) ||
          (global.tvg_level_value == NULL) ||
          (global.tvg_sensitivity_value == NULL) ||
          (global.signal_value == NULL))
        {
          g_message ("incorrect sonar control ui");
          goto exit;
        }
    }

  /* Список галсов. */
  track_control = GTK_WIDGET (gtk_builder_get_object (builder, "track_control"));
  if (track_control == NULL)
    {
      g_message ("can't load track control ui");
      goto exit;
    }

  global.track_view = GTK_TREE_VIEW (gtk_builder_get_object (builder, "track_view"));
  global.track_list = GTK_TREE_MODEL (gtk_builder_get_object (builder, "track_list"));
  global.track_range = GTK_ADJUSTMENT (gtk_builder_get_object (builder, "track_range"));
  if ((global.track_view == NULL) ||
      (global.track_list == NULL) ||
      (global.track_range == NULL))
    {
      g_message ("incorrect track control ui");
      goto exit;
    }

  /* Сортировка списка галсов. */
  gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (global.track_list), 0, GTK_SORT_DESCENDING);

  /* Область управления. */
  control = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (control, FALSE);
  gtk_box_pack_start (GTK_BOX (control), view_control, FALSE, FALSE, 0);
  if (sonar != NULL)
    gtk_box_pack_end (GTK_BOX (control), sonar_control, FALSE, FALSE, 0);

  /* Основная раскладка окна. */
  container = hyscan_gtk_area_new ();

  /* Объект "водопад". */
  global.wf = HYSCAN_GTK_WATERFALL (hyscan_gtk_waterfall_new ());
  global.wf_state = HYSCAN_GTK_WATERFALL_STATE (global.wf);
  global.wf_grid = hyscan_gtk_waterfall_grid_new (global.wf);
  global.wf_control = hyscan_gtk_waterfall_control_new (global.wf);

  hyscan_gtk_waterfall_state_set_cache (global.wf_state, global.cache, global.cache, NULL);
  gtk_widget_set_hexpand (GTK_WIDGET (global.wf), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (global.wf), TRUE);
  gtk_widget_set_margin_top (GTK_WIDGET (global.wf), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (global.wf), 12);

  /* Цвет подложки. */
  hyscan_gtk_waterfall_set_substrate (HYSCAN_GTK_WATERFALL (global.wf),
                                      hyscan_tile_color_converter_d2i (0.0, 0.0, 0.0, 1.0));

  /* Скорость обновления экрана. */
  hyscan_gtk_waterfall_set_automove_period (global.wf, 100000);
  hyscan_gtk_waterfall_set_regeneration_period (global.wf, 500000);

  /* Устанавливаем скорости движения судна и скорость звука в воде. */
  svp = g_array_new (FALSE, FALSE, sizeof (HyScanSoundVelocity));
  svp_val.depth = 0.0;
  svp_val.velocity = sound_velocity;
  g_array_insert_val (svp, 0, svp_val);
  hyscan_gtk_waterfall_state_set_ship_speed (global.wf_state, ship_speed);
  hyscan_gtk_waterfall_state_set_sound_velocity (global.wf_state, svp);
  g_array_unref (svp);

  /* Основное окно программы. */
  global.window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title (GTK_WINDOW (global.window), "");
  gtk_window_set_default_size (GTK_WINDOW (global.window), 1024, 768);

  /* Заголовок окна. */
  header = gtk_header_bar_new ();
  gtk_header_bar_set_show_close_button (GTK_HEADER_BAR (header), TRUE);
  gtk_header_bar_set_title (GTK_HEADER_BAR (header), "Боковой обзор");
  gtk_window_set_titlebar (GTK_WINDOW (global.window), header);

  /* Разметка экрана. */
  hyscan_gtk_area_set_central (HYSCAN_GTK_AREA (container), GTK_WIDGET (global.wf));
  hyscan_gtk_area_set_left (HYSCAN_GTK_AREA (container), track_control);
  hyscan_gtk_area_set_right (HYSCAN_GTK_AREA (container), control);
  gtk_container_add (GTK_CONTAINER (global.window), container);

  /* Обработчики сигналов. */
  g_signal_connect (G_OBJECT (global.window), "destroy", G_CALLBACK (gtk_main_quit), NULL);
  g_signal_connect (G_OBJECT (global.window), "key-press-event", G_CALLBACK (key_press), &global);
  g_signal_connect (G_OBJECT (global.db_info), "projects-changed", G_CALLBACK (projects_changed), &global);
  g_signal_connect (G_OBJECT (global.db_info), "tracks-changed", G_CALLBACK (tracks_changed), &global);
  g_signal_connect (G_OBJECT (global.wf), "automove-state", G_CALLBACK (live_view_off), &global);
  g_signal_connect_swapped (G_OBJECT (global.wf), "waterfall-zoom", G_CALLBACK (scale_set), &global);

  gtk_builder_add_callback_symbol (builder, "track_scroll", G_CALLBACK (track_scroll));
  gtk_builder_add_callback_symbol (builder, "track_changed", G_CALLBACK (track_changed));

  gtk_builder_add_callback_symbol (builder, "brightness_up", G_CALLBACK (brightness_up));
  gtk_builder_add_callback_symbol (builder, "brightness_down", G_CALLBACK (brightness_down));
  gtk_builder_add_callback_symbol (builder, "color_map_up", G_CALLBACK (color_map_up));
  gtk_builder_add_callback_symbol (builder, "color_map_down", G_CALLBACK (color_map_down));
  gtk_builder_add_callback_symbol (builder, "scale_up", G_CALLBACK (scale_up));
  gtk_builder_add_callback_symbol (builder, "scale_down", G_CALLBACK (scale_down));
  gtk_builder_add_callback_symbol (builder, "live_view", G_CALLBACK (live_view));

  gtk_builder_add_callback_symbol (builder, "distance_up", G_CALLBACK (distance_up));
  gtk_builder_add_callback_symbol (builder, "distance_down", G_CALLBACK (distance_down));
  gtk_builder_add_callback_symbol (builder, "tvg_level_up", G_CALLBACK (tvg_level_up));
  gtk_builder_add_callback_symbol (builder, "tvg_level_down", G_CALLBACK (tvg_level_down));
  gtk_builder_add_callback_symbol (builder, "tvg_sensitivity_up", G_CALLBACK (tvg_sensitivity_up));
  gtk_builder_add_callback_symbol (builder, "tvg_sensitivity_down", G_CALLBACK (tvg_sensitivity_down));
  gtk_builder_add_callback_symbol (builder, "signal_up", G_CALLBACK (signal_up));
  gtk_builder_add_callback_symbol (builder, "signal_down", G_CALLBACK (signal_down));
  gtk_builder_add_callback_symbol (builder, "start_stop", G_CALLBACK (start_stop));
  gtk_builder_connect_signals (builder, &global);

  /* Начальные значения. */
  global.cur_brightness = 20;
  global.sonar.cur_signal = 1;
  global.sonar.cur_tvg_level = 0.5;
  global.sonar.cur_tvg_sensitivity = 0.6;
  global.sonar.cur_distance = SIDE_SCAN_MAX_DISTANCE;

  /* Цветовые палитры. */
  {
    guint i;

    global.color_maps[0] = g_array_sized_new (FALSE, FALSE, sizeof (guint32), 256);
    global.color_maps[1] = g_array_sized_new (FALSE, FALSE, sizeof (guint32), 256);
    global.color_maps[2] = g_array_sized_new (FALSE, FALSE, sizeof (guint32), 256);

    for (i = 0; i < 256; i++)
      {
        gdouble luminance = i / 255.0;
        guint32 color;

        color = hyscan_tile_color_converter_d2i (luminance, luminance, luminance, 1.0);
        g_array_insert_val (global.color_maps[0], i, color);

        color = hyscan_tile_color_converter_d2i (luminance, luminance, 0.0, 1.0);
        g_array_insert_val (global.color_maps[1], i, color);

        color = hyscan_tile_color_converter_d2i (0.0, luminance, 0.0, 1.0);
        g_array_insert_val (global.color_maps[2], i, color);
      }

    global.cur_color_map = 0;
  }

  color_map_set (&global, global.cur_color_map);
  brightness_set (&global, global.cur_brightness);
  if (sonar != NULL)
    {
      distance_set (&global, global.sonar.cur_distance);
      tvg_set (&global, global.sonar.cur_tvg_level, global.sonar.cur_tvg_sensitivity);
      signal_set (&global, global.sonar.cur_signal);
    }

  if (full_screen)
    gtk_window_fullscreen (GTK_WINDOW (global.window));

  gtk_widget_show_all (global.window);

  gtk_main ();

  if (sonar != NULL)
    hyscan_sonar_control_stop (global.sonar.sonar);

exit:
  g_clear_object (&builder);

  g_clear_object (&global.cache);
  g_clear_object (&global.db_info);
  g_clear_object (&global.db);

  g_clear_object (&global.wf);
  g_clear_object (&global.wf_grid);
  g_clear_object (&global.wf_control);

  g_clear_pointer (&global.sonar.starboard.signals, hyscan_data_schema_free_enum_values);
  g_clear_pointer (&global.sonar.port.signals, hyscan_data_schema_free_enum_values);
  g_clear_object (&global.sonar.sonar);
  g_clear_object (&sonar);
  g_clear_object (&driver);

  g_free (global.track_name);

  g_free (driver_path);
  g_free (driver_name);
  g_free (sonar_uri);
  g_free (db_uri);
  g_free (project_name);
  g_free (track_prefix);
  g_free (config_file);

  return 0;
}
