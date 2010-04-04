/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
/** this is the view for the darkroom module.  */
#include "views/view.h"
#include "develop/develop.h"
#include "control/jobs.h"
#include "control/control.h"
#include "control/conf.h"
#include "develop/imageop.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "gui/gtk.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glade/glade.h>

DT_MODULE(1)

const char
*name(dt_view_t *self)
{
  return _("darkroom");
}


void
init(dt_view_t *self)
{
  self->data = malloc(sizeof(dt_develop_t));
  dt_dev_init((dt_develop_t *)self->data, 1);
}


void cleanup(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_dev_cleanup(dev);
  free(dev);
}


void expose(dt_view_t *self, cairo_t *cri, int32_t width_i, int32_t height_i, int32_t pointerx, int32_t pointery)
{
  // if width or height > max pipeline pixels: center the view and clamp.
  int32_t width  = MIN(width_i,  DT_IMAGE_WINDOW_SIZE);
  int32_t height = MIN(height_i, DT_IMAGE_WINDOW_SIZE);

  cairo_set_source_rgb (cri, .2, .2, .2);
  cairo_rectangle(cri, 0, 0, fmaxf(0, width_i-DT_IMAGE_WINDOW_SIZE) *.5f, height);
  cairo_fill (cri);
  cairo_rectangle(cri, fmaxf(0.0, width_i-DT_IMAGE_WINDOW_SIZE) *.5f + width, 0, width_i, height);
  cairo_fill (cri);

  if(width_i  > DT_IMAGE_WINDOW_SIZE) cairo_translate(cri, -(DT_IMAGE_WINDOW_SIZE-width_i) *.5f, 0.0f);
  if(height_i > DT_IMAGE_WINDOW_SIZE) cairo_translate(cri, 0.0f, -(DT_IMAGE_WINDOW_SIZE-height_i)*.5f);
  cairo_save(cri);

  dt_develop_t *dev = (dt_develop_t *)self->data;

  if(dev->gui_synch)
  { // synch module guis from gtk thread:
    darktable.gui->reset = 1;
    GList *modules = dev->iop;
    while(modules)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      dt_iop_gui_update(module);
      modules = g_list_next(modules);
    }
    darktable.gui->reset = 0;
    dev->gui_synch = 0;
  }
  
  if(dev->image_dirty || dev->pipe->input_timestamp < dev->preview_pipe->input_timestamp) dt_dev_process_image(dev);
  if(dev->preview_dirty) dt_dev_process_preview(dev);

  pthread_mutex_t *mutex = NULL;
  int wd, ht, stride, closeup;
  int32_t zoom;
  float zoom_x, zoom_y;
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  static cairo_surface_t *image_surface = NULL;
  static int image_surface_width = 0, image_surface_height = 0, image_surface_imgid = -1;

  if(image_surface_width != width || image_surface_height != height || image_surface == NULL)
  { // create double-buffered image to draw on, to make modules draw more fluently.
    image_surface_width = width; image_surface_height = height;
    if(image_surface) cairo_surface_destroy(image_surface);
    image_surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
  }
  cairo_surface_t *surface;
  cairo_t *cr = cairo_create(image_surface);

  // adjust scroll bars
  {
    float zx = zoom_x, zy = zoom_y, boxw = 1., boxh = 1.;
    dt_dev_check_zoom_bounds(dev, &zx, &zy, zoom, closeup, &boxw, &boxh);
    dt_view_set_scrollbar(self, zx+.5-boxw*.5, 1.0, boxw, zy+.5-boxh*.5, 1.0, boxh);
  }

  if(!dev->image_dirty && dev->pipe->input_timestamp >= dev->preview_pipe->input_timestamp)
  { // draw image
    mutex = &dev->pipe->backbuf_mutex;
    pthread_mutex_lock(mutex);
    wd = dev->pipe->backbuf_width;
    ht = dev->pipe->backbuf_height;
    stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, wd);
    surface = cairo_image_surface_create_for_data (dev->pipe->backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride); 
    cairo_set_source_rgb (cr, .2, .2, .2);
    cairo_paint(cr);
    cairo_translate(cr, .5f*(width-wd), .5f*(height-ht));
    if(closeup)
    {
      const float closeup_scale = 2.0;
      cairo_scale(cr, closeup_scale, closeup_scale);
      float boxw = 1, boxh = 1, zx0 = zoom_x, zy0 = zoom_y, zx1 = zoom_x, zy1 = zoom_y, zxm = -1.0, zym = -1.0;
      dt_dev_check_zoom_bounds(dev, &zx0, &zy0, zoom, 0, &boxw, &boxh);
      dt_dev_check_zoom_bounds(dev, &zx1, &zy1, zoom, 1, &boxw, &boxh);
      dt_dev_check_zoom_bounds(dev, &zxm, &zym, zoom, 1, &boxw, &boxh);
      const float fx = 1.0 - fmaxf(0.0, (zx0 - zx1)/(zx0 - zxm)), fy = 1.0 - fmaxf(0.0, (zy0 - zy1)/(zy0 - zym));
      cairo_translate(cr, -wd/(2.0*closeup_scale) * fx, -ht/(2.0*closeup_scale) * fy);
    }
    cairo_rectangle(cr, 0, 0, wd, ht);
    cairo_set_source_surface (cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgb (cr, .3, .3, .3);
    cairo_stroke(cr);
    cairo_surface_destroy (surface);
    pthread_mutex_unlock(mutex);
    image_surface_imgid = dev->image->id;
  }
  else if(!dev->preview_dirty)
  // else if(!dev->preview_loading)
  { // draw preview
    mutex = &dev->preview_pipe->backbuf_mutex;
    pthread_mutex_lock(mutex);

    wd = dev->preview_pipe->backbuf_width;
    ht = dev->preview_pipe->backbuf_height;
    float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);
    cairo_set_source_rgb (cr, .2, .2, .2);
    cairo_paint(cr);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_clip(cr);
    stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, wd);
    surface = cairo_image_surface_create_for_data (dev->preview_pipe->backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride); 
    cairo_translate(cr, width/2.0, height/2.0f);
    cairo_scale(cr, zoom_scale, zoom_scale);
    cairo_translate(cr, -.5f*wd-zoom_x*wd, -.5f*ht-zoom_y*ht);
    cairo_rectangle(cr, 0, 0, wd, ht);
    cairo_set_source_surface (cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
    cairo_fill(cr);
    cairo_surface_destroy (surface);
    pthread_mutex_unlock(mutex);
    image_surface_imgid = dev->image->id;
  }
  cairo_restore(cri);

  if(image_surface_imgid == dev->image->id)
  {
    cairo_destroy(cr);
    cairo_set_source_surface(cri, image_surface, 0, 0);
    cairo_paint(cri);
  }

  if(darktable.gui->request_snapshot)
  {
    cairo_surface_write_to_png(image_surface, darktable.gui->snapshot[0].filename);
    darktable.gui->request_snapshot = 0;
  }
  // and if a snapshot is currently selected, draw it on top!
  if(darktable.gui->snapshot_image)
  {
    cairo_set_source_surface(cri, darktable.gui->snapshot_image, 0, 0);
    cairo_rectangle(cri, 0, 0, width*.5f, height);
    cairo_fill(cri);
    cairo_set_source_rgb(cri, .7, .7, .7);
    cairo_set_line_width(cri, 1.0);
    cairo_move_to(cri, width*.5f, 0.0f);
    cairo_line_to(cri, width*.5f, height);
    cairo_stroke(cri);
  }
  
  // execute module callback hook.
  if(dev->gui_module && dev->gui_module->request_color_pick)
  {
    int32_t zoom, closeup;
    float zoom_x, zoom_y;
    float wd = dev->preview_pipe->backbuf_width;
    float ht = dev->preview_pipe->backbuf_height;
    DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
    DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
    DT_CTL_GET_GLOBAL(zoom, dev_zoom);
    DT_CTL_GET_GLOBAL(closeup, dev_closeup);
    float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);

    cairo_translate(cri, width/2.0, height/2.0f);
    cairo_scale(cri, zoom_scale, zoom_scale);
    cairo_translate(cri, -.5f*wd-zoom_x*wd, -.5f*ht-zoom_y*ht);

    // cairo_set_operator(cri, CAIRO_OPERATOR_XOR);
    cairo_set_line_width(cri, 1.0/zoom_scale);
    cairo_set_source_rgb(cri, .2, .2, .2);

    float *box = dev->gui_module->color_picker_box;
    cairo_rectangle(cri, box[0]*wd, box[1]*ht, (box[2] - box[0])*wd, (box[3] - box[1])*ht);
    cairo_stroke(cri);
    cairo_translate(cri, 1.0/zoom_scale, 1.0/zoom_scale);
    cairo_set_source_rgb(cri, .8, .8, .8);
    cairo_rectangle(cri, box[0]*wd, box[1]*ht, (box[2] - box[0])*wd, (box[3] - box[1])*ht);
    cairo_stroke(cri);
  }
  else if(dev->gui_module && dev->gui_module->gui_post_expose)
    dev->gui_module->gui_post_expose(dev->gui_module, cri, width, height, pointerx, pointery);
}


