/*
 * Copyright (c) 2018-2019, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "deepstream_app.h"
#include "deepstream_config_file_parser.h"
#include "nvds_version.h"
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
// jayden.choe
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <pthread.h>
#include <stdio.h>
#include <semaphore.h>
#include <sys/types.h> 
#include <sys/stat.h> 
#include <fcntl.h>
////////////////////////////

#define MAX_INSTANCES 128
#define APP_TITLE "DeepStream"

#define DEFAULT_X_WINDOW_WIDTH 1920
#define DEFAULT_X_WINDOW_HEIGHT 1080

// jayden.choe
void python_test( void );
void init_python3 (char *argv[] );
void end_python3 ( void );
void call_python3_command( char *p_command_string );
void call_python3_file ( char *p_filename );
void *thread_a ( void* pArg );
///////////////////////////////////////////

AppCtx *appCtx[MAX_INSTANCES];
static guint cintr = FALSE;
static GMainLoop *main_loop = NULL;
static gchar **cfg_files = NULL;
static gchar **input_files = NULL;
static gboolean print_version = FALSE;
static gboolean show_bbox_text = FALSE;
static gboolean print_dependencies_version = FALSE;
static gboolean quit = FALSE;
static gint return_value = 0;
static guint num_instances;
static guint num_input_files;
static GMutex fps_lock;
static gdouble fps[MAX_SOURCE_BINS];
static gdouble fps_avg[MAX_SOURCE_BINS];
static guint num_fps_inst = 0;

static Display *display = NULL;
static Window windows[MAX_INSTANCES] = { 0 };

static gint source_ids[MAX_INSTANCES];

static GThread *x_event_thread = NULL;
static GMutex disp_lock;

// jayden.choe
static int s_b_terminate_thread = FALSE;
static sem_t sem_one;
static sem_t sem_two;
static int s_human_x = 0;
static int s_human_y = 0;

GST_DEBUG_CATEGORY (NVDS_APP);

GOptionEntry entries[] = {
  {"version", 'v', 0, G_OPTION_ARG_NONE, &print_version,
      "Print DeepStreamSDK version", NULL}
  ,
  {"tiledtext", 't', 0, G_OPTION_ARG_NONE, &show_bbox_text,
      "Display Bounding box labels in tiled mode", NULL}
  ,
  {"version-all", 0, 0, G_OPTION_ARG_NONE, &print_dependencies_version,
      "Print DeepStreamSDK and dependencies version", NULL}
  ,
  {"cfg-file", 'c', 0, G_OPTION_ARG_FILENAME_ARRAY, &cfg_files,
      "Set the config file", NULL}
  ,
  {"input-file", 'i', 0, G_OPTION_ARG_FILENAME_ARRAY, &input_files,
      "Set the input file", NULL}
  ,
  {NULL}
  ,
};

/**
 * Callback function to be called once all inferences (Primary + Secondary)
 * are done. This is opportunity to modify content of the metadata.
 * e.g. Here Person is being replaced with Man/Woman and corresponding counts
 * are being maintained. It should be modified according to network classes
 * or can be removed altogether if not required.
 */
static void
all_bbox_generated (AppCtx * appCtx, GstBuffer * buf,
    NvDsBatchMeta * batch_meta, guint index)
{
  guint num_male = 0;
  guint num_female = 0;
  guint num_objects[128];
  // jayden.choe
  guint center_x = 0;
  guint center_y = 0;

 // g_print( "all_bbox_generated started\n" );

  memset (num_objects, 0, sizeof (num_objects));

  for (NvDsMetaList * l_frame = batch_meta->frame_meta_list; l_frame != NULL;
      l_frame = l_frame->next) {
    NvDsFrameMeta *frame_meta = l_frame->data;
    for (NvDsMetaList * l_obj = frame_meta->obj_meta_list; l_obj != NULL;
        l_obj = l_obj->next) {
      NvDsObjectMeta *obj = (NvDsObjectMeta *) l_obj->data;
      if (obj->unique_component_id ==
          (gint) appCtx->config.primary_gie_config.unique_id) {
        if (obj->class_id >= 0 && obj->class_id < 128) {
          num_objects[obj->class_id]++;
        }
          // jayden.choe
        center_x = (obj->rect_params.left + obj->rect_params.width) / 2;
        center_y = (obj->rect_params.top + obj->rect_params.height) / 2; 
        g_printf( "c-id: %d, center x: %d, center y: %d\n", obj->class_id, center_x, center_y );
        s_human_x = center_x;
        s_human_y = center_y;
        
        // jayden.choe below condition never fit        
        if (appCtx->person_class_id > -1
            && obj->class_id == appCtx->person_class_id) {
          if (strstr (obj->text_params.display_text, "Man")) {
            str_replace (obj->text_params.display_text, "Man", "");
            str_replace (obj->text_params.display_text, "Person", "Man");
            num_male++;
          } else if (strstr (obj->text_params.display_text, "Woman")) {
            str_replace (obj->text_params.display_text, "Woman", "");
            str_replace (obj->text_params.display_text, "Person", "Woman");
            num_female++;
          }
        }
      }
    }
  }
}

