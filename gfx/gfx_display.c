/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2016-2019 - Brad Parker
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */
#include <encodings/utf.h>
#include "gfx_display.h"

#include "video_coord_array.h"
#include "../configuration.h"
#include "../verbosity.h"

#ifdef HAVE_MIST
#include "../steam/steam.h"
#endif

/* Standard reference DPI value, used when determining
 * DPI-aware scaling factors */
#define REFERENCE_DPI 96.0f

/* 'OZONE_SIDEBAR_WIDTH' must be kept in sync
 * with Ozone driver metrics */
#define OZONE_SIDEBAR_WIDTH 408

/* Small 1x1 white texture used for blending purposes */
static uintptr_t gfx_white_texture;

/* ptr alignment */
static gfx_display_t dispgfx_st = {0};

gfx_display_t *disp_get_ptr(void)
{
   return &dispgfx_st;
}

static const float *null_get_default_matrix(void)
{
   static float dummy[16] = {0.0f};
   return &dummy[0];
}

gfx_display_ctx_driver_t gfx_display_ctx_null = {
   NULL,                                     /* draw            */
   NULL,                                     /* draw_pipeline   */
   NULL,                                     /* blend_begin     */
   NULL,                                     /* blend_end       */
   NULL,                                     /* get_default_mvp */
   null_get_default_matrix,
   null_get_default_matrix,
   FONT_DRIVER_RENDER_DONT_CARE,
   GFX_VIDEO_DRIVER_GENERIC,
   "null",
   false,
   NULL,
   NULL
};

/* Menu display drivers */
static gfx_display_ctx_driver_t *gfx_display_ctx_drivers[] = {
#ifdef HAVE_D3D8
   &gfx_display_ctx_d3d8,
#endif
#ifdef HAVE_D3D9
#ifdef HAVE_HLSL
   &gfx_display_ctx_d3d9_hlsl,
#endif
#ifdef HAVE_CG
   &gfx_display_ctx_d3d9_cg,
#endif
#endif
#ifdef HAVE_D3D10
   &gfx_display_ctx_d3d10,
#endif
#ifdef HAVE_D3D11
   &gfx_display_ctx_d3d11,
#endif
#ifdef HAVE_D3D12
   &gfx_display_ctx_d3d12,
#endif
#ifdef HAVE_OPENGL
   &gfx_display_ctx_gl,
#endif
#ifdef HAVE_OPENGL1
   &gfx_display_ctx_gl1,
#endif
#ifdef HAVE_OPENGL_CORE
   &gfx_display_ctx_gl3,
#endif
#ifdef HAVE_VULKAN
   &gfx_display_ctx_vulkan,
#endif
#ifdef HAVE_METAL
   &gfx_display_ctx_metal,
#endif
#ifdef HAVE_VITA2D
   &gfx_display_ctx_vita2d,
#endif
#ifdef _3DS
   &gfx_display_ctx_ctr,
#endif
#ifdef WIIU
   &gfx_display_ctx_wiiu,
#endif
#ifdef __PSL1GHT__
   &gfx_display_ctx_rsx,
#endif
#if defined(_WIN32) && !defined(_XBOX) && !defined(__WINRT__)
#ifdef HAVE_GDI
   &gfx_display_ctx_gdi,
#endif
#endif
   &gfx_display_ctx_null,
   NULL,
};

float gfx_display_get_adjusted_scale(
      gfx_display_t *p_disp,
      float base_scale, float scale_factor, unsigned width)
{
   /* Apply user-set scaling factor */
   float adjusted_scale   = base_scale * scale_factor;
#ifdef HAVE_OZONE
   /* Ozone has a capped scale factor */
   if (p_disp->menu_driver_id == MENU_DRIVER_ID_OZONE)
   {
      float new_width    = (float)width * 0.3333333f;
      if (((float)OZONE_SIDEBAR_WIDTH * adjusted_scale)
            > new_width)
         adjusted_scale  = (new_width / (float)OZONE_SIDEBAR_WIDTH);
   }
#endif
   /* Ensure final scale is 'sane' */
   if (adjusted_scale > 0.0001f)
      return adjusted_scale;
   return 1.0f;
}

/* Check if the current menu driver is compatible
 * with your video driver. */
static bool gfx_display_check_compatibility(
      const char *video_driver,
      enum gfx_display_driver_type type,
      const char *ident,
      bool video_is_threaded)
{
   switch (type)
   {
      case GFX_VIDEO_DRIVER_OPENGL:
      case GFX_VIDEO_DRIVER_OPENGL1:
      case GFX_VIDEO_DRIVER_OPENGL_CORE:
      case GFX_VIDEO_DRIVER_VULKAN:
      case GFX_VIDEO_DRIVER_METAL:
      case GFX_VIDEO_DRIVER_DIRECT3D8:
      case GFX_VIDEO_DRIVER_DIRECT3D9_HLSL:
      case GFX_VIDEO_DRIVER_DIRECT3D9_CG:
      case GFX_VIDEO_DRIVER_DIRECT3D10:
      case GFX_VIDEO_DRIVER_DIRECT3D11:
      case GFX_VIDEO_DRIVER_DIRECT3D12:
      case GFX_VIDEO_DRIVER_VITA2D:
      case GFX_VIDEO_DRIVER_CTR:
      case GFX_VIDEO_DRIVER_WIIU:
      case GFX_VIDEO_DRIVER_GDI:
      case GFX_VIDEO_DRIVER_SWITCH:
      case GFX_VIDEO_DRIVER_RSX:
         return string_is_equal(video_driver, ident);
      case GFX_VIDEO_DRIVER_GENERIC:
         return true;
   }

   return false;
}