void reset(dt_view_t *self)
{
  DT_CTL_SET_GLOBAL(dev_zoom, DT_ZOOM_FIT);
  DT_CTL_SET_GLOBAL(dev_zoom_x, 0);
  DT_CTL_SET_GLOBAL(dev_zoom_y, 0);
  DT_CTL_SET_GLOBAL(dev_closeup, 0);
}


static void module_show_callback(GtkToggleButton *togglebutton, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  char option[512];
  snprintf(option, 512, "plugins/darkroom/%s/visible", module->op);
  if(gtk_toggle_button_get_active(togglebutton))
  {
    gtk_widget_show_all(GTK_WIDGET(module->topwidget));
    dt_conf_set_bool (option, TRUE);
    gtk_expander_set_expanded(module->expander, TRUE);
    snprintf(option, 512, _("hide %s"), module->name());
  }
  else
  {
    gtk_widget_hide_all(GTK_WIDGET(module->topwidget));
    dt_conf_set_bool (option, FALSE);
    gtk_expander_set_expanded(module->expander, FALSE);
    snprintf(option, 512, _("show %s"), module->name());
  }
  gtk_object_set(GTK_OBJECT(module->showhide), "tooltip-text", option, NULL);
}


int try_enter(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  int selected;
  DT_CTL_GET_GLOBAL(selected, lib_image_mouse_over_id);
  if(selected < 0)
  { // try last selected
    int rc;
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(darktable.db, "select * from selected_images", -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
      selected = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }

  if(selected < 0)
  { // fail :(
    dt_control_log(_("no image selected!"));
    return 1;
  }

  // this loads the image from db if needed:
  dev->image = dt_image_cache_get(selected, 'r');
  // get image and check if it has been deleted from disk first!
  char imgfilename[1024];
  dt_image_full_path(dev->image, imgfilename, 1024);
  if(!g_file_test(imgfilename, G_FILE_TEST_IS_REGULAR))
  {
    dt_control_log(_("image does no longer exist"));
    dt_image_remove(selected);
    dt_image_cache_release(dev->image, 'r');
    dev->image = NULL;
    return 1;
  }
  return 0;
}


void enter(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;

  // select this image, if no multiple selection:
  int count = 0;
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(darktable.db, "select count(imgid) from selected_images", -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  if(count < 2)
  {
    sqlite3_exec(darktable.db, "delete from selected_images", NULL, NULL, NULL);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(darktable.db, "insert into selected_images values (?1)", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, dev->image->id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  DT_CTL_SET_GLOBAL(dev_zoom, DT_ZOOM_FIT);
  DT_CTL_SET_GLOBAL(dev_zoom_x, 0);
  DT_CTL_SET_GLOBAL(dev_zoom_y, 0);
  DT_CTL_SET_GLOBAL(dev_closeup, 0);

  dev->gui_leaving = 0;
  dev->gui_module = NULL;
  dt_dev_load_image(dev, dev->image);

  // adjust gui:
  GtkWidget *widget;
  widget = glade_xml_get_widget (darktable.gui->main_window, "navigation_expander");
  gtk_widget_set_visible(widget, TRUE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "histogram_expander");
  gtk_widget_set_visible(widget, TRUE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "snapshots_eventbox");
  gtk_widget_set_visible(widget, TRUE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "history_eventbox");
  gtk_widget_set_visible(widget, TRUE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "bottom_darkroom_box");
  gtk_widget_set_visible(widget, TRUE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "bottom_lighttable_box");
  gtk_widget_set_visible(widget, FALSE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "library_eventbox");
  gtk_widget_set_visible(widget, FALSE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "module_list_eventbox");
  gtk_widget_set_visible(widget, TRUE);

  // get top level vbox containing all expanders, plugins_vbox:
  GtkBox *box = GTK_BOX(glade_xml_get_widget (darktable.gui->main_window, "plugins_vbox"));
  GtkTable *module_list = GTK_TABLE(glade_xml_get_widget (darktable.gui->main_window, "module_list"));
  GList *modules = g_list_last(dev->iop);
  int ti = 0, tj = 0;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    module->gui_init(module);
    // add the widget created by gui_init to an expander and both to list.
    GtkWidget *expander = dt_iop_gui_get_expander(module);
    module->topwidget = GTK_WIDGET(expander);
    gtk_box_pack_start(box, expander, FALSE, FALSE, 0);
    if(strcmp(module->op, "gamma"))
    {
      module->showhide = gtk_toggle_button_new();
      char filename[1024], datadir[1024];
      dt_get_datadir(datadir, 1024);
      snprintf(filename, 1024, "%s/pixmaps/plugins/darkroom/%s.png", datadir, module->op);
      if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR))
        snprintf(filename, 1024, "%s/pixmaps/plugins/darkroom/template.png", datadir);
      GtkWidget *image = gtk_image_new_from_file(filename);
      gtk_button_set_image(GTK_BUTTON(module->showhide), image);
      g_signal_connect(G_OBJECT(module->showhide), "toggled",
                       G_CALLBACK(module_show_callback), module);
      gtk_table_attach(module_list, module->showhide, ti, ti+1, tj, tj+1,
          GTK_FILL | GTK_EXPAND | GTK_SHRINK,
          GTK_SHRINK,
          0, 0);
      if(ti < 5) ti++;
      else { ti = 0; tj ++; }
    }
    modules = g_list_previous(modules);
  }
  // end marker widget:
  GtkWidget *endmarker = gtk_drawing_area_new();
  gtk_widget_set_size_request(GTK_WIDGET(endmarker), 250, 50);
  gtk_box_pack_start(box, endmarker, FALSE, FALSE, 0);
  g_signal_connect (G_OBJECT (endmarker), "expose-event",
                    G_CALLBACK (dt_control_expose_endmarker), 0);

  gtk_widget_show_all(GTK_WIDGET(box));
  gtk_widget_show_all(GTK_WIDGET(module_list));
  // hack: now hide all custom expander widgets again.
  modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    if(strcmp(module->op, "gamma"))
    {
      char option[1024];
      snprintf(option, 1024, "plugins/darkroom/%s/visible", module->op);
      gboolean active = dt_conf_get_bool (option);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->showhide), !active);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->showhide), active);

      snprintf(option, 1024, "plugins/darkroom/%s/expanded", module->op);
      active = dt_conf_get_bool (option);
      gtk_expander_set_expanded (module->expander, active);
    }
    else
    {
      gtk_widget_hide_all(GTK_WIDGET(module->topwidget));
    }
    modules = g_list_next(modules);
  }
  // synch gui and flag gegl pipe as dirty
  // FIXME: this assumes static pipeline as well
  // this is done here and not in dt_read_history, as it would else be triggered before module->gui_init.
  dt_dev_pop_history_items(dev, dev->history_end);

  // image should be there now.
  float zoom_x, zoom_y;
  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, DT_ZOOM_FIT, 0, NULL, NULL);
  DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
  DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);
}