/**
 * Function to handle program interrupt signal.
 * It installs default handler after handling the interrupt.
 */
static void
_intr_handler (int signum)
{
  struct sigaction action;

  NVGSTDS_ERR_MSG_V ("User Interrupted.. \n");

  memset (&action, 0, sizeof (action));
  action.sa_handler = SIG_DFL;

  sigaction (SIGINT, &action, NULL);

  cintr = TRUE;
}

/**
 * callback function to print the performance numbers of each stream.
 */
static void
perf_cb (gpointer context, NvDsAppPerfStruct * str)
{
  static guint header_print_cnt = 0;
  guint i;
  AppCtx *appCtx = (AppCtx *) context;
  guint numf = (num_instances == 1) ? str->num_instances : num_instances;

//  g_print( "perf_cb started\n" );

  g_mutex_lock (&fps_lock);
  if (num_instances > 1) {
    fps[appCtx->index] = str->fps[0];
    fps_avg[appCtx->index] = str->fps_avg[0];
  } else {
    for (i = 0; i < numf; i++) {
      fps[i] = str->fps[i];
      fps_avg[i] = str->fps_avg[i];
    }
  }

  num_fps_inst++;
  if (num_fps_inst < num_instances) {
    g_mutex_unlock (&fps_lock);
    return;
  }

  num_fps_inst = 0;

  if (header_print_cnt % 20 == 0) {
    g_print ("\n**PERF: ");
    for (i = 0; i < numf; i++) {
      g_print ("FPS %d (Avg)\t", i);
    }
    g_print ("\n");
    header_print_cnt = 0;
  }
  header_print_cnt++;
  g_print ("**PERF: ");
  for (i = 0; i < numf; i++) {
    g_print ("%.2f (%.2f)\t", fps[i], fps_avg[i]);
  }
  g_print ("\n");
  g_mutex_unlock (&fps_lock);
}

/**
 * Loop function to check the status of interrupts.
 * It comes out of loop if application got interrupted.
 */
static gboolean
check_for_interrupt (gpointer data)
{
  if (quit) {
    return FALSE;
  }

  if (cintr) {
    cintr = FALSE;

    quit = TRUE;
    g_main_loop_quit (main_loop);

    return FALSE;
  }
  return TRUE;
}

/*
 * Function to install custom handler for program interrupt signal.
 */
static void
_intr_setup (void)
{
  struct sigaction action;

  memset (&action, 0, sizeof (action));
  action.sa_handler = _intr_handler;

  sigaction (SIGINT, &action, NULL);
}

static gboolean
kbhit (void)
{
  struct timeval tv;
  fd_set rdfs;

  tv.tv_sec = 0;
  tv.tv_usec = 0;

  FD_ZERO (&rdfs);
  FD_SET (STDIN_FILENO, &rdfs);

  select (STDIN_FILENO + 1, &rdfs, NULL, NULL, &tv);
  return FD_ISSET (STDIN_FILENO, &rdfs);
}

/*
 * Function to enable / disable the canonical mode of terminal.
 * In non canonical mode input is available immediately (without the user
 * having to type a line-delimiter character).
 */
static void
changemode (int dir)
{
  static struct termios oldt, newt;

  if (dir == 1) {
    tcgetattr (STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON);
    tcsetattr (STDIN_FILENO, TCSANOW, &newt);
  } else
    tcsetattr (STDIN_FILENO, TCSANOW, &oldt);
}

static void
print_runtime_commands (void)
{
  g_print ("\nRuntime commands:\n"
      "\th: Print this help\n"
      "\tq: Quit\n\n" "\tp: Pause\n" "\tr: Resume\n\n");

  if (appCtx[0]->config.tiled_display_config.enable) {
    g_print
        ("NOTE: To expand a source in the 2D tiled display and view object details,"
        " left-click on the source.\n"
        "      To go back to the tiled display, right-click anywhere on the window.\n\n");
  }
}

static guint rrow, rcol;
static gboolean rrowsel = FALSE, selecting = FALSE;

/**
 * Loop function to check keyboard inputs and status of each pipeline.
 */