float gfx_display_get_dpi_scale_internal(
      unsigned width, unsigned height)
{
   float dpi;
   float diagonal_pixels;
   float pixel_scale;
   static unsigned last_width  = 0;
   static unsigned last_height = 0;
   static float scale          = 0.0f;
   static bool scale_cached    = false;
   gfx_ctx_metrics_t metrics;

   if (scale_cached &&
       (width == last_width) &&
       (height == last_height))
      return scale;

   /* Determine the diagonal 'size' of the display
    * (or window) in terms of pixels */
   diagonal_pixels = (float)sqrt(
         (double)((width * width) + (height * height)));

   /* TODO/FIXME: On Mac, calling video_context_driver_get_metrics()
    * here causes RetroArch to crash (EXC_BAD_ACCESS). This is
    * unfortunate, and needs to be fixed at the gfx context driver
    * level. Until this is done, all we can do is fallback to using
    * the old legacy 'magic number' scaling on Mac platforms. */
#if !defined(HAVE_COCOATOUCH) && (defined(HAVE_COCOA) || defined(HAVE_COCOA_METAL))
   if (true)
   {
      scale        = (diagonal_pixels / 6.5f) / 212.0f;
      scale_cached = true;
      last_width   = width;
      last_height  = height;
      return scale;
   }
#endif

   /* Get pixel scale relative to baseline 1080p display */
   pixel_scale   = diagonal_pixels / DIAGONAL_PIXELS_1080P;

   /* Attempt to get display DPI */
   metrics.type  = DISPLAY_METRIC_DPI;
   metrics.value = &dpi;

   if (video_context_driver_get_metrics(&metrics) && (dpi > 0.0f))
   {
      float display_size;
      float dpi_scale;

#if defined(ANDROID) || defined(HAVE_COCOATOUCH)
      /* Android/iOS devices tell complete lies when
       * reporting DPI values. From the Android devices
       * I've had access to, the DPI is generally
       * overestimated by 17%. All we can do is apply
       * a blind correction factor... */
      dpi *= 0.83f;
#endif

      /* Note: If we are running in windowed mode, this
       * 'display size' is actually the window size - which
       * kinda makes a mess of everything. Since we cannot
       * get fullscreen resolution when running in windowed
       * mode, there is nothing we can do about this. So just
       * treat the window as a display, and hope for the best... */
      display_size = diagonal_pixels / dpi;
      dpi_scale    = dpi / REFERENCE_DPI;

      /* Note: We have tried leveraging every possible metric
       * (and numerous studies on TV/monitor/mobile device
       * usage habits) to determine an appropriate auto scaling
       * factor. *None of these 'smart'/technical methods work
       * consistently in the real world* - there is simply too
       * much variance.
       * So instead we have implemented a very fuzzy/loose
       * method which is crude as can be, but actually has
       * some semblance of usability... */

      if (display_size > 24.0f)
      {
         /* DPI scaling fails miserably when using large
          * displays. Having a UI element that's 1 inch high
          * on all screens might seem like a good idea - until
          * you realise that a HTPC user is probably sitting
          * several metres from their TV, which makes something
          * 1 inch high virtually invisible.
          * So we make some assumptions:
          * - Normal size displays <= 24 inches are probably
          *   PC monitors, with an eye-to-screen distance of
          *   1 arm length. Under these conditions, fixed size
          *   (DPI scaled) UI elements should be visible for most
          *   users
          * - Large displays > 24 inches start to encroach on
          *   TV territory. Once we start working with TVs, we
          *   have to consider users sitting on a couch - and
          *   in this situation, we fall back to the age-old
          *   standard of UI elements occupying a fixed fraction
          *   of the display size (i.e. just look at the menu of
          *   any console system for the past decade)
          * - 24 -> 32 inches is a grey area, where the display
          *   might be a monitor or a TV. Above 32 inches, a TV
          *   is almost a certainty. So we simply lerp between
          *   dpi scaling and pixel scaling as the display size
          *   increases from 24 to 32 */
         float fraction  = (display_size > 32.0f) ? 32.0f : display_size;
         fraction       -= 24.0f;
         fraction       /= (32.0f - 24.0f);

         scale           =   ((1.0f - fraction) * dpi_scale) 
                           + (fraction * pixel_scale);
      }
      else if (display_size < 12.0f)
      {
         /* DPI scaling also fails when using very small
          * displays - i.e. mobile devices (tablets/phones).
          * That 1 inch UI element is going to look pretty
          * dumb on a 5 inch screen in landscape orientation...
          * We're essentially in the opposite situation to the
          * TV case above, and it turns out that a similar
          * solution provides relief: as screen size reduces
          * from 12 inches to zero, we lerp from dpi scaling
          * to pixel scaling */
         float fraction = display_size / 12.0f;

         scale          =   ((1.0f - fraction) * pixel_scale) 
                          + (fraction * dpi_scale);
      }
      else
         scale          = dpi_scale;
   }
   /* If DPI retrieval is unsupported, all we can do
    * is use the raw pixel scale */
   else
      scale             = pixel_scale;

   scale_cached         = true;
   last_width           = width;
   last_height          = height;

   return scale;
}

float gfx_display_get_dpi_scale(
      gfx_display_t *p_disp,
      void *settings_data,
      unsigned width, unsigned height,
      bool fullscreen,
      bool is_widget
)
{
   static unsigned last_width                          = 0;
   static unsigned last_height                         = 0;
   static float scale                                  = 0.0f;
   static bool scale_cached                            = false;
   bool scale_updated                                  = false;
   static float last_menu_scale_factor                 = 0.0f;
   static enum menu_driver_id_type last_menu_driver_id = MENU_DRIVER_ID_UNKNOWN;
   static float adjusted_scale                         = 1.0f;
   settings_t *settings                                = (settings_t*)settings_data;
#ifdef HAVE_GFX_WIDGETS
   bool gfx_widget_scale_auto                          = settings->bools.menu_widget_scale_auto;
#if (defined(RARCH_CONSOLE) || defined(RARCH_MOBILE))
   float menu_widget_scale_factor                      = settings->floats.menu_widget_scale_factor;
#else
   float menu_widget_scale_factor_fullscreen           = settings->floats.menu_widget_scale_factor;
   float menu_widget_scale_factor_windowed             = settings->floats.menu_widget_scale_factor_windowed;
   float menu_widget_scale_factor                      = fullscreen ?
         menu_widget_scale_factor_fullscreen : menu_widget_scale_factor_windowed;
#endif
   float menu_scale_factor                             = is_widget 
      ? menu_widget_scale_factor
      : settings->floats.menu_scale_factor;
#else
   float menu_scale_factor                             = settings->floats.menu_scale_factor;
#endif

#ifdef HAVE_GFX_WIDGETS
   if (is_widget)
   {
      if (gfx_widget_scale_auto)
      {
#ifdef HAVE_RGUI
         /* When using RGUI, _menu_scale_factor
          * is ignored
          * > If we are not using a widget scale factor override,
          *   just set menu_scale_factor to 1.0 */
         if (p_disp->menu_driver_id == MENU_DRIVER_ID_RGUI)
            menu_scale_factor                             = 1.0f;
         else
#endif
         {
            float _menu_scale_factor                      = 
               settings->floats.menu_scale_factor;
            menu_scale_factor                             = _menu_scale_factor;
         }
      }
   }
#endif

   /* Scale is based on display metrics - these are a fixed
    * hardware property. To minimise performance overheads
    * we therefore only call video_context_driver_get_metrics()
    * on first run, or when the current video resolution changes */
   if (!scale_cached ||
       (width  != last_width) ||
       (height != last_height))
   {
      scale         = gfx_display_get_dpi_scale_internal(width, height);
      scale_cached  = true;
      scale_updated = true;
      last_width    = width;
      last_height   = height;
   }

   /* Adjusted scale calculation may also be slow, so
    * only update if something changes */
   if (scale_updated ||
       (menu_scale_factor != last_menu_scale_factor) ||
       (p_disp->menu_driver_id != last_menu_driver_id))
   {
      adjusted_scale         = gfx_display_get_adjusted_scale(
            p_disp,
            scale, menu_scale_factor, width);
      last_menu_scale_factor = menu_scale_factor;
      last_menu_driver_id    = p_disp->menu_driver_id;
   }

   return adjusted_scale;
}