static void dt_dev_remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove(GTK_CONTAINER(data), widget);
}


void leave(dt_view_t *self)
{
  GtkWidget *widget;
  widget = glade_xml_get_widget (darktable.gui->main_window, "navigation_expander");
  gtk_widget_set_visible(widget, FALSE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "histogram_expander");
  gtk_widget_set_visible(widget, FALSE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "snapshots_eventbox");
  gtk_widget_set_visible(widget, FALSE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "history_eventbox");
  gtk_widget_set_visible(widget, FALSE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "bottom_darkroom_box");
  gtk_widget_set_visible(widget, FALSE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "bottom_lighttable_box");
  gtk_widget_set_visible(widget, TRUE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "library_eventbox");
  gtk_widget_set_visible(widget, TRUE);
  widget = glade_xml_get_widget (darktable.gui->main_window, "module_list_eventbox");
  gtk_widget_set_visible(widget, FALSE);

  dt_develop_t *dev = (dt_develop_t *)self->data;
  // commit image ops to db
  dt_dev_write_history(dev);
  // write .dt file
  dt_image_write_dt_files(dev->image);

  // commit updated mipmaps to db
  dt_dev_process_to_mip(dev);

  // clear gui.
  dev->gui_leaving = 1;
  pthread_mutex_lock(&dev->history_mutex);
  GtkBox *box = GTK_BOX(glade_xml_get_widget (darktable.gui->main_window, "plugins_vbox"));
  while(dev->history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(dev->history->data);
    // printf("removing history item %d - %s, data %f %f\n", hist->module->instance, hist->module->op, *(float *)hist->params, *((float *)hist->params+1));
    free(hist->params); hist->params = NULL;
    dev->history = g_list_delete_link(dev->history, dev->history);
  }
  while(dev->iop)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(dev->iop->data);
    // printf("removing module %d - %s\n", module->instance, module->op);
    char var[1024];
    snprintf(var, 1024, "plugins/darkroom/%s/expanded", module->op);
    dt_conf_set_bool(var, gtk_expander_get_expanded (module->expander));

    g_object_ref_sink(module->menu);
    module->gui_cleanup(module);
    module->cleanup(module);
    dev->iop = g_list_delete_link(dev->iop, dev->iop);
  }
  gtk_container_foreach(GTK_CONTAINER(box), (GtkCallback)dt_dev_remove_child, (gpointer)box);
  pthread_mutex_unlock(&dev->history_mutex);
  
  // release full buffer
  if(dev->image->pixels)
    dt_image_release(dev->image, DT_IMAGE_FULL, 'r');

  // release image struct with metadata as well.
  dt_image_cache_flush(dev->image);
  dt_image_cache_release(dev->image, 'r');
}