static gboolean
event_thread_func (gpointer arg)
{
  guint i;
  gboolean ret = TRUE;

  // Check if all instances have quit
  for (i = 0; i < num_instances; i++) {
    if (!appCtx[i]->quit)
      break;
  }

  if (i == num_instances) {
    quit = TRUE;
    g_main_loop_quit (main_loop);
    return FALSE;
  }
  // Check for keyboard input
  if (!kbhit ()) {
    //continue;
    return TRUE;
  }
  int c = fgetc (stdin);
  g_print ("\n");

  gint source_id;
  GstElement *tiler = appCtx[0]->pipeline.tiled_display_bin.tiler;
  g_object_get (G_OBJECT (tiler), "show-source", &source_id, NULL);

  if (selecting) {
    if (rrowsel == FALSE) {
      if (c >= '0' && c <= '9') {
        rrow = c - '0';
        if (rrow < appCtx[0]->config.tiled_display_config.rows){
          g_print ("--selecting source  row %d--\n", rrow);
          rrowsel = TRUE;
        }else{
          g_print ("--selected source  row %d out of bound, reenter\n", rrow);
        }
      }
    } else {
      if (c >= '0' && c <= '9') {
        unsigned int tile_num_columns = appCtx[0]->config.tiled_display_config.columns;
        rcol = c - '0';
        if (rcol < tile_num_columns){
          selecting = FALSE;
          rrowsel = FALSE;
          source_id = tile_num_columns * rrow + rcol;
          g_print ("--selecting source  col %d sou=%d--\n", rcol, source_id);
          if (source_id >= (gint) appCtx[0]->config.num_source_sub_bins) {
            source_id = -1;
          } else {
            source_ids[0] = source_id;
            appCtx[0]->show_bbox_text = TRUE;
            g_object_set (G_OBJECT (tiler), "show-source", source_id, NULL);
          }
        }else{
          g_print ("--selected source  col %d out of bound, reenter\n", rcol);
        }
     }
    }
  }
  switch (c) {
    case 'h':
      print_runtime_commands ();
      break;
    case 'p':
      for (i = 0; i < num_instances; i++)
        pause_pipeline (appCtx[i]);
      break;
    case 'r':
      for (i = 0; i < num_instances; i++)
        resume_pipeline (appCtx[i]);
      break;
    case 'q':
      quit = TRUE;
      g_main_loop_quit (main_loop);
      ret = FALSE;
      break;
    case 'z':
      if (source_id == -1 && selecting == FALSE) {
        g_print ("--selecting source --\n");
        selecting = TRUE;
      } else {
        if (!show_bbox_text)
          appCtx[0]->show_bbox_text = FALSE;
        g_object_set (G_OBJECT (tiler), "show-source", -1, NULL);
        source_ids[0] = -1;
        selecting = FALSE;
        g_print ("--tiled mode --\n");
      }
      break;
    default:
      break;
  }
  return ret;
}

static int
get_source_id_from_coordinates (float x_rel, float y_rel)
{
  int tile_num_rows = appCtx[0]->config.tiled_display_config.rows;
  int tile_num_columns = appCtx[0]->config.tiled_display_config.columns;

  int source_id = (int) (x_rel * tile_num_columns);
  source_id += ((int) (y_rel * tile_num_rows)) * tile_num_columns;

  /* Don't allow clicks on empty tiles. */
  if (source_id >= (gint) appCtx[0]->config.num_source_sub_bins)
    source_id = -1;

  return source_id;
}

/**
 * Thread to monitor X window events.
 */