/* Begin scissoring operation */
void gfx_display_scissor_begin(
      gfx_display_t *p_disp,
      void *userdata,
      unsigned video_width,
      unsigned video_height,
      int x, int y, unsigned width, unsigned height)
{
   gfx_display_ctx_driver_t *dispctx = p_disp->dispctx;
   if (dispctx && dispctx->scissor_begin)
   {
      if (y < 0)
      {
         if (height < (unsigned)(-y))
            height  = 0;
         else
            height += y;
         y          = 0;
      }
      if (x < 0)
      {
         if (width < (unsigned)(-x))
            width   = 0;
         else
            width  += x;
         x          = 0;
      }
      if (y >= (int)video_height)
      {
         height     = 0;
         y          = 0;
      }
      if (x >= (int)video_width)
      {
         width      = 0;
         x          = 0;
      }
      if ((y + height) > video_height)
         height     = video_height - y;
      if ((x + width) > video_width)
         width      = video_width - x;

      dispctx->scissor_begin(userdata,
            video_width, video_height,
            x, y, width, height);
   }
}

font_data_t *gfx_display_font_file(
      gfx_display_t *p_disp,
      char* fontpath, float menu_font_size, bool is_threaded)
{
   gfx_display_ctx_driver_t *dispctx = p_disp->dispctx;

   if (dispctx)
   {
      font_data_t        *font_data  = NULL;
      float               font_size  = menu_font_size;
      /* Font size must be at least 2, or font_init_first()
       * will generate a heap-buffer-overflow when using
       * many font drivers */
      if (font_size < 2.0f)
         font_size = 2.0f;
      if ((font_data = font_driver_init_first(video_driver_get_ptr(),
                  fontpath, font_size, true, is_threaded,
                  dispctx->font_type)))
         return font_data;
   }
   return NULL;
}

/* Draw text on top of the screen */
void gfx_display_draw_text(
      const font_data_t *font, const char *text,
      float x, float y, int width, int height,
      uint32_t color, enum text_alignment text_align,
      float scale, bool shadows_enable, float shadow_offset,
      bool draw_outside)
{
   struct font_params params;
   video_driver_state_t *video_st = video_state_get_ptr();

   if ((color & 0x000000FF) == 0)
      return;

   /* Don't draw outside of the screen */
   if (!draw_outside &&
           ((x < -64 || x > width  + 64)
         || (y < -64 || y > height + 64))
      )
      return;

   params.x           = x / width;
   params.y           = 1.0f - y / height;
   params.scale       = scale;
   params.drop_mod    = 0.0f;
   params.drop_x      = 0.0f;
   params.drop_y      = 0.0f;
   params.color       = color;
   params.full_screen = true;
   params.text_align  = text_align;

   if (shadows_enable)
   {
      params.drop_x      = shadow_offset;
      params.drop_y      = -shadow_offset;
      params.drop_alpha  = GFX_SHADOW_ALPHA;
   }

   if (video_st->poke && video_st->poke->set_osd_msg)
      video_st->poke->set_osd_msg(video_st->data,
            text, &params, (void*)font);
}

void gfx_display_draw_bg(
      gfx_display_t *p_disp,
      gfx_display_ctx_draw_t *draw,
      void *userdata, bool add_opacity_to_wallpaper,
      float override_opacity)
{
   static struct video_coords coords;
   const float           *new_vertex = NULL;
   const float        *new_tex_coord = NULL;
   gfx_display_ctx_driver_t *dispctx = p_disp->dispctx;
   if (!dispctx || !draw)
      return;

   if (draw->vertex)
      new_vertex                     = draw->vertex;
   else if (dispctx->get_default_vertices)
      new_vertex                     = dispctx->get_default_vertices();

   if (draw->tex_coord)
      new_tex_coord                  = draw->tex_coord;
   else if (dispctx->get_default_tex_coords)
      new_tex_coord                  = dispctx->get_default_tex_coords();

   coords.vertices                   = (unsigned)draw->vertex_count;
   coords.vertex                     = new_vertex;
   coords.tex_coord                  = new_tex_coord;
   coords.lut_tex_coord              = new_tex_coord;
   coords.color                      = (const float*)draw->color;

   draw->coords                      = &coords;
   draw->scale_factor                = 1.0f;
   draw->rotation                    = 0.0f;

   if (draw->texture)
      add_opacity_to_wallpaper       = true;
   else
      draw->texture                  = gfx_white_texture;

   if (add_opacity_to_wallpaper)
      gfx_display_set_alpha(draw->color, override_opacity);

   if (dispctx->get_default_mvp)
      draw->matrix_data = (math_matrix_4x4*)dispctx->get_default_mvp(
            userdata);
}

void gfx_display_draw_quad(
      gfx_display_t *p_disp,
      void *data,
      unsigned video_width,
      unsigned video_height,
      int x, int y, unsigned w, unsigned h,
      unsigned width, unsigned height,
      float *color,
      uintptr_t *texture)
{
   gfx_display_ctx_draw_t draw;
   struct video_coords coords;
   gfx_display_ctx_driver_t 
      *dispctx             = p_disp->dispctx;

   if (w == 0 || h == 0)
      return;
   if (!dispctx)
      return;

   coords.vertices      = 4;
   coords.vertex        = NULL;
   coords.tex_coord     = NULL;
   coords.lut_tex_coord = NULL;
   coords.color         = color;

   draw.x               = x;
   draw.y               = (int)height - y - (int)h;
   draw.width           = w;
   draw.height          = h;
   draw.coords          = &coords;
   draw.matrix_data     = NULL;
   draw.texture         = (texture != 0) 
      ? *texture 
      : gfx_white_texture;
   draw.prim_type       = GFX_DISPLAY_PRIM_TRIANGLESTRIP;
   draw.pipeline_id     = 0;
   draw.scale_factor    = 1.0f;
   draw.rotation        = 0.0f;

   if (dispctx->blend_begin)
      dispctx->blend_begin(data);
   if (dispctx->draw)
      dispctx->draw(&draw, data, video_width, video_height);
   if (dispctx->blend_end)
      dispctx->blend_end(data);
}

/* Draw the texture split into 9 sections, without scaling the corners.
 * The middle sections will only scale in the X axis, and the side
 * sections will only scale in the Y axis. */