// void mouse_leave(dt_view_t *self) {}

void mouse_moved(dt_view_t *self, double x, double y, int which)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_control_t *ctl = darktable.control;
  const int32_t width_i  = ctl->width  - ctl->tabborder*2;
  const int32_t height_i = ctl->height - ctl->tabborder*2;
  int32_t offx = 0.0f, offy = 0.0f;
  if(width_i  > DT_IMAGE_WINDOW_SIZE) offx =   (DT_IMAGE_WINDOW_SIZE-width_i) *.5f;
  if(height_i > DT_IMAGE_WINDOW_SIZE) offy = - (DT_IMAGE_WINDOW_SIZE-height_i)*.5f;
  int handled = 0;
  x += offx;
  y += offy;
  if(dev->gui_module && dev->gui_module->request_color_pick &&
     ctl->button_down &&
     ctl->button_down_which == 1)
  { // module requested a color box
    float zoom_x, zoom_y, bzoom_x, bzoom_y;
    dt_dev_get_pointer_zoom_pos(dev, x, y, &zoom_x, &zoom_y);
    dt_dev_get_pointer_zoom_pos(dev, ctl->button_x, ctl->button_y, &bzoom_x, &bzoom_y);
    dev->gui_module->color_picker_box[0] = fmaxf(0.0, fminf(.5f+bzoom_x, .5f+zoom_x));
    dev->gui_module->color_picker_box[1] = fmaxf(0.0, fminf(.5f+bzoom_y, .5f+zoom_y));
    dev->gui_module->color_picker_box[2] = fminf(1.0, fmaxf(.5f+bzoom_x, .5f+zoom_x));
    dev->gui_module->color_picker_box[3] = fminf(1.0, fmaxf(.5f+bzoom_y, .5f+zoom_y));
    
    dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
    dt_dev_invalidate_all(dev);
    dt_control_queue_draw_all();
    return;
  }
  if(dev->gui_module && dev->gui_module->mouse_moved) handled = dev->gui_module->mouse_moved(dev->gui_module, x, y, which);
  if(handled) return;

  if(darktable.control->button_down && darktable.control->button_down_which == 1)
  { // depending on dev_zoom, adjust dev_zoom_x/y.
    dt_dev_zoom_t zoom;
    int closeup;
    DT_CTL_GET_GLOBAL(zoom, dev_zoom);
    DT_CTL_GET_GLOBAL(closeup, dev_closeup);
    int procw, proch;
    dt_dev_get_processed_size(dev, &procw, &proch);
    const float scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 0);
    float old_zoom_x, old_zoom_y;
    DT_CTL_GET_GLOBAL(old_zoom_x, dev_zoom_x);
    DT_CTL_GET_GLOBAL(old_zoom_y, dev_zoom_y);
    float zx = old_zoom_x - (1.0/scale)*(x - darktable.control->button_x)/procw;
    float zy = old_zoom_y - (1.0/scale)*(y - darktable.control->button_y)/proch;
    dt_dev_check_zoom_bounds(dev, &zx, &zy, zoom, closeup, NULL, NULL);
    DT_CTL_SET_GLOBAL(dev_zoom_x, zx);
    DT_CTL_SET_GLOBAL(dev_zoom_y, zy);
    darktable.control->button_x = x;
    darktable.control->button_y = y;
    dt_dev_invalidate(dev);
    dt_control_queue_draw_all();
  }
}