static gpointer
nvds_x_event_thread (gpointer data)
{
  g_mutex_lock (&disp_lock);
  while (display) {
    XEvent e;
    guint index;
    while (XPending (display)) {
      XNextEvent (display, &e);
      switch (e.type) {
        case ButtonPress:
        {
          XWindowAttributes win_attr;
          XButtonEvent ev = e.xbutton;
          gint source_id;
          GstElement *tiler;

          XGetWindowAttributes (display, ev.window, &win_attr);

          for (index = 0; index < MAX_INSTANCES; index++)
            if (ev.window == windows[index])
              break;

          tiler = appCtx[index]->pipeline.tiled_display_bin.tiler;
          g_object_get (G_OBJECT (tiler), "show-source", &source_id, NULL);

          if (ev.button == Button1 && source_id == -1) {
            source_id =
                get_source_id_from_coordinates (ev.x * 1.0 / win_attr.width,
                ev.y * 1.0 / win_attr.height);
            if (source_id > -1) {
              g_object_set (G_OBJECT (tiler), "show-source", source_id, NULL);
              source_ids[index] = source_id;
              appCtx[index]->show_bbox_text = TRUE;
            }
          } else if (ev.button == Button3) {
            g_object_set (G_OBJECT (tiler), "show-source", -1, NULL);
            source_ids[index] = -1;
            if (!show_bbox_text)
              appCtx[index]->show_bbox_text = FALSE;
          }
        }
          break;
        case KeyRelease:
        case KeyPress:
        {
          KeySym p, r, q;
          guint i;
          p = XKeysymToKeycode (display, XK_P);
          r = XKeysymToKeycode (display, XK_R);
          q = XKeysymToKeycode (display, XK_Q);
          if (e.xkey.keycode == p) {
            for (i = 0; i < num_instances; i++)
              pause_pipeline (appCtx[i]);
            break;
          }
          if (e.xkey.keycode == r) {
            for (i = 0; i < num_instances; i++)
              resume_pipeline (appCtx[i]);
            break;
          }
          if (e.xkey.keycode == q) {
            quit = TRUE;
            g_main_loop_quit (main_loop);
          }
        }
          break;
        case ClientMessage:
        {
          Atom wm_delete;
          for (index = 0; index < MAX_INSTANCES; index++)
            if (e.xclient.window == windows[index])
              break;

          wm_delete = XInternAtom (display, "WM_DELETE_WINDOW", 1);
          if (wm_delete != None && wm_delete == (Atom) e.xclient.data.l[0]) {
            quit = TRUE;
            g_main_loop_quit (main_loop);
          }
        }
          break;
      }
    }
    g_mutex_unlock (&disp_lock);
    g_usleep (G_USEC_PER_SEC / 20);
    g_mutex_lock (&disp_lock);
  }
  g_mutex_unlock (&disp_lock);
  return NULL;
}

/**
 * callback function to add application specific metadata.
 * Here it demonstrates how to display the URI of source in addition to
 * the text generated after inference.
 */
static gboolean
overlay_graphics (AppCtx * appCtx, GstBuffer * buf,
    NvDsBatchMeta * batch_meta, guint index)
{

  // g_print( "overlay_graphics started\n" );

  if (source_ids[index] == -1)
    return TRUE;

  NvDsFrameLatencyInfo *latency_info = NULL;
  NvDsDisplayMeta *display_meta =
      nvds_acquire_display_meta_from_pool (batch_meta);

  display_meta->num_labels = 1;
  display_meta->text_params[0].display_text = g_strdup_printf ("Source: %s",
      appCtx->config.multi_source_config[source_ids[index]].uri);

  display_meta->text_params[0].y_offset = 20;
  display_meta->text_params[0].x_offset = 20;
  display_meta->text_params[0].font_params.font_color = (NvOSD_ColorParams) {
  0, 1, 0, 1};
  display_meta->text_params[0].font_params.font_size =
      appCtx->config.osd_config.text_size * 1.5;
  display_meta->text_params[0].font_params.font_name = "Serif";
  display_meta->text_params[0].set_bg_clr = 1;
  display_meta->text_params[0].text_bg_clr = (NvOSD_ColorParams) {
  0, 0, 0, 1.0};


  if(nvds_enable_latency_measurement) {
    g_mutex_lock (&appCtx->latency_lock);
    latency_info = &appCtx->latency_info[index];
    display_meta->num_labels++;
    display_meta->text_params[1].display_text = g_strdup_printf ("Latency: %lf",
        latency_info->latency);
    g_mutex_unlock (&appCtx->latency_lock);

    display_meta->text_params[1].y_offset = (display_meta->text_params[0].y_offset * 2 )+
      display_meta->text_params[0].font_params.font_size;
    display_meta->text_params[1].x_offset = 20;
    display_meta->text_params[1].font_params.font_color = (NvOSD_ColorParams) {
      0, 1, 0, 1};
    display_meta->text_params[1].font_params.font_size =
      appCtx->config.osd_config.text_size * 1.5;
    display_meta->text_params[1].font_params.font_name = "Arial";
    display_meta->text_params[1].set_bg_clr = 1;
    display_meta->text_params[1].text_bg_clr = (NvOSD_ColorParams) {
      0, 0, 0, 1.0};
  }

  nvds_add_display_meta_to_frame (nvds_get_nth_frame_meta (batch_meta->
          frame_meta_list, 0), display_meta);
  return TRUE;
}