void gfx_display_draw_texture_slice(
      gfx_display_t *p_disp,
      void *userdata,
      unsigned video_width,
      unsigned video_height,
      int x, int y, unsigned w, unsigned h,
      unsigned new_w, unsigned new_h,
      unsigned width, unsigned height,
      float *color, unsigned offset, float scale_factor, uintptr_t texture,
      math_matrix_4x4 *mymat
)
{
   gfx_display_ctx_draw_t draw;
   struct video_coords coords;
   gfx_display_ctx_driver_t 
      *dispctx              = p_disp->dispctx;
   float V_BL[2], V_BR[2], V_TL[2], V_TR[2], T_BL[2], T_BR[2], T_TL[2], T_TR[2];
   /* To prevent visible seams between the corners and
    * middle segments of the sliced texture, the texture
    * must be scaled such that its effective size (before
    * expansion of the middle segments) is no greater than
    * the requested display size.
    * > This is consequence of the way textures are rendered
    *   in hardware...
    * > Whenever an image is scaled, the colours at the
    *   transparent edges get interpolated, which means
    *   the colours of the transparent pixels themselves bleed
    *   into the visible area.
    * > This effectively 'blurs' anything that gets scaled
    *   [SIDE NOTE: this causes additional issues if the transparent
    *    pixels have the wrong colour - i.e. if they are black,
    *    every edge gets a nasty dark border...]
    * > This burring is a problem because (by design) the corners
    *   of the sliced texture are drawn at native resolution,
    *   whereas the middle segments are stretched to fit the
    *   requested dimensions. Consequently, the corners are sharp
    *   while the middle segments are blurred.
    * > When *upscaling* the middle segments (i.e. display size
    *   greater than texture size), the visible effects of this
    *   are mostly imperceptible.
    * > When *downscaling* them, however, the interpolation effects
    *   completely dominate the output image - creating an ugly
    *   transition between the corners and middle parts.
    * > Since this is a property of hardware rendering, it is not
    *   practical to fix this 'properly'...
    * > However: An effective workaround is to force downscaling of
    *   the entire texture (including corners) whenever the
    *   requested display size is less than the texture dimensions.
    * > This blurs the corners enough that the corner/middle
    *   transitions are essentially invisible. */
   float max_scale_w = (float)new_w / (float)w;
   float max_scale_h = (float)new_h / (float)h;
   /* Find the minimum of scale_factor, max_scale_w, max_scale_h */
   float slice_scale = (scale_factor < max_scale_w) ?
         (scale_factor < max_scale_h) ? scale_factor : max_scale_h :
         (max_scale_w  < max_scale_h) ? max_scale_w  : max_scale_h;

   /* need space for the coordinates of two triangles in a strip,
    * so 8 vertices */
   float tex_coord[8];
   float vert_coord[8];
   static float colors[16] = { 
      1.0f, 1.0f, 1.0f, 1.0f,
      1.0f, 1.0f, 1.0f, 1.0f,
      1.0f, 1.0f, 1.0f, 1.0f,
      1.0f, 1.0f, 1.0f, 1.0f
   };

   /* normalized width/height of the amount to offset from the corners,
    * for both the vertex and texture coordinates */
   float vert_woff   = (offset * slice_scale) / (float)width;
   float vert_hoff   = (offset * slice_scale) / (float)height;
   float tex_woff    = offset / (float)w;
   float tex_hoff    = offset / (float)h;

   /* the width/height of the middle sections of both the scaled and original image */
   float vert_scaled_mid_width  = (new_w - (offset * slice_scale * 2))
      / (float)width;
   float vert_scaled_mid_height = (new_h - (offset * slice_scale * 2))
      / (float)height;
   float tex_mid_width          = (w - (offset * 2)) / (float)w;
   float tex_mid_height         = (h - (offset * 2)) / (float)h;

   /* normalized coordinates for the start position of the image */
   float norm_x                 = x / (float)width;
   float norm_y                 = (height - y) / (float)height;

   if (width == 0 || height == 0)
      return;
   if (!dispctx || !dispctx->draw)
      return;

   /* the four vertices of the top-left corner of the image,
    * used as a starting point for all the other sections */
   V_BL[0] = norm_x;
   V_BL[1] = norm_y;
   V_BR[0] = norm_x + vert_woff;
   V_BR[1] = norm_y;
   V_TL[0] = norm_x;
   V_TL[1] = norm_y + vert_hoff;
   V_TR[0] = norm_x + vert_woff;
   V_TR[1] = norm_y + vert_hoff;
   T_BL[0] = 0.0f;
   T_BL[1] = tex_hoff;
   T_BR[0] = tex_woff;
   T_BR[1] = tex_hoff;
   T_TL[0] = 0.0f;
   T_TL[1] = 0.0f;
   T_TR[0] = tex_woff;
   T_TR[1] = 0.0f;

   coords.vertices          = 4;
   coords.vertex            = vert_coord;
   coords.tex_coord         = tex_coord;
   coords.lut_tex_coord     = NULL;
   draw.width               = width;
   draw.height              = height;
   draw.coords              = &coords;
   draw.matrix_data         = mymat;
   draw.prim_type           = GFX_DISPLAY_PRIM_TRIANGLESTRIP;
   draw.pipeline_id         = 0;
   coords.color             = (const float*)(color == NULL ? colors : color);

   draw.texture             = texture;
   draw.x                   = 0;
   draw.y                   = 0;

   /* vertex coords are specfied bottom-up in this order: BL BR TL TR */
   /* texture coords are specfied top-down in this order: BL BR TL TR */

   /* If someone wants to change this to not draw several times, the
    * coordinates will need to be modified because of the triangle strip usage. */

   /* top-left corner */
   vert_coord[0] = V_BL[0];
   vert_coord[1] = V_BL[1];
   vert_coord[2] = V_BR[0];
   vert_coord[3] = V_BR[1];
   vert_coord[4] = V_TL[0];
   vert_coord[5] = V_TL[1];
   vert_coord[6] = V_TR[0];
   vert_coord[7] = V_TR[1];

   tex_coord[0] = T_BL[0];
   tex_coord[1] = T_BL[1];
   tex_coord[2] = T_BR[0];
   tex_coord[3] = T_BR[1];
   tex_coord[4] = T_TL[0];
   tex_coord[5] = T_TL[1];
   tex_coord[6] = T_TR[0];
   tex_coord[7] = T_TR[1];

   dispctx->draw(&draw, userdata, video_width, video_height);

   /* top-middle section */
   vert_coord[0] = V_BL[0] + vert_woff;
   vert_coord[1] = V_BL[1];
   vert_coord[2] = V_BR[0] + vert_scaled_mid_width;
   vert_coord[3] = V_BR[1];
   vert_coord[4] = V_TL[0] + vert_woff;
   vert_coord[5] = V_TL[1];
   vert_coord[6] = V_TR[0] + vert_scaled_mid_width;
   vert_coord[7] = V_TR[1];

   tex_coord[0] = T_BL[0] + tex_woff;
   tex_coord[1] = T_BL[1];
   tex_coord[2] = T_BR[0] + tex_mid_width;
   tex_coord[3] = T_BR[1];
   tex_coord[4] = T_TL[0] + tex_woff;
   tex_coord[5] = T_TL[1];
   tex_coord[6] = T_TR[0] + tex_mid_width;
   tex_coord[7] = T_TR[1];

   dispctx->draw(&draw, userdata, video_width, video_height);

   /* top-right corner */
   vert_coord[0] = V_BL[0] + vert_woff + vert_scaled_mid_width;
   vert_coord[1] = V_BL[1];
   vert_coord[2] = V_BR[0] + vert_scaled_mid_width + vert_woff;
   vert_coord[3] = V_BR[1];
   vert_coord[4] = V_TL[0] + vert_woff + vert_scaled_mid_width;
   vert_coord[5] = V_TL[1];
   vert_coord[6] = V_TR[0] + vert_scaled_mid_width + vert_woff;
   vert_coord[7] = V_TR[1];

   tex_coord[0] = T_BL[0] + tex_woff + tex_mid_width;
   tex_coord[1] = T_BL[1];
   tex_coord[2] = T_BR[0] + tex_mid_width + tex_woff;
   tex_coord[3] = T_BR[1];
   tex_coord[4] = T_TL[0] + tex_woff + tex_mid_width;
   tex_coord[5] = T_TL[1];
   tex_coord[6] = T_TR[0] + tex_mid_width + tex_woff;
   tex_coord[7] = T_TR[1];

   dispctx->draw(&draw, userdata, video_width, video_height);

   /* middle-left section */
   vert_coord[0] = V_BL[0];
   vert_coord[1] = V_BL[1] - vert_scaled_mid_height;
   vert_coord[2] = V_BR[0];
   vert_coord[3] = V_BR[1] - vert_scaled_mid_height;
   vert_coord[4] = V_TL[0];
   vert_coord[5] = V_TL[1] - vert_hoff;
   vert_coord[6] = V_TR[0];
   vert_coord[7] = V_TR[1] - vert_hoff;

   tex_coord[0] = T_BL[0];
   tex_coord[1] = T_BL[1] + tex_mid_height;
   tex_coord[2] = T_BR[0];
   tex_coord[3] = T_BR[1] + tex_mid_height;
   tex_coord[4] = T_TL[0];
   tex_coord[5] = T_TL[1] + tex_hoff;
   tex_coord[6] = T_TR[0];
   tex_coord[7] = T_TR[1] + tex_hoff;

   dispctx->draw(&draw, userdata, video_width, video_height);

   /* center section */
   vert_coord[0] = V_BL[0] + vert_woff;
   vert_coord[1] = V_BL[1] - vert_scaled_mid_height;
   vert_coord[2] = V_BR[0] + vert_scaled_mid_width;
   vert_coord[3] = V_BR[1] - vert_scaled_mid_height;
   vert_coord[4] = V_TL[0] + vert_woff;
   vert_coord[5] = V_TL[1] - vert_hoff;
   vert_coord[6] = V_TR[0] + vert_scaled_mid_width;
   vert_coord[7] = V_TR[1] - vert_hoff;

   tex_coord[0] = T_BL[0] + tex_woff;
   tex_coord[1] = T_BL[1] + tex_mid_height;
   tex_coord[2] = T_BR[0] + tex_mid_width;
   tex_coord[3] = T_BR[1] + tex_mid_height;
   tex_coord[4] = T_TL[0] + tex_woff;
   tex_coord[5] = T_TL[1] + tex_hoff;
   tex_coord[6] = T_TR[0] + tex_mid_width;
   tex_coord[7] = T_TR[1] + tex_hoff;

   dispctx->draw(&draw, userdata, video_width, video_height);

   /* middle-right section */
   vert_coord[0] = V_BL[0] + vert_woff + vert_scaled_mid_width;
   vert_coord[1] = V_BL[1] - vert_scaled_mid_height;
   vert_coord[2] = V_BR[0] + vert_woff + vert_scaled_mid_width;
   vert_coord[3] = V_BR[1] - vert_scaled_mid_height;
   vert_coord[4] = V_TL[0] + vert_woff + vert_scaled_mid_width;
   vert_coord[5] = V_TL[1] - vert_hoff;
   vert_coord[6] = V_TR[0] + vert_woff + vert_scaled_mid_width;
   vert_coord[7] = V_TR[1] - vert_hoff;

   tex_coord[0] = T_BL[0] + tex_woff + tex_mid_width;
   tex_coord[1] = T_BL[1] + tex_mid_height;
   tex_coord[2] = T_BR[0] + tex_woff + tex_mid_width;
   tex_coord[3] = T_BR[1] + tex_mid_height;
   tex_coord[4] = T_TL[0] + tex_woff + tex_mid_width;
   tex_coord[5] = T_TL[1] + tex_hoff;
   tex_coord[6] = T_TR[0] + tex_woff + tex_mid_width;
   tex_coord[7] = T_TR[1] + tex_hoff;

   dispctx->draw(&draw, userdata, video_width, video_height);

   /* bottom-left corner */
   vert_coord[0] = V_BL[0];
   vert_coord[1] = V_BL[1] - vert_hoff - vert_scaled_mid_height;
   vert_coord[2] = V_BR[0];
   vert_coord[3] = V_BR[1] - vert_hoff - vert_scaled_mid_height;
   vert_coord[4] = V_TL[0];
   vert_coord[5] = V_TL[1] - vert_hoff - vert_scaled_mid_height;
   vert_coord[6] = V_TR[0];
   vert_coord[7] = V_TR[1] - vert_hoff - vert_scaled_mid_height;

   tex_coord[0] = T_BL[0];
   tex_coord[1] = T_BL[1] + tex_hoff + tex_mid_height;
   tex_coord[2] = T_BR[0];
   tex_coord[3] = T_BR[1] + tex_hoff + tex_mid_height;
   tex_coord[4] = T_TL[0];
   tex_coord[5] = T_TL[1] + tex_hoff + tex_mid_height;
   tex_coord[6] = T_TR[0];
   tex_coord[7] = T_TR[1] + tex_hoff + tex_mid_height;

   dispctx->draw(&draw, userdata, video_width, video_height);

   /* bottom-middle section */
   vert_coord[0] = V_BL[0] + vert_woff;
   vert_coord[1] = V_BL[1] - vert_hoff - vert_scaled_mid_height;
   vert_coord[2] = V_BR[0] + vert_scaled_mid_width;
   vert_coord[3] = V_BR[1] - vert_hoff - vert_scaled_mid_height;
   vert_coord[4] = V_TL[0] + vert_woff;
   vert_coord[5] = V_TL[1] - vert_hoff - vert_scaled_mid_height;
   vert_coord[6] = V_TR[0] + vert_scaled_mid_width;
   vert_coord[7] = V_TR[1] - vert_hoff - vert_scaled_mid_height;

   tex_coord[0] = T_BL[0] + tex_woff;
   tex_coord[1] = T_BL[1] + tex_hoff + tex_mid_height;
   tex_coord[2] = T_BR[0] + tex_mid_width;
   tex_coord[3] = T_BR[1] + tex_hoff + tex_mid_height;
   tex_coord[4] = T_TL[0] + tex_woff;
   tex_coord[5] = T_TL[1] + tex_hoff + tex_mid_height;
   tex_coord[6] = T_TR[0] + tex_mid_width;
   tex_coord[7] = T_TR[1] + tex_hoff + tex_mid_height;

   dispctx->draw(&draw, userdata, video_width, video_height);

   /* bottom-right corner */
   vert_coord[0] = V_BL[0] + vert_woff + vert_scaled_mid_width;
   vert_coord[1] = V_BL[1] - vert_hoff - vert_scaled_mid_height;
   vert_coord[2] = V_BR[0] + vert_scaled_mid_width + vert_woff;
   vert_coord[3] = V_BR[1] - vert_hoff - vert_scaled_mid_height;
   vert_coord[4] = V_TL[0] + vert_woff + vert_scaled_mid_width;
   vert_coord[5] = V_TL[1] - vert_hoff - vert_scaled_mid_height;
   vert_coord[6] = V_TR[0] + vert_scaled_mid_width + vert_woff;
   vert_coord[7] = V_TR[1] - vert_hoff - vert_scaled_mid_height;

   tex_coord[0] = T_BL[0] + tex_woff + tex_mid_width;
   tex_coord[1] = T_BL[1] + tex_hoff + tex_mid_height;
   tex_coord[2] = T_BR[0] + tex_woff + tex_mid_width;
   tex_coord[3] = T_BR[1] + tex_hoff + tex_mid_height;
   tex_coord[4] = T_TL[0] + tex_woff + tex_mid_width;
   tex_coord[5] = T_TL[1] + tex_hoff + tex_mid_height;
   tex_coord[6] = T_TR[0] + tex_woff + tex_mid_width;
   tex_coord[7] = T_TR[1] + tex_hoff + tex_mid_height;

   dispctx->draw(&draw, userdata, video_width, video_height);
}