int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  dt_develop_t *dev = darktable.develop;
  int handled = 0;
  if(dev->gui_module && dev->gui_module->button_pressed) handled = dev->gui_module->button_released(dev->gui_module, x, y, which, state);
  if(handled) return handled;
  if(which == 1) dt_control_change_cursor(GDK_ARROW);
  return 1;
}


int button_pressed(dt_view_t *self, double x, double y, int which, int type, uint32_t state)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
#if 1 // FIXME: x, y needs( to be moved and clamped to DT_WINDOW_SIZE as in expose!!
  const int32_t width_i  = darktable.control->width  - darktable.control->tabborder*2;
  const int32_t height_i = darktable.control->height - darktable.control->tabborder*2;
  if(width_i  > DT_IMAGE_WINDOW_SIZE) x += (DT_IMAGE_WINDOW_SIZE-width_i) *.5f;
  if(height_i > DT_IMAGE_WINDOW_SIZE) y -= (DT_IMAGE_WINDOW_SIZE-height_i)*.5f;
#endif

  int handled = 0;
  if(dev->gui_module && dev->gui_module->request_color_pick)
  {
    float zoom_x, zoom_y;
    dt_dev_get_pointer_zoom_pos(dev, x, y, &zoom_x, &zoom_y);
    dev->gui_module->color_picker_box[0] = .5f+zoom_x;
    dev->gui_module->color_picker_box[1] = .5f+zoom_y;
    dev->gui_module->color_picker_box[2] = .5f+zoom_x;
    dev->gui_module->color_picker_box[3] = .5f+zoom_y;
    dt_control_queue_draw_all();
    return 1;
  }
  if(dev->gui_module && dev->gui_module->button_pressed) handled = dev->gui_module->button_pressed(dev->gui_module, x, y, which, type, state);
  if(handled) return handled;

  if(which == 1 && type == GDK_2BUTTON_PRESS) return 0;
  if(which == 1)
  {
    dt_control_change_cursor(GDK_HAND1);
    return 1;
  }
  if(which == 2)
  {
    // zoom to 1:1 2:1 and back
    dt_dev_zoom_t zoom;
    int closeup, procw, proch;
    float zoom_x, zoom_y;
    DT_CTL_GET_GLOBAL(zoom, dev_zoom);
    DT_CTL_GET_GLOBAL(closeup, dev_closeup);
    DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
    DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
    dt_dev_get_processed_size(dev, &procw, &proch);
    const float scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 0);
    zoom_x += (1.0/scale)*(x - .5f*dev->width )/procw;
    zoom_y += (1.0/scale)*(y - .5f*dev->height)/proch;
    if(zoom == DT_ZOOM_1)
    {
      if(!closeup) closeup = 1;
      else
      {
        zoom = DT_ZOOM_FIT;
        zoom_x = zoom_y = 0.0f;
        closeup = 0;
      }
    }
    else zoom = DT_ZOOM_1;
    dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
    DT_CTL_SET_GLOBAL(dev_zoom, zoom);
    DT_CTL_SET_GLOBAL(dev_closeup, closeup);
    DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
    DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);
    dt_dev_invalidate(dev);
    return 1;
  }
  return 0;
}