int
main (int argc, char *argv[])
{
  GOptionContext *ctx = NULL;
  GOptionGroup *group = NULL;
  GError *error = NULL;
  guint i;

// jayden.choe
  pthread_t tA;
  int thread_err = 0;
  sem_init(&sem_one, 0, 0);
  sem_init(&sem_two, 0, 1);
  


// jayden.choe
  init_python3( (void*) argv );
  python_test( );
  gpio_export(14);
  gpio_set_outdir(14, 1);
  gpio_set(14, 1);
  
  thread_err = pthread_create(&tA, NULL, thread_a, (void*)argv );
  if (thread_err != 0) {
    g_printf( "thread A create fail: %d\n", thread_err);
  }
////////////////////////////////////////////////////////////////
  ctx = g_option_context_new ("Nvidia DeepStream Demo");
  group = g_option_group_new ("abc", NULL, NULL, NULL, NULL);
  g_option_group_add_entries (group, entries);

  g_option_context_set_main_group (ctx, group);
  
  g_option_context_add_group (ctx, gst_init_get_option_group ());

  GST_DEBUG_CATEGORY_INIT (NVDS_APP, "NVDS_APP", 0, NULL);

  if (!g_option_context_parse (ctx, &argc, &argv, &error)) {
    NVGSTDS_ERR_MSG_V ("%s", error->message);
    return -1;
  }

  if (print_version) {
    g_print ("deepstream-app version %d.%d.%d\n",
        NVDS_APP_VERSION_MAJOR, NVDS_APP_VERSION_MINOR, NVDS_APP_VERSION_MICRO);
    nvds_version_print ();
    return 0;
  }

  if (print_dependencies_version) {
    g_print ("deepstream-app version %d.%d.%d\n",
        NVDS_APP_VERSION_MAJOR, NVDS_APP_VERSION_MINOR, NVDS_APP_VERSION_MICRO);
    nvds_version_print ();
    nvds_dependencies_version_print ();
    return 0;
  }

  if (cfg_files) {
    num_instances = g_strv_length (cfg_files);
  }
  if (input_files) {
    num_input_files = g_strv_length (input_files);
  }

  memset (source_ids, -1, sizeof (source_ids));

  if (!cfg_files || num_instances == 0) {
    NVGSTDS_ERR_MSG_V ("Specify config file with -c option");
    return_value = -1;
    goto done;
  }

  for (i = 0; i < num_instances; i++) {
    appCtx[i] = g_malloc0 (sizeof (AppCtx));
    appCtx[i]->person_class_id = -1;
    appCtx[i]->car_class_id = -1;
    appCtx[i]->index = i;
    if (show_bbox_text) {
      appCtx[i]->show_bbox_text = TRUE;
    }

    if (input_files && input_files[i]) {
      appCtx[i]->config.multi_source_config[0].uri =
          g_strdup_printf ("file://%s", input_files[i]);
      g_free (input_files[i]);
    }

    if (!parse_config_file (&appCtx[i]->config, cfg_files[i])) {
      NVGSTDS_ERR_MSG_V ("Failed to parse config file '%s'", cfg_files[i]);
      appCtx[i]->return_value = -1;
      goto done;
    }
  }

  for (i = 0; i < num_instances; i++) {
    if (!create_pipeline (appCtx[i], NULL,
            all_bbox_generated, perf_cb, overlay_graphics)) {
      NVGSTDS_ERR_MSG_V ("Failed to create pipeline");
      return_value = -1;
      goto done;
    }
  }

  main_loop = g_main_loop_new (NULL, FALSE);

  _intr_setup ();
  g_timeout_add (400, check_for_interrupt, NULL);


  g_mutex_init (&disp_lock);
  display = XOpenDisplay (NULL);
  for (i = 0; i < num_instances; i++) {
    guint j;

    if (gst_element_set_state (appCtx[i]->pipeline.pipeline,
            GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
      NVGSTDS_ERR_MSG_V ("Failed to set pipeline to PAUSED");
      return_value = -1;
      goto done;
    }

    if (!appCtx[i]->config.tiled_display_config.enable)
      continue;

    for (j = 0; j < appCtx[i]->config.num_sink_sub_bins; j++) {
      XTextProperty xproperty;
      gchar *title;
      guint width, height;

      if (!GST_IS_VIDEO_OVERLAY (appCtx[i]->pipeline.instance_bins[0].
              sink_bin.sub_bins[j].sink)) {
        continue;
      }

      if (!display) {
        NVGSTDS_ERR_MSG_V ("Could not open X Display");
        return_value = -1;
        goto done;
      }

      if (appCtx[i]->config.sink_bin_sub_bin_config[j].render_config.width)
        width =
            appCtx[i]->config.sink_bin_sub_bin_config[j].render_config.width;
      else
        width = appCtx[i]->config.tiled_display_config.width;

      if (appCtx[i]->config.sink_bin_sub_bin_config[j].render_config.height)
        height =
            appCtx[i]->config.sink_bin_sub_bin_config[j].render_config.height;
      else
        height = appCtx[i]->config.tiled_display_config.height;

      width = (width) ? width : DEFAULT_X_WINDOW_WIDTH;
      height = (height) ? height : DEFAULT_X_WINDOW_HEIGHT;

      windows[i] =
          XCreateSimpleWindow (display, RootWindow (display,
              DefaultScreen (display)), 0, 0, width, height, 2, 0x00000000,
          0x00000000);

      if (num_instances > 1)
        title = g_strdup_printf (title, APP_TITLE "-%d", i);
      else
        title = g_strdup (APP_TITLE);
      if (XStringListToTextProperty ((char **) &title, 1, &xproperty) != 0) {
        XSetWMName (display, windows[i], &xproperty);
        XFree (xproperty.value);
      }

      XSetWindowAttributes attr = { 0 };
      if ((appCtx[i]->config.tiled_display_config.enable &&
              appCtx[i]->config.tiled_display_config.rows *
              appCtx[i]->config.tiled_display_config.columns == 1) ||
          (appCtx[i]->config.tiled_display_config.enable == 0 &&
              appCtx[i]->config.num_source_sub_bins == 1)) {
        attr.event_mask = KeyPress;
      } else {
        attr.event_mask = ButtonPress | KeyRelease;
      }
      XChangeWindowAttributes (display, windows[i], CWEventMask, &attr);

      Atom wmDeleteMessage = XInternAtom (display, "WM_DELETE_WINDOW", False);
      if (wmDeleteMessage != None) {
        XSetWMProtocols (display, windows[i], &wmDeleteMessage, 1);
      }
      XMapRaised (display, windows[i]);
      XSync (display, 1);       //discard the events for now
      gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (appCtx
              [i]->pipeline.instance_bins[0].sink_bin.sub_bins[j].sink),
          (gulong) windows[i]);
      gst_video_overlay_expose (GST_VIDEO_OVERLAY (appCtx[i]->
              pipeline.instance_bins[0].sink_bin.sub_bins[j].sink));
      if (!x_event_thread)
        x_event_thread = g_thread_new ("nvds-window-event-thread",
            nvds_x_event_thread, NULL);
    }
  }

  /* Dont try to set playing state if error is observed */
  if (return_value != -1) {
    for (i = 0; i < num_instances; i++) {
      if (gst_element_set_state (appCtx[i]->pipeline.pipeline,
              GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {

        g_print ("\ncan't set pipeline to playing state.\n");
        return_value = -1;
        goto done;
      }
    }
  }

  print_runtime_commands ();

  changemode (1);

  g_timeout_add (40, event_thread_func, NULL);
  g_main_loop_run (main_loop);

  changemode (0);

done:

  g_print ("Quitting\n");
  for (i = 0; i < num_instances; i++) {
    if (appCtx[i]->return_value == -1)
      return_value = -1;
    destroy_pipeline (appCtx[i]);

    g_mutex_lock (&disp_lock);
    if (windows[i])
      XDestroyWindow (display, windows[i]);
    windows[i] = 0;
    g_mutex_unlock (&disp_lock);

    g_free (appCtx[i]);
  }

  g_mutex_lock (&disp_lock);
  if (display)
    XCloseDisplay (display);
  display = NULL;
  g_mutex_unlock (&disp_lock);
  g_mutex_clear (&disp_lock);

  if (main_loop) {
    g_main_loop_unref (main_loop);
  }

  if (ctx) {
    g_option_context_free (ctx);
  }

  if (return_value == 0) {
    g_print ("App run successful\n");
  } else {
    g_print ("App run failed\n");
  }

  gst_deinit ();

// jayden.choe
  s_b_terminate_thread = TRUE;
  thread_err = pthread_join(tA, NULL);
  if ( thread_err != 0) {
    g_print("thread A Join fail : %d\n", thread_err);
  }

  sem_destroy(&sem_one);
  sem_destroy(&sem_two);
  end_python3();
//////////////////////////////////////////////
  return return_value;
}

// jayden.choe
wchar_t *g_p_program = NULL;

void python_test( void ) {


  call_python3_command( "from time import time,ctime\n"
                      "print('Today is', ctime(time()))\n" );
  //call_python3_file( "adjust_camera_to_center.py");
//  call_python3_command_camera_to_center();
   call_python3_command_camera_to_front();
   call_python3_command_3beep();
   call_python3_command_move_down();
 //  call_python3_command_move_down();
 //  call_python3_command_move_down();
// call_python3_command_move_down();
 // call_python3_file( "adjust_camera_to_front.py");
 // call_python3_command_move_forward();
 // call_python3_command_move_backward();
 // call_python3_command_move_right();
 // call_python3_command_move_right();
 // call_python3_command_move_forward();
  //call_python3_command_move_left();
 // call_python3_command_move_forward();
 // call_python3_command_move_right();
 // call_python3_command_move_forward();
 // call_python3_command_move_left();
 // call_python3_command_move_forward();
 

}

void init_python3 (char *argv[] ) {
  g_p_program = Py_DecodeLocale(argv[0], NULL);
  if (g_p_program == NULL) {
      fprintf(stderr, "Fatal error: cannot decode argv[0]\n");
      exit(1);
  }
  Py_SetProgramName(g_p_program);  /* optional but recommended */
 
  Py_Initialize();
  g_printf( "Current folder has been appended to sys.path\n" );    
  PyObject *sys_path = PySys_GetObject("path");
  PyList_Append(sys_path, PyUnicode_FromString("/home/jetbot/deepstream_sdk_v4.0.2_jetson/sources/apps/sample_apps/deepstream-app"));
  PyList_Append(sys_path, PyUnicode_FromString("/home/jetbot/yahboom-jetbot"));
}

void end_python3 ( void ) {
  g_printf ( "end_python3\n");
  if ( g_p_program != NULL ) {
    PyMem_RawFree(g_p_program);
  }
}

void call_python3_command_camera_to_center( void ) {
  PyRun_SimpleString( "from servoserial import ServoSerial\n"
                      "servo_device = ServoSerial()\n"
                      "servo_device.Servo_serial_double_control(1, 2100, 2, 2048)\n");  
}

void call_python3_command_camera_to_front( void ) {
  PyRun_SimpleString( "from servoserial import ServoSerial\n"
                      "servo_device = ServoSerial()\n"
                      "servo_device.Servo_serial_double_control(1, 2100, 2, 1500)\n");  
}

void call_python3_command_move_up( void ) {
  PyRun_SimpleString( "from jetbot import Robot\n"
                      "import time\n"
                      "robot = Robot()\n"
                      "robot.up(1)\n"
                      "time.sleep(1.0)\n"
                      "robot.vertical_motors_stop()" );  
}

void call_python3_command_move_down( void ) {
  PyRun_SimpleString( "from jetbot import Robot\n"
                      "import time\n"  
                      "robot = Robot()\n"
                      "robot.down(1)\n"
                      "time.sleep(1.0)\n"
                      "robot.vertical_motors_stop()" );  
}

void call_python3_command_move_forward( void ) {
  PyRun_SimpleString( "from jetbot import Robot\n"
                      "import time\n"
                      "robot = Robot()\n"
                      "robot.forward(0.8)\n"
                      "time.sleep(0.5)\n"
                      "robot.stop()\n" );  
}

void call_python3_command_move_backward( void ) {
  PyRun_SimpleString( "from jetbot import Robot\n"
                      "import time\n"
                      "robot = Robot()\n"
                      "robot.backward(0.8)\n"
                      "time.sleep(0.5)\n"
                      "robot.stop()\n" );  
}

void call_python3_command_move_left( void ) {
  PyRun_SimpleString( "from jetbot import Robot\n"
                      "import time\n"
                      "robot = Robot()\n"  
                      "robot.left(0.7)\n"
                      "time.sleep(0.5)\n"
                      "robot.stop()\n" );  
}

void call_python3_command_move_right( void ) {
  PyRun_SimpleString( "from jetbot import Robot\n"
                      "import time\n"
                      "robot = Robot()\n"  
                      "robot.right(0.5)\n"
                      "time.sleep(0.5)\n"
                      "robot.stop()\n" );  
}

void call_python3_command_move_stop( void ) {
  PyRun_SimpleString( "from jetbot import Robot\n"
                      "import time\n"
                      "robot = Robot()\n"
                      "robot.stop()\n" );  
}

void call_python3_command_sleep( void ) {
  PyRun_SimpleString( "import time\n"
                      "time.sleep(0.5)\n" );
}

void call_python3_command_3beep( void ) {
  PyRun_SimpleString( "import RPi.GPIO as GPIO\n"
                      "import time\n"
                      "BEEP_pin = 6\n"
                      "GPIO.setmode(GPIO.BCM)\n" 
                      "GPIO.setup(BEEP_pin, GPIO.OUT, initial=GPIO.LOW)\n"
                      "GPIO.output(BEEP_pin, GPIO.HIGH)\n"
                      "time.sleep(0.1)\n"
                      "GPIO.output(BEEP_pin, GPIO.LOW)\n"
                      "time.sleep(0.2)\n"
                      "GPIO.output(BEEP_pin, GPIO.HIGH)\n"
                      "time.sleep(0.1)\n"
                      "GPIO.output(BEEP_pin, GPIO.LOW)\n"
                      "time.sleep(0.2)\n"
                      "GPIO.output(BEEP_pin, GPIO.HIGH)\n"
                      "time.sleep(0.1)\n"
                      "GPIO.output(BEEP_pin, GPIO.LOW)\n"
                      "time.sleep(0.2)\n"
                      );
}

void call_python3_command( char *p_command_string ) {

  PyRun_SimpleString(p_command_string);
  return;
}

void call_python3_file ( char *p_filename ) {
  PyObject *obj = Py_BuildValue("s", p_filename );
  FILE *fp = _Py_fopen_obj(obj, "r+");
  if(!fp) {
    fprintf(stderr, "Error: Could not open file '%s'\n", p_filename);    
    exit(1);
  }
 
  g_printf( "PyRun_SimpleFile\n");
  if(PyRun_SimpleFile(fp, p_filename) == 0) {
    g_printf("Problem running script file '%s'\n", p_filename);
    exit(1);
  }
  g_printf ( "fclose\n");
  fclose(fp);  
}

int gpio_export(int gpio)
{
 int fd = 0;
 int len = 0;
 char buf[64] = { 0, };

  len = snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d", gpio);
 if (0 == access(buf, F_OK))
 {
 return 0;
 }

  fd = open("/sys/class/gpio/export", O_WRONLY);
 if (fd < 0)
 {
 return -1;
 }

  len = snprintf(buf, sizeof(buf), "%d", gpio);
 write(fd, buf, len);
 close(fd);

  return 0;
}

int gpio_set_outdir(int gpio, int isout)
{
      int fd = 0;
      char buf[64] = { 0, };

      snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/direction", gpio);
      fd = open(buf, O_WRONLY);
      if (fd < 0)
      {
            return -1;
      }
     
      if (isout)
      {
            write(fd, "out", 3);
      }
      else
      {
            write(fd, "in", 3);
      }

      close(fd);

      return 0;
}

int gpio_set(int gpio, int level)
{
      int fd;
      char buf[64] = { 0, };
      if (gpio <= 0) return -1;

      sprintf(buf, "/sys/class/gpio/gpio%d/value", gpio);
      fd = open(buf, O_WRONLY);
      if (fd < 0)
      {
            return -1;
      }

      if (level == 1)
            write(fd, "1", 1);
      else
            write(fd, "0", 1);

      close(fd);

      return 1;
}

int gpio_get(int gpio)
{
      int fd;
      char buf[64] = { 0, };
      if (gpio <= 0) return -1;

      sprintf(buf, "/sys/class/gpio/gpio%d/value", gpio);
      fd = open(buf, O_RDONLY);
      if (fd < 0)
      {
            return -1;
      }

      read(fd, buf, 1);
      close(fd);

      if (buf[0] != '0')
            return 1;
     
      return 0;
}

void *thread_a ( void* p_arg ) {
  int human_x = 0;
  int human_y = 0;
  int move_count = 0;

  g_printf ( "thread_a: started\n");

  g_printf ( "thread_a: going into while\n");

  while ( s_b_terminate_thread == FALSE ) {
    call_python3_command_sleep();
    if ( human_x == s_human_x && human_y == s_human_y ) {
      continue;
    }
    human_x = s_human_x;
    human_y = s_human_y;
  // invalidate values if wrong or broken value has come.  
    if ( human_x == -1 || human_x < 150 || 650 < human_x ) {
      human_x = -1;
    }
    if ( human_y == -1 || human_y < 150 || 400 < human_y) {
      human_y = -1;
    }
  // devide x segments to 4, 100~299, 300~399, 400~499, 500~600
    if ( 0 < human_x && human_x <= 299 ) {
        g_printf ( "thread_a: go left and forward\n");
        // go left and forward
        call_python3_command_move_left();
        call_python3_command_move_forward();
        move_count++;
    }
    if ( 300 < human_x && human_x <= 399 ) {
        g_printf ( "thread_a: go forward\n");
        call_python3_command_move_forward();
        move_count++;
    }
    if ( 400 < human_x && human_x <= 499 ) {
         g_printf ( "thread_a: go forward\n");
        call_python3_command_move_forward();
        move_count++;
    }
    if ( 500 < human_x && human_x <= 650 ) {
         g_printf ( "thread_a: go righ and forward\n");
        call_python3_command_move_right();
        call_python3_command_move_forward();
        move_count++;
    }    
    if ( 10 <= move_count ) {
      call_python3_command_move_up();
      gpio_set(14, 0);
      g_printf( "thread_a: gpio 14 set to 1. read value: %d\n", gpio_get(14));
      g_printf( "thread_a: move count is over. exit loop for safety");
      break;
    }
  }
    g_printf ( "thread_a: ended\n");
}