void gfx_display_rotate_z(gfx_display_t *p_disp,
      math_matrix_4x4 *matrix, float cosine, float sine, void *data)
{
   gfx_display_ctx_driver_t *dispctx  = p_disp->dispctx;
   math_matrix_4x4 *b                 = (dispctx->get_default_mvp) 
      ? (math_matrix_4x4*)dispctx->get_default_mvp(data)
      : NULL;
   if (b)
   {
      static math_matrix_4x4 rot         = {
         {  0.0f,          0.0f,          0.0f,          0.0f ,
            0.0f,          0.0f,          0.0f,          0.0f ,
            0.0f,          0.0f,          1.0f,          0.0f ,
            0.0f,          0.0f,          0.0f,          1.0f } 
      };
      MAT_ELEM_4X4(rot, 0, 0)            = cosine;
      MAT_ELEM_4X4(rot, 0, 1)            = -sine;
      MAT_ELEM_4X4(rot, 1, 0)            = sine;
      MAT_ELEM_4X4(rot, 1, 1)            = cosine;
      matrix_4x4_multiply(*matrix, rot, *b);
   }
}

/*
 * Draw a hardware cursor on top of the screen for the mouse.
 */
void gfx_display_draw_cursor(
      gfx_display_t *p_disp,
      void *userdata,
      unsigned video_width,
      unsigned video_height,
      bool cursor_visible,
      float *color, float cursor_size, uintptr_t texture,
      float x, float y, unsigned width, unsigned height)
{
   gfx_display_ctx_draw_t draw;
   struct video_coords coords;
   gfx_display_ctx_driver_t *dispctx = p_disp->dispctx;

   if (!dispctx)
      return;

   coords.vertices      = 4;
   coords.vertex        = NULL;
   coords.tex_coord     = NULL;
   coords.lut_tex_coord = NULL;
   coords.color         = (const float*)color;

   draw.x               = x - (cursor_size / 2);
   draw.y               = (int)height - y - (cursor_size / 2);
   draw.width           = cursor_size;
   draw.height          = cursor_size;
   draw.coords          = &coords;
   draw.matrix_data     = NULL;
   draw.texture         = texture;
   draw.prim_type       = GFX_DISPLAY_PRIM_TRIANGLESTRIP;
   draw.pipeline_id     = 0;
   draw.scale_factor    = 1.0f;
   draw.rotation        = 0.0f;

   if (dispctx->blend_begin)
      dispctx->blend_begin(userdata);
   if (dispctx->draw)
      dispctx->draw(&draw, userdata, video_width, video_height);
   if (dispctx->blend_end)
      dispctx->blend_end(userdata);
}