void scrolled(dt_view_t *self, double x, double y, int up)
{ // free zoom
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_dev_zoom_t zoom;
  int closeup, procw, proch;
  float zoom_x, zoom_y;
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
  dt_dev_get_processed_size(dev, &procw, &proch);
  float scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2.0 : 1.0, 0);
  const float minscale = dt_dev_get_zoom_scale(dev, DT_ZOOM_FIT, 1.0, 0);
  // offset from center now (current zoom_{x,y} points there)
  float mouse_off_x = x - .5*dev->width, mouse_off_y = y - .5*dev->height;
  zoom_x += mouse_off_x/(procw*scale);
  zoom_y += mouse_off_y/(proch*scale);
  zoom = DT_ZOOM_FREE;
  closeup = 0;
  if(up) scale += .1f*(1.0f - minscale);
  else   scale -= .1f*(1.0f - minscale);
  DT_CTL_SET_GLOBAL(dev_zoom_scale, scale);
  if(scale > 0.99)            zoom = DT_ZOOM_1;
  if(scale < minscale + 0.01) zoom = DT_ZOOM_FIT;
  zoom_x -= mouse_off_x/(procw*scale);
  zoom_y -= mouse_off_y/(proch*scale);
  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
  DT_CTL_SET_GLOBAL(dev_zoom, zoom);
  DT_CTL_SET_GLOBAL(dev_closeup, closeup);
  DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
  DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);
  dt_dev_invalidate(dev);
}


