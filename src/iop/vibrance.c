/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <gtk/gtk.h>
#include <inttypes.h>

// NaN-safe clip: NaN compares false and will result in 0.0
#define CLIP(x) (((x) >= 0.0) ? ((x) <= 1.0 ? (x) : 1.0) : 0.0)
DT_MODULE_INTROSPECTION(2, dt_iop_vibrance_params_t)

typedef struct dt_iop_vibrance_params_t
{
  float amount; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 25.0 $DESCRIPTION: "vibrance"
} dt_iop_vibrance_params_t;

typedef struct dt_iop_vibrance_gui_data_t
{
  GtkWidget *amount_scale;
} dt_iop_vibrance_gui_data_t;

typedef struct dt_iop_vibrance_data_t
{
  float amount;
} dt_iop_vibrance_data_t;

typedef struct dt_iop_vibrance_global_data_t
{
  int kernel_vibrance;
} dt_iop_vibrance_global_data_t;


const char *name()
{
  return _("vibrance");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_GRADING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_Lab;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "vibrance"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_vibrance_gui_data_t *g = (dt_iop_vibrance_gui_data_t*)self->gui_data;

  dt_accel_connect_slider_iop(self, "vibrance",
                              GTK_WIDGET(g->amount_scale));
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_vibrance_data_t *d = (dt_iop_vibrance_data_t *)piece->data;
  const float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

  const float amount = (d->amount * 0.01);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(amount, ch, roi_out) \
  shared(in, out) \
  schedule(static)
#endif
  for(int k = 0; k < roi_out->height; k++)
  {
    size_t offs = (size_t)k * roi_out->width * ch;
    for(int l = 0; l < (roi_out->width * ch); l += ch)
    {
      /* saturation weight 0 - 1 */
      float sw = sqrt((in[offs + l + 1] * in[offs + l + 1]) + (in[offs + l + 2] * in[offs + l + 2])) / 256.0;
      float ls = 1.0 - ((amount * sw) * .25);
      float ss = 1.0 + (amount * sw);
      out[offs + l + 0] = in[offs + l + 0] * ls;
      out[offs + l + 1] = in[offs + l + 1] * ss;
      out[offs + l + 2] = in[offs + l + 2] * ss;
      out[offs + l + 3] = in[offs + l + 3];
    }
  }
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_vibrance_data_t *data = (dt_iop_vibrance_data_t *)piece->data;
  dt_iop_vibrance_global_data_t *gd = (dt_iop_vibrance_global_data_t *)self->global_data;
  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const float amount = data->amount * 0.01f;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  dt_opencl_set_kernel_arg(devid, gd->kernel_vibrance, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vibrance, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vibrance, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vibrance, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vibrance, 4, sizeof(float), (void *)&amount);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_vibrance, sizes);
  if(err != CL_SUCCESS) goto error;

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_vibrance] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif



void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_vibrance_global_data_t *gd
      = (dt_iop_vibrance_global_data_t *)malloc(sizeof(dt_iop_vibrance_global_data_t));
  module->data = gd;
  gd->kernel_vibrance = dt_opencl_create_kernel(program, "vibrance");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_vibrance_global_data_t *gd = (dt_iop_vibrance_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_vibrance);
  free(module->data);
  module->data = NULL;
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_vibrance_params_t *p = (dt_iop_vibrance_params_t *)p1;
  dt_iop_vibrance_data_t *d = (dt_iop_vibrance_data_t *)piece->data;
  d->amount = p->amount;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_vibrance_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_vibrance_gui_data_t *g = (dt_iop_vibrance_gui_data_t *)self->gui_data;
  dt_iop_vibrance_params_t *p = (dt_iop_vibrance_params_t *)module->params;
  dt_bauhaus_slider_set(g->amount_scale, p->amount);
}
void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_vibrance_gui_data_t *g = IOP_GUI_ALLOC(vibrance);

  g->amount_scale = dt_bauhaus_slider_from_params(self, "amount");
  dt_bauhaus_slider_set_format(g->amount_scale, "%.0f%%");
  gtk_widget_set_tooltip_text(g->amount_scale, _("the amount of vibrance"));
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