static struct gfx_osk_state osk_state = {0,};
struct gfx_osk_state * gfx_state_get_ptr(void)
{
   return &osk_state;
}
/* Returns the OSK key at a given position */
int gfx_display_osk_ptr_at_pos(void *data, int x, int y,
      unsigned width, unsigned height)
{
   unsigned i;
   int ptr_width  = width / OSK_CHARS_PER_LINE;
   int ptr_height = height / 10;

   if (ptr_width > ptr_height)
      ptr_width = ptr_height;
   osk_state.mouse_x   = x;
   osk_state.mouse_y   = y;
   if( y > osk_state.bottom) return -1;

   for (i = 0; i<OSK_CHARS_MAX; i++)
   {
      int line_y    = (i/OSK_CHARS_PER_LINE)*height/10.0;
      int ptr_x     = width/2.0 - (OSK_CHARS_PER_LINE*ptr_width)/2.0 + (i%OSK_CHARS_PER_LINE) * ptr_width;
      int ptr_y     = height/2.0 + line_y;

      if (x > ptr_x && x < ptr_x + ptr_width
       && y > ptr_y && y < ptr_y + ptr_height)
         return i;
   }

   return -1;
}

/* Get the display framebuffer's size dimensions. */
void gfx_display_get_fb_size(unsigned *fb_width,
      unsigned *fb_height, size_t *fb_pitch)
{
   gfx_display_t *p_disp = &dispgfx_st;
   *fb_width             = p_disp->framebuf_width;
   *fb_height            = p_disp->framebuf_height;
   *fb_pitch             = p_disp->framebuf_pitch;
}

/* Set the display framebuffer's width. */
void gfx_display_set_width(unsigned width)
{
   gfx_display_t *p_disp  = &dispgfx_st;
   p_disp->framebuf_width = width;
}

/* Set the display framebuffer's height. */
void gfx_display_set_height(unsigned height)
{
   gfx_display_t *p_disp   = &dispgfx_st;
   p_disp->framebuf_height = height;
}

void gfx_display_set_framebuffer_pitch(size_t pitch)
{
   gfx_display_t *p_disp  = &dispgfx_st;
   p_disp->framebuf_pitch = pitch;
}