void border_scrolled(dt_view_t *view, double x, double y, int which, int up)
{
  dt_develop_t *dev = (dt_develop_t *)view->data;
  dt_dev_zoom_t zoom;
  int closeup;
  float zoom_x, zoom_y, scale;
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
  DT_CTL_GET_GLOBAL(scale, dev_zoom_scale);
  if(which > 1) 
  {
    if(up) zoom_x -= 0.02;
    else   zoom_x += 0.02;
  }
  else
  {
    if(up) zoom_y -= 0.02;
    else   zoom_y += 0.02;
  }
  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
  DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
  DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);
  dt_dev_invalidate(dev);
  dt_control_queue_draw_all();
}


int key_pressed(dt_view_t *self, uint16_t which)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  int handled = 0;
  if(dev->gui_module && dev->gui_module->key_pressed) handled = dev->gui_module->key_pressed(dev->gui_module, which);
  if(handled) return handled;
  int zoom, closeup;
  float zoom_x, zoom_y;
  switch (which)
  {
    case KEYCODE_1:
      DT_CTL_GET_GLOBAL(zoom, dev_zoom);
      DT_CTL_GET_GLOBAL(closeup, dev_closeup);
      if(zoom == DT_ZOOM_1) closeup ^= 1;
      DT_CTL_SET_GLOBAL(dev_closeup, closeup);
      DT_CTL_SET_GLOBAL(dev_zoom, DT_ZOOM_1);
      /*if(!closeup)
      { // doesn't quite work as expected:
        dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, DT_ZOOM_1, 0, NULL, NULL);
        DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
        DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);
      }*/
      dt_dev_invalidate(dev);
      break;
    case KEYCODE_2:
      DT_CTL_SET_GLOBAL(dev_zoom, DT_ZOOM_FILL);
      dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, DT_ZOOM_FILL, 0, NULL, NULL);
      DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
      DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);
      DT_CTL_SET_GLOBAL(dev_closeup, 0);
      dt_dev_invalidate(dev);
      break;
    case KEYCODE_3:
      DT_CTL_SET_GLOBAL(dev_zoom, DT_ZOOM_FIT);
      DT_CTL_SET_GLOBAL(dev_zoom_x, 0);
      DT_CTL_SET_GLOBAL(dev_zoom_y, 0);
      DT_CTL_SET_GLOBAL(dev_closeup, 0);
      dt_dev_invalidate(dev);
      break;
    default:
      return 0;
      break;
  }
  return 1;
}


void configure(dt_view_t *self, int wd, int ht)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_dev_configure(dev, wd, ht);
}