static float c_red[16]   = COLOR_HEX_TO_FLOAT(0x905060, 1.0f);
static float c_dark[16]  = COLOR_HEX_TO_FLOAT(0x303030, 0.5f);
static float c_white[16] = COLOR_HEX_TO_FLOAT(0xFFFFFF, 1.0f);
static float c_blue[16]  = COLOR_HEX_TO_FLOAT(0x0000FF, 0.5f);
#define MAX_EDIT_LINE  10
void  gfx_display_draw_edit(
      gfx_display_t *p_disp,
      void *userdata,
      unsigned vw,  unsigned vh,
      const font_data_t *font,
      unsigned text_color, 
      const char *src, 
      const char *mark, 
      int ptr,
      float scale   )
{
   int    i;
   int    margin    = (8.0f*scale>1.0f) ? (8.0f*scale) : 1;
   int    padding   = (8.0f*scale>1.0f) ? (8.0f*scale) : 1;
   float  font_size = (font->size+0.6f)*scale>2.0f ? (font->size+0.5f)*scale : 2.0f;
   int    px1       = (scale*1 > 1) ? (scale*1) : 1;
   int    px2       = (scale*2 > 2) ? (scale*2) : 2;
   int    gap       = font_size*2+1; 
   int    button    = gap + padding*2 - px1;
   int    base_x    = margin + (button + px2 ) + px2 + px1 + padding;
   int    base_y    = vh - margin - padding - px2;
   int    line      = 0;
   char*  ptxt   [MAX_EDIT_LINE]={0,};
   int    pmouse [MAX_EDIT_LINE]={0,};
   int    pcursor[MAX_EDIT_LINE]={0,};
   int    len       = strlen(src);
   char*  txt       = (char*) malloc(len+MAX_EDIT_LINE);	 
   int    cursor_x  = 0;
   int    cursor_line = 0;
   int    mouse_line  = (base_y - osk_state.mouse_y) / gap;
   if( !src ) return ;
   memset( txt,0x00,len+MAX_EDIT_LINE);  
   osk_state.ptr = 0;
   memset(osk_state.dir,0,sizeof(osk_state.dir));
   {
      int   x     = 0;
      char* sta   = (char*)txt;
      char* prv   = (char*)src;
      int   ulen  = utf8len(src);
      int   width = vw - margin*2  - px2 - padding*2 - (button+px2+px1)*2;
      char* cur   = sta;
      for( i = 0; i<ulen; i++)
      {
          char* nxt  = prv;
          int   size = 0;
          uint64_t ch= utf8_get(&nxt,&size);
          int   add  = font_driver_get_message_width( (void*)font, (char*)&ch, strlen((char*)&ch), 1.0f);
          int   left,mid,right;
          if( x+add >= width)
          {
              *cur = 0;
              cur++;
              ptxt[line] = sta;
              sta = cur;
              line++;
              x = 0;
              if( line>=MAX_EDIT_LINE) break;
          }
          left = x+base_x;
          mid  = x+base_x+add/2;
          right= x+base_x+add;
          if( i==ulen-1) right =  vw - margin*2 - px2 - button;
          if( osk_state.mouse_x>=left  && osk_state.mouse_x<mid    ) pmouse[line]  = 1+(cur-txt)-line;     else
          if( osk_state.mouse_x>=mid   && osk_state.mouse_x<right  ) pmouse[line]  = 1+(cur-txt)+size-line;   
          if( osk_state.cursor_x>=left && osk_state.cursor_x<mid   ) pcursor[line] = 1+(cur-txt)-line;     else
          if( osk_state.cursor_x>=mid  && osk_state.cursor_x<right ) pcursor[line] = 1+(cur-txt)+size-line;   
          if( ptr == prv-src ) osk_state.dir[1] = (nxt-src)+1;
          x=x+add;
          if( ptr == nxt-src )
          {
              cursor_line = line;
              cursor_x = x;
              osk_state.dir[0] = (prv-src)+1;
          }
          memcpy(cur, prv, size);
          cur +=size;
          *(cur) = 0;
          prv = nxt;
          if( i>=ulen-1)  ptxt[line] = sta;
      }
      line++;
   }
   base_y -= gap*line;
   osk_state.cursor_x = base_x + cursor_x;
   osk_state.cursor_y = base_y + cursor_line * gap;
   if( mouse_line>=0  && mouse_line<line)    osk_state.ptr   = pmouse[line-mouse_line-1];
   if( cursor_line>=1 && cursor_line<line)   osk_state.dir[2]= pcursor[cursor_line-1];
   if( cursor_line>=0 && cursor_line<line-1) osk_state.dir[3]= pcursor[cursor_line+1];
   /* show_border */
   {
        static const char* kbd  = "KB";
        int  x = base_x - padding - px1;
        int  y = base_y - padding - px1;
        int  w = vw     - margin*2 - px2 - (button+px2+px2)*2;
        int  h = gap*line + padding*2 -px1;
        int  font_y = base_y + gap*(line-1) + gap/2 + (font_size+0.5)/3 + px2+ px1; 
        int  font_x = 0;
        osk_state.bottom = y;
        gfx_display_draw_quad( p_disp, userdata, vw, vh, x,y,       w+px2, px1, vw, vh, c_red, NULL);
        gfx_display_draw_quad( p_disp, userdata, vw, vh, x,y+h+px1, w+px2, px1, vw, vh, c_red, NULL);
        gfx_display_draw_quad( p_disp, userdata, vw, vh, x,y,       px1, h+px2, vw, vh, c_red, NULL);
        gfx_display_draw_quad( p_disp, userdata, vw, vh, x+w+px1,y, px1, h+px2, vw, vh, c_red, NULL);
        gfx_display_draw_quad( p_disp, userdata, vw, vh, x+px1, y+px1,  w, h,   vw, vh, c_dark,   NULL);
        /* right button */
        x = vw - margin - button - px2;
        w = button;
        h = button;
        y = y + (line-1)*gap;
        gfx_display_draw_quad( p_disp, userdata, vw, vh, x,y,       w+px2, px1, vw, vh, c_blue, NULL);
        gfx_display_draw_quad( p_disp, userdata, vw, vh, x,y+h+px1, w+px2, px1, vw, vh, c_blue, NULL);
        gfx_display_draw_quad( p_disp, userdata, vw, vh, x,y,       px1, h+px2, vw, vh, c_blue, NULL);
        gfx_display_draw_quad( p_disp, userdata, vw, vh, x+w+px1,y, px1, h+px2, vw, vh, c_blue, NULL);
        gfx_display_draw_quad( p_disp, userdata, vw, vh, x+px1, y+px1,  w, h,   vw, vh, c_dark, NULL);
        font_x = x + (button - font_driver_get_message_width((void*)font,(char*)kbd,strlen((char*)kbd),1.0f) + 1)/2;
        gfx_display_draw_text( font, kbd, font_x, font_y, vw, vh, 0xffffffff, TEXT_ALIGN_LEFT, 1.0f, false, 1.0f, false);  
        if( osk_state.mouse_x>x && osk_state.mouse_x<=x+w && osk_state.mouse_y>y && osk_state.mouse_y<=y+h )
           osk_state.ptr = -1;
        /* left button */
        x = margin;
        gfx_display_draw_quad( p_disp, userdata, vw, vh, x,y,       w+px2, px1, vw, vh, c_red, NULL);
        gfx_display_draw_quad( p_disp, userdata, vw, vh, x,y+h+px1, w+px2, px1, vw, vh, c_red, NULL);
        gfx_display_draw_quad( p_disp, userdata, vw, vh, x,y,       px1, h+px2, vw, vh, c_red, NULL);
        gfx_display_draw_quad( p_disp, userdata, vw, vh, x+w+px1,y, px1, h+px2, vw, vh, c_red, NULL);
        gfx_display_draw_quad( p_disp, userdata, vw, vh, x+px1, y+px1,  w, h,   vw, vh, c_blue,NULL);
        font_x = x + (button - font_driver_get_message_width((void*)font,(char*)mark,strlen((char*)mark),1.0f) + 1)/2;
        gfx_display_draw_text( font, mark, font_x, font_y, vw, vh, 0xffffffff, TEXT_ALIGN_LEFT, 1.0f, false, 1.0f, false);
        if( osk_state.mouse_x>x && osk_state.mouse_x<=x+w && osk_state.mouse_y>y && osk_state.mouse_y<=y+h )
            osk_state.ptr = -2;
   }
   /* show_edit_text */
   for (i = 0; i<line; i++)
   {
       const char *msg =  ptxt[i];
       int x = base_x ;
       int y = base_y + i*gap + gap/2 + (font_size+0.5)/3  + px2; 
       gfx_display_draw_text( font, msg, x, y, vw, vh, text_color, TEXT_ALIGN_LEFT, 1.0f, false, 1.0f, false);
   }
   /* show_cursor */
   {
        static int show     = 0;
        static int last_ptr = 0;
        static retro_time_t last_time = 0;
        retro_time_t        cur_time  = cpu_features_get_time_usec();
        int   h  = gap + px2 - padding;
        if( ptr != last_ptr || (cur_time - last_time >= 0.5f*1000000) )
        {
            show = !show;
            if( ptr != last_ptr )	show = 1;
            last_ptr   = ptr;
            last_time  = cur_time;
        }
        if( show ) gfx_display_draw_quad( p_disp, userdata, vw, vh, osk_state.cursor_x, osk_state.cursor_y, px2, h, vw, vh, c_white, NULL);
   }
   return;
}

void gfx_display_draw_keyboard(
      gfx_display_t *p_disp,
      void *userdata,
      unsigned video_width,
      unsigned video_height,
      uintptr_t hover_texture,
      const font_data_t *font,
      char *grid[], unsigned id,
      unsigned text_color)
{
   unsigned i;
   int ptr_width, ptr_height;
   gfx_display_ctx_driver_t *dispctx = p_disp->dispctx;


   if( grid==NULL ) return;
   if( osk_state.hide_keyboard )    return;

#ifdef HAVE_MIST
   if (steam_has_osk_open())
      return;
#endif

   gfx_display_draw_quad(
         p_disp,
         userdata,
         video_width,
         video_height,
         0,
         video_height / 2.0,
         video_width,
         video_height / 2.0,
         video_width,
         video_height,
         c_dark,
         NULL);

   ptr_width  = video_width  / OSK_CHARS_PER_LINE;
   ptr_height = video_height / 10;

   if (ptr_width > ptr_height)
      ptr_width = ptr_height;

   for (i = 0; i < OSK_CHARS_MAX; i++)
   {
      int line_y     = (i/OSK_CHARS_PER_LINE) * video_height / 10.0;
      unsigned color = 0xffffffff;

      if (i == id)
      {
         if (dispctx && dispctx->blend_begin)
            dispctx->blend_begin(userdata);

         gfx_display_draw_quad(
           p_disp,
           userdata,
           video_width,
           video_height,
           video_width / 2.0 - (OSK_CHARS_PER_LINE*ptr_width) / 2.0 + (i%OSK_CHARS_PER_LINE) * ptr_width,
           video_height / 2.0 + line_y,
           ptr_width, ptr_height,
           video_width,
           video_height,
           &c_white[0],
           &hover_texture);

         if (dispctx && dispctx->blend_end)
            dispctx->blend_end(userdata);

         color = text_color;
      }

      gfx_display_draw_text(font, grid[i],
            video_width/2.0 - (OSK_CHARS_PER_LINE*ptr_width)/2.0 + (i%OSK_CHARS_PER_LINE)
            * ptr_width + ptr_width/2.0,
            video_height / 2.0  + line_y + font->size / 3 + (ptr_height/2),
            video_width,
            video_height,
            color,
            TEXT_ALIGN_CENTER,
            1.0f,
            false, 0, false);
   }
}

/* NOTE: Reads image from memory buffer */
bool gfx_display_reset_textures_list_buffer(
        uintptr_t *item, enum texture_filter_type filter_type,
        void* buffer, unsigned buffer_len, enum image_type_enum image_type,
        unsigned *width, unsigned *height)
{
   struct texture_image ti;

   ti.width                      = 0;
   ti.height                     = 0;
   ti.pixels                     = NULL;
   ti.supports_rgba              = video_driver_supports_rgba();

   if (!image_texture_load_buffer(&ti, image_type, buffer, buffer_len))
      return false;

   if (width)
      *width = ti.width;

   if (height)
      *height = ti.height;

   /* if the poke interface doesn't support texture load then return false */  
   if (!video_driver_texture_load(&ti, filter_type, item))
       return false;
   image_texture_free(&ti);
   return true;
}

/* NOTE: Reads image from file */
bool gfx_display_reset_textures_list(
      const char *texture_path, const char *iconpath,
      uintptr_t *item, enum texture_filter_type filter_type,
      unsigned *width, unsigned *height)
{
   char texpath[PATH_MAX_LENGTH];
   struct texture_image ti;

   ti.width                      = 0;
   ti.height                     = 0;
   ti.pixels                     = NULL;
   ti.supports_rgba              = video_driver_supports_rgba();

   if (string_is_empty(texture_path))
      return false;

   fill_pathname_join_special(texpath,
         iconpath, texture_path, sizeof(texpath));

   if (!image_texture_load(&ti, texpath))
      return false;

   if (width)
      *width = ti.width;

   if (height)
      *height = ti.height;

   video_driver_texture_load(&ti,
         filter_type, item);
   image_texture_free(&ti);

   return true;
}

/* Teardown; deinitializes and frees all
 * fonts associated to the display driver */
void gfx_display_font_free(font_data_t *font)
{
   font_driver_free(font);
}

void gfx_display_deinit_white_texture(void)
{
   if (gfx_white_texture)
      video_driver_texture_unload(&gfx_white_texture);
   gfx_white_texture = 0;
}

void gfx_display_init_white_texture(void)
{
   struct texture_image ti;
   static const uint8_t white_data[] = { 0xff, 0xff, 0xff, 0xff };

   ti.width  = 1;
   ti.height = 1;
   ti.pixels = (uint32_t*)&white_data;

   video_driver_texture_load(&ti,
         TEXTURE_FILTER_NEAREST, &gfx_white_texture);
}

void gfx_display_free(void)
{
   gfx_display_t *p_disp       = &dispgfx_st;
   video_coord_array_free(&p_disp->dispca);

   p_disp->flags              &= ~(GFX_DISP_FLAG_MSG_FORCE
                                 | GFX_DISP_FLAG_HAS_WINDOWED
                                  );
   p_disp->header_height       = 0;
   p_disp->framebuf_width      = 0;
   p_disp->framebuf_height     = 0;
   p_disp->framebuf_pitch      = 0;
   p_disp->dispctx             = NULL;
}

void gfx_display_init(void)
{
   gfx_display_t *p_disp         = &dispgfx_st;
   video_coord_array_t *p_dispca = &p_disp->dispca;

   if (video_driver_has_windowed())
      p_disp->flags             |=  GFX_DISP_FLAG_HAS_WINDOWED;
   else
      p_disp->flags             &= ~GFX_DISP_FLAG_HAS_WINDOWED;
   p_dispca->allocated           =  0;
}

bool gfx_display_init_first_driver(gfx_display_t *p_disp,
      bool video_is_threaded)
{
   unsigned i;
   const char *video_driver = video_driver_get_ident();

   for (i = 0; gfx_display_ctx_drivers[i]; i++)
   {
      if (!gfx_display_check_compatibility(video_driver,
               gfx_display_ctx_drivers[i]->type,
               gfx_display_ctx_drivers[i]->ident,
               video_is_threaded))
         continue;

      RARCH_LOG("[Display]: Found display driver: \"%s\".\n",
            gfx_display_ctx_drivers[i]->ident);
      p_disp->dispctx = gfx_display_ctx_drivers[i];
      return true;
   }
   return false;
}
