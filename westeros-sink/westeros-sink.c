/*
 * Copyright (C) 2016 RDK Management
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "westeros-sink.h"

#include "westeros-version.h"

#ifdef ENABLE_SW_DECODE
#include "../../westeros-sink/westeros-sink-sw.c"
#endif

#ifdef USE_GST_VIDEO
#include <gst/video/gstvideometa.h>
#endif

#define GST_PACKAGE_ORIGIN "http://gstreamer.net/"

static GstStaticPadTemplate gst_westeros_sink_pad_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(WESTEROS_SINK_CAPS));

GST_DEBUG_CATEGORY (gst_westeros_sink_debug);
#define GST_CAT_DEFAULT gst_westeros_sink_debug

enum
{
  PROP_0,
  PROP_WINDOW_SET,
  PROP_ZORDER,
  PROP_OPACITY,
  PROP_VIDEO_WIDTH,
  PROP_VIDEO_HEIGHT,
  PROP_ENABLE_TIMECODE,
  PROP_VIDEO_PTS,
  PROP_RES_PRIORITY,
  PROP_RES_USAGE
};

#ifdef USE_GST1
#define gst_westeros_sink_parent_class parent_class
G_DEFINE_TYPE (GstWesterosSink, gst_westeros_sink, GST_TYPE_BASE_SINK)
#else
GST_BOILERPLATE (GstWesterosSink, gst_westeros_sink, GstBaseSink, GST_TYPE_BASE_SINK)
#endif

static bool resMgrCheckUse( GstWesterosSinkClass *klass );
static void resMgrInit( GstWesterosSink *sink );
static void resMgrTerm( GstWesterosSink *sink );
static void resMgrNotify( EssRMgr *rm, int event, int type, int id, void* userData );
static void resMgrRequestDecoder( GstWesterosSink *sink );
static void resMgrReleaseDecoder( GstWesterosSink *sink );
static void gst_westeros_sink_term(GstWesterosSink *sink); 
static void gst_westeros_sink_finalize(GObject *object); 
static void gst_westeros_sink_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void gst_westeros_sink_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static GstStateChangeReturn gst_westeros_sink_change_state(GstElement *element, GstStateChange transition);
static gboolean gst_westeros_sink_query(GstElement *element, GstQuery *query);
static gboolean gst_westeros_sink_start(GstBaseSink *base_sink);
static gboolean gst_westeros_sink_stop(GstBaseSink *base_sink);
static gboolean gst_westeros_sink_unlock(GstBaseSink *base_sink);
static gboolean gst_westeros_sink_unlock_stop(GstBaseSink *base_sink);
static gboolean gst_westeros_sink_check_caps(GstWesterosSink *sink, GstPad *peer);
#ifdef USE_GST1
static gboolean gst_westeros_sink_event(GstPad *pad, GstObject *parent, GstEvent *event);
static GstPadLinkReturn gst_westeros_sink_link(GstPad *pad, GstObject *parent, GstPad *peer);
static void gst_westeros_sink_unlink(GstPad *pad, GstObject *parent);
static gboolean gst_westeros_sink_sink_query(GstPad *pad, GstObject *parent, GstQuery *query);
#else
static gboolean gst_westeros_sink_event(GstPad *pad, GstEvent *event);
static GstPadLinkReturn gst_westeros_sink_link(GstPad *pad, GstPad *peer);
static void gst_westeros_sink_unlink(GstPad *pad);
static gboolean gst_westeros_sink_sink_query(GstPad *pad, GstQuery *query);
#endif
static GstFlowReturn gst_westeros_sink_render(GstBaseSink *base_sink, GstBuffer *buffer);
static GstFlowReturn gst_westeros_sink_preroll(GstBaseSink *base_sink, GstBuffer *buffer);


static void shellSurfaceId(void *data,
                           struct wl_simple_shell *wl_simple_shell,
                           struct wl_surface *surface,
                           uint32_t surfaceId)
{
   GstWesterosSink *sink= (GstWesterosSink*)data;
   sink->surfaceId= surfaceId;
   char name[32];
   wl_fixed_t z, op;
   WESTEROS_UNUSED(wl_simple_shell);
   WESTEROS_UNUSED(surface);

   sprintf( name, "westeros-sink-surface-%x", surfaceId );
   wl_simple_shell_set_name( sink->shell, surfaceId, name );
   if ( (sink->windowWidth == 0) || (sink->windowHeight == 0) )
   {
      wl_simple_shell_set_visible( sink->shell, sink->surfaceId, false);
   }
   else
   {
      if ( sink->show )
      {
         wl_simple_shell_set_visible( sink->shell, sink->surfaceId, true);
      }
      if ( !sink->vpc )
      {
         wl_simple_shell_set_geometry( sink->shell, sink->surfaceId, sink->windowX, sink->windowY, sink->windowWidth, sink->windowHeight );
      }
   }

   z= wl_fixed_from_double(sink->zorder);
   wl_simple_shell_set_zorder( sink->shell, sink->surfaceId, z);
   op= wl_fixed_from_double(sink->opacity);
   wl_simple_shell_set_opacity( sink->shell, sink->surfaceId, op);
   wl_simple_shell_get_status( sink->shell, sink->surfaceId );

   wl_display_flush(sink->display);
}

static void shellSurfaceCreated(void *data,
                                struct wl_simple_shell *wl_simple_shell,
                                uint32_t surfaceId,
                                const char *name)
{
   WESTEROS_UNUSED(data);
   WESTEROS_UNUSED(wl_simple_shell);
   WESTEROS_UNUSED(surfaceId);
   WESTEROS_UNUSED(name);
}
                                
static void shellSurfaceDestroyed(void *data,
                                  struct wl_simple_shell *wl_simple_shell,
                                  uint32_t surfaceId,
                                  const char *name)
{
   WESTEROS_UNUSED(data);
   WESTEROS_UNUSED(wl_simple_shell);
   WESTEROS_UNUSED(surfaceId);
   WESTEROS_UNUSED(name);
}
                                  
static void shellSurfaceStatus(void *data,
                               struct wl_simple_shell *wl_simple_shell,
                               uint32_t surfaceId,
                               const char *name,
                               uint32_t visible,
                               int32_t x,
                               int32_t y,
                               int32_t width,
                               int32_t height,
                               wl_fixed_t opacity,
                               wl_fixed_t zorder)
{
   GstWesterosSink *sink= (GstWesterosSink*)data;
   WESTEROS_UNUSED(wl_simple_shell);
   WESTEROS_UNUSED(surfaceId);
   WESTEROS_UNUSED(name);
   WESTEROS_UNUSED(x);
   WESTEROS_UNUSED(y);
   WESTEROS_UNUSED(width);
   WESTEROS_UNUSED(height);
   if ( sink->show )
   {
      sink->visible= visible;
   }
   sink->windowChange= true;
   sink->opacity= opacity;
   sink->zorder= zorder;
}

static void shellGetSurfacesDone(void *data, struct wl_simple_shell *wl_simple_shell )
{
   WESTEROS_UNUSED(data);
   WESTEROS_UNUSED(wl_simple_shell);
}

static const struct wl_simple_shell_listener shellListener = 
{
   shellSurfaceId,
   shellSurfaceCreated,
   shellSurfaceDestroyed,
   shellSurfaceStatus,
   shellGetSurfacesDone
};

static void vpcVideoPathChange(void *data,
                               struct wl_vpc_surface *wl_vpc_surface,
                               uint32_t new_pathway )
{
   WESTEROS_UNUSED(wl_vpc_surface);
   GstWesterosSink *sink= (GstWesterosSink*)data;
   #ifdef ENABLE_SW_DECODE
   if ( sink->rm && (sink->resCurrCaps.capabilities & EssRMgrVidCap_software) )
   {
      return;
   }
   #endif
   printf("westeros-sink: new pathway: %d\n", new_pathway);
   gst_westeros_sink_soc_set_video_path( sink, (new_pathway == WL_VPC_SURFACE_PATHWAY_GRAPHICS) );
}                               

static void vpcVideoXformChange(void *data,
                                struct wl_vpc_surface *wl_vpc_surface,
                                int32_t x_translation,
                                int32_t y_translation,
                                uint32_t x_scale_num,
                                uint32_t x_scale_denom,
                                uint32_t y_scale_num,
                                uint32_t y_scale_denom,
                                uint32_t output_width,
                                uint32_t output_height)
{                                
   WESTEROS_UNUSED(wl_vpc_surface);
   GstWesterosSink *sink= (GstWesterosSink*)data;
      
   sink->transX= x_translation;
   sink->transY= y_translation;
   if ( x_scale_denom )
   {
      sink->scaleXNum= x_scale_num;
      sink->scaleXDenom= x_scale_denom;
   }
   if ( y_scale_denom )
   {
      sink->scaleYNum= y_scale_num;
      sink->scaleYDenom= y_scale_denom;
   }
   sink->outputWidth= (int)output_width;
   sink->outputHeight= (int)output_height;

   #ifdef ENABLE_SW_DECODE
   if ( sink->rm && (sink->resCurrCaps.capabilities & EssRMgrVidCap_software) )
   {
      return;
   }
   #endif
   
   LOCK( sink );
   gst_westeros_sink_soc_update_video_position( sink );
   UNLOCK( sink );
}

static const struct wl_vpc_surface_listener vpcListener= {
   vpcVideoPathChange,
   vpcVideoXformChange
};

static void outputHandleGeometry( void *data,
                                  struct wl_output *output,
                                  int x,
                                  int y,
                                  int mmWidth,
                                  int mmHeight,
                                  int subPixel,
                                  const char *make,
                                  const char *model,
                                  int transform )
{
   WESTEROS_UNUSED(data);
   WESTEROS_UNUSED(output);
   WESTEROS_UNUSED(x);
   WESTEROS_UNUSED(y);
   WESTEROS_UNUSED(mmWidth);
   WESTEROS_UNUSED(mmHeight);
   WESTEROS_UNUSED(subPixel);
   WESTEROS_UNUSED(make);
   WESTEROS_UNUSED(model);
   WESTEROS_UNUSED(transform);
}

static void outputHandleMode( void *data,
                              struct wl_output *output,
                              uint32_t flags,
                              int width,
                              int height,
                              int refreshRate )
{
   GstWesterosSink *sink= (GstWesterosSink*)data;

   if ( flags & WL_OUTPUT_MODE_CURRENT )
   {
      LOCK( sink );
      sink->displayWidth= width;
      sink->displayHeight= height;
      if ( !sink->windowSizeOverride )
      {
         printf("westeros-sink: compositor sets window to (%dx%d)\n", width, height);
         sink->windowWidth= width;
         sink->windowHeight= height;
         if ( sink->vpcSurface )
         {
            wl_vpc_surface_set_geometry( sink->vpcSurface, sink->windowX, sink->windowY, sink->windowWidth, sink->windowHeight );
         }
      }
      UNLOCK( sink );
   }
}

static void outputHandleDone( void *data,
                              struct wl_output *output )
{
   WESTEROS_UNUSED(data);
   WESTEROS_UNUSED(output);
}

static void outputHandleScale( void *data,
                               struct wl_output *output,
                               int32_t scale )
{
   WESTEROS_UNUSED(data);
   WESTEROS_UNUSED(output);
   WESTEROS_UNUSED(scale);
}

static const struct wl_output_listener outputListener = {
   outputHandleGeometry,
   outputHandleMode,
   outputHandleDone,
   outputHandleScale
};

static void registryHandleGlobal(void *data, 
                                 struct wl_registry *registry, uint32_t id,
		                           const char *interface, uint32_t version);
static void registryHandleGlobalRemove(void *data, 
                                       struct wl_registry *registry,
			                              uint32_t name);

static const struct wl_registry_listener registryListener = 
{
	registryHandleGlobal,
	registryHandleGlobalRemove
};

static void registryHandleGlobal(void *data, 
                                 struct wl_registry *registry, uint32_t id,
		                           const char *interface, uint32_t version)
{
   GstWesterosSink *sink= (GstWesterosSink*)data;
   int len;

   printf("westeros-sink: registry: id %d interface (%s) version %d\n", id, interface, version );
   
   len= strlen(interface);
   if ((len==13) && (strncmp(interface, "wl_compositor",len) == 0)) 
   {
      sink->compositor= (struct wl_compositor*)wl_registry_bind(registry, id, &wl_compositor_interface, 1);
      printf("westeros-sink: compositor %p\n", (void*)sink->compositor);
      wl_proxy_set_queue((struct wl_proxy*)sink->compositor, sink->queue);
   }
   else if ((len==15) && (strncmp(interface, "wl_simple_shell",len) == 0)) 
   {
      sink->shell= (struct wl_simple_shell*)wl_registry_bind(registry, id, &wl_simple_shell_interface, 1);
      printf("westeros-sink: shell %p\n", (void*)sink->shell);
      wl_proxy_set_queue((struct wl_proxy*)sink->shell, sink->queue);
      wl_simple_shell_add_listener(sink->shell, &shellListener, sink);
   }
   else if ((len==6) && (strncmp(interface, "wl_vpc", len) ==0))
   {
      sink->vpc= (struct wl_vpc*)wl_registry_bind(registry, id, &wl_vpc_interface, 1);
      printf("westeros-sink: registry: vpc %p\n", (void*)sink->vpc);
      wl_proxy_set_queue((struct wl_proxy*)sink->vpc, sink->queue);
   }
   else if ((len==9) && !strncmp(interface, "wl_output", len) )
   {
      sink->output= (struct wl_output*)wl_registry_bind(registry, id, &wl_output_interface, 2);
      printf("westeros-sink: registry: output %p\n", (void*)sink->output);
      wl_proxy_set_queue((struct wl_proxy*)sink->output, sink->queue);
      wl_output_add_listener(sink->output, &outputListener, sink);
   }
   gst_westeros_sink_soc_registryHandleGlobal( sink, registry, id, interface, version );

   wl_display_flush(sink->display);
}

static void registryHandleGlobalRemove(void *data, 
                                       struct wl_registry *registry,
			                              uint32_t name)
{
   GstWesterosSink *sink= (GstWesterosSink*)data;

   gst_westeros_sink_soc_registryHandleGlobalRemove( sink, registry, name );
}

#define DEFAULT_USAGE (EssRMgrVidUse_fullResolution|EssRMgrVidUse_fullQuality|EssRMgrVidUse_fullPerformance)

#define GST_TYPE_USAGE_FLAGS (gst_usage_flags_get_type())
GType gst_usage_flags_get_type( void )
{
   static volatile GType id= 0;
   static const GFlagsValue flagValues[]=
   {
      {(guint)(EssRMgrVidUse_fullResolution), "Play at full output resolution", "fullResolution"},
      {(guint)(EssRMgrVidUse_fullQuality), "Play at full quality", "fullQuality"},
      {(guint)(EssRMgrVidUse_fullPerformance), "Play with full performance", "fullPerformance"},
      {0, NULL,  NULL}
   };
   if ( g_once_init_enter( (gsize *)&id) )
   {
      GType flagTypeId;

      flagTypeId= g_flags_register_static( "EssRMgrVideoUsage", flagValues );

      g_once_init_leave( (gsize *)&id, flagTypeId);
   }

   return id;
}

static bool resMgrCheckUse( GstWesterosSinkClass *klass )
{
   bool result= false;

   if ( klass && klass->canUseResMgr )
   {
      result= true;
   }

   return result;
}

static void resMgrInit( GstWesterosSink *sink )
{
   GstWesterosSinkClass *klass= GST_WESTEROS_SINK_GET_CLASS(sink);

   if ( klass && resMgrCheckUse( klass ) )
   {
      sink->rm= EssRMgrCreate();
      if ( !sink->rm )
      {
         GST_ERROR("gst_westeros_sink: resMgrInit: failed to create resmgr");
      }

      sink->resAssignedId= -1;
      sink->resReqPrimary.sink= sink;
      sink->resReqPrimary.resReq.assignedId= -1;
      sink->resReqPrimary.resReq.requestId= -1;
      sink->resReqSecondary.sink= sink;
      sink->resReqSecondary.resReq.assignedId= -1;
      sink->resReqSecondary.resReq.requestId= -1;
      memset( &sink->resCurrCaps, 0, sizeof(EssRMgrCaps) );
   }
}

static void resMgrTerm( GstWesterosSink *sink )
{
   if ( sink->rm )
   {
      EssRMgrDestroy( sink->rm );
      sink->rm= 0;
      sink->resAssignedId= -1;
      sink->resReqPrimary.resReq.assignedId= -1;
      sink->resReqPrimary.resReq.requestId= -1;
      sink->resReqSecondary.resReq.assignedId= -1;
      sink->resReqSecondary.resReq.requestId= -1;
      memset( &sink->resCurrCaps, 0, sizeof(EssRMgrCaps) );
   }
}

static void resMgrNotify( EssRMgr *rm, int event, int type, int id, void* userData )
{
   WstSinkResReqInfo *info= (WstSinkResReqInfo*)userData;
   GstWesterosSink *sink= info->sink;

   GST_DEBUG("resMgrNotify: enter");
   switch( type )
   {
      case EssRMgrResType_videoDecoder:
         switch( event )
         {
            case EssRMgrEvent_granted:
               sink->resAssignedId= id;
               memset( &sink->resCurrCaps, 0, sizeof(EssRMgrCaps) );
               if ( !EssRMgrResourceGetCaps( sink->rm, EssRMgrResType_videoDecoder, sink->resAssignedId, &sink->resCurrCaps ) )
               {
                  GST_ERROR("gst_westeros_sink: resMgrNotify: failed to get caps of assigned decoder");
               }
               GST_DEBUG("async assigned id %d caps %X (%dx%d)",
                       sink->resAssignedId,
                       sink->resCurrCaps.capabilities,
                       sink->resCurrCaps.info.video.maxWidth,
                       sink->resCurrCaps.info.video.maxHeight  );
               break;
            case EssRMgrEvent_revoked:
               {
                  memset( &sink->resCurrCaps, 0, sizeof(EssRMgrCaps) );
                  GST_DEBUG("releasing video decoder %d", id);
                  sink->releaseResources( sink );
                  EssRMgrReleaseResource( sink->rm, EssRMgrResType_videoDecoder, id );
                  GST_DEBUG("done releasing video decoder %d", id);
                  sink->resAssignedId= -1;
                  if (
                       (EssRMgrGetPolicyPriorityTie( sink->rm ) == false) ||
                       (sink->resReqPrimary.resReq.priority != sink->resPriority)
                     )
                  {
                     resMgrRequestDecoder(sink);
                     if ( sink->resAssignedId >= 0 )
                     {
                        sink->acquireResources( sink );
                     }
                  }
               }
               break;
            default:
               break;
         }
         break;
      default:
         break;
   }
   GST_DEBUG("resMgrNotify: exit");
}

static void resMgrRequestDecoder( GstWesterosSink *sink )
{
   if ( sink->rm )
   {
      bool result;

      sink->resReqPrimary.resReq.type= EssRMgrResType_videoDecoder;
      sink->resReqPrimary.resReq.usage= sink->resUsage;
      sink->resReqPrimary.resReq.priority= sink->resPriority;
      sink->resReqPrimary.resReq.info.video.maxWidth= sink->windowWidth;
      sink->resReqPrimary.resReq.info.video.maxHeight= sink->windowHeight;
      sink->resReqPrimary.resReq.asyncEnable= true;
      sink->resReqPrimary.resReq.notifyCB= resMgrNotify;
      sink->resReqPrimary.resReq.notifyUserData= &sink->resReqPrimary;

      result= EssRMgrRequestResource( sink->rm, EssRMgrResType_videoDecoder, &sink->resReqPrimary.resReq );
      if ( result )
      {
         if ( sink->resReqPrimary.resReq.assignedId >= 0 )
         {
            GST_DEBUG("assigned id %d caps %X", sink->resReqPrimary.resReq.assignedId, sink->resReqPrimary.resReq.assignedCaps );
            sink->resAssignedId= sink->resReqPrimary.resReq.assignedId;
            memset( &sink->resCurrCaps, 0, sizeof(EssRMgrCaps) );
            if ( !EssRMgrResourceGetCaps( sink->rm, EssRMgrResType_videoDecoder, sink->resAssignedId, &sink->resCurrCaps ) )
            {
               GST_ERROR("gst_westeros_sink: resMgrRequestDecoder: failed to get caps of assigned decoder");
            }
            GST_DEBUG("assigned id %d caps %X (%dx%d)",
                      sink->resAssignedId,
                      sink->resCurrCaps.capabilities,
                      sink->resCurrCaps.info.video.maxWidth,
                      sink->resCurrCaps.info.video.maxHeight  );
         }
         else
         {
            GST_DEBUG("async grant pending" );
         }
      }
      else
      {
         GST_ERROR("gst_westeros_sink: resMgrRequestDecoder: request failed");
      }
   }
}

static void resMgrReleaseDecoder( GstWesterosSink *sink )
{
   if ( sink->rm )
   {
      if ( sink->resAssignedId >= 0 )
      {
         EssRMgrReleaseResource( sink->rm, EssRMgrResType_videoDecoder, sink->resAssignedId );
         sink->resReqPrimary.resReq.assignedId= -1;
         sink->resAssignedId= -1;
      }
   }
}

static void resMgrUpdateState( GstWesterosSink *sink, int state )
{
   if ( sink->rm )
   {
      if ( sink->resAssignedId >= 0 )
      {
         EssRMgrResourceSetState( sink->rm, EssRMgrResType_videoDecoder, sink->resAssignedId, state );
      }
   }
}

static gboolean gst_westeros_sink_backend_null_to_ready( GstWesterosSink *sink, gboolean *passToDefault )
{
   gboolean result;
   if ( sink->rm && (sink->resAssignedId < 0) )
   {
      result= TRUE;
   }
   #ifdef ENABLE_SW_DECODE
   else if ( sink->rm && (sink->resCurrCaps.capabilities & EssRMgrVidCap_software) )
   {
      result= wstsw_null_to_ready( sink, passToDefault );
   }
   #endif
   else
   {
      result= gst_westeros_sink_soc_null_to_ready( sink, passToDefault );
   }
   return result;
}

static gboolean gst_westeros_sink_backend_ready_to_paused( GstWesterosSink *sink, gboolean *passToDefault )
{
   gboolean result;
   if ( sink->rm && (sink->resAssignedId < 0) )
   {
      result= TRUE;
   }
   #ifdef ENABLE_SW_DECODE
   else if ( sink->rm && (sink->resCurrCaps.capabilities & EssRMgrVidCap_software) )
   {
      result= wstsw_ready_to_paused( sink, passToDefault );
   }
   #endif
   else
   {
      result= gst_westeros_sink_soc_ready_to_paused( sink, passToDefault );
   }
   if ( result && sink->rm && sink->resAssignedId >= 0 )
   {
      resMgrUpdateState( sink, EssRMgrRes_paused );
   }
   return result;
}

static gboolean gst_westeros_sink_backend_paused_to_playing( GstWesterosSink *sink, gboolean *passToDefault )
{
   gboolean result;
   if ( sink->rm && (sink->resAssignedId < 0) )
   {
      result= TRUE;
   }
   #ifdef ENABLE_SW_DECODE
   else if ( sink->rm && (sink->resCurrCaps.capabilities & EssRMgrVidCap_software) )
   {
      result= wstsw_paused_to_playing( sink, passToDefault );
   }
   #endif
   else
   {
      result= gst_westeros_sink_soc_paused_to_playing( sink, passToDefault );
   }
   if ( result && sink->rm && sink->resAssignedId >= 0 )
   {
      resMgrUpdateState( sink, EssRMgrRes_active );
   }
   return result;
}

static gboolean gst_westeros_sink_backend_playing_to_paused( GstWesterosSink *sink, gboolean *passToDefault )
{
   gboolean result;
   if ( sink->rm && (sink->resAssignedId < 0) )
   {
      result= TRUE;
   }
   #ifdef ENABLE_SW_DECODE
   else if ( sink->rm && (sink->resCurrCaps.capabilities & EssRMgrVidCap_software) )
   {
      result= wstsw_playing_to_paused( sink, passToDefault );
   }
   #endif
   else
   {
      result= gst_westeros_sink_soc_playing_to_paused( sink, passToDefault );
   }
   if ( result && sink->rm && sink->resAssignedId >= 0 )
   {
      resMgrUpdateState( sink, EssRMgrRes_paused );
   }
   return result;
}

static gboolean gst_westeros_sink_backend_paused_to_ready( GstWesterosSink *sink, gboolean *passToDefault )
{
   gboolean result;
   if ( sink->rm && (sink->resAssignedId < 0) )
   {
      result= TRUE;
   }
   #ifdef ENABLE_SW_DECODE
   else if ( sink->rm && (sink->resCurrCaps.capabilities & EssRMgrVidCap_software) )
   {
      result= wstsw_paused_to_ready( sink, passToDefault );
   }
   #endif
   else
   {
      result= gst_westeros_sink_soc_paused_to_ready( sink, passToDefault );
   }
   if ( sink->rm && sink->resAssignedId >= 0 )
   {
      resMgrUpdateState( sink, EssRMgrRes_idle );
   }
   return result;
}

static gboolean gst_westeros_sink_backend_ready_to_null( GstWesterosSink *sink, gboolean *passToDefault )
{
   gboolean result;
   if ( sink->rm && (sink->resAssignedId < 0) )
   {
      result= TRUE;
   }
   #ifdef ENABLE_SW_DECODE
   else if ( sink->rm && (sink->resCurrCaps.capabilities & EssRMgrVidCap_software) )
   {
      result= wstsw_ready_to_null( sink, passToDefault );
   }
   #endif
   else
   {
      result= gst_westeros_sink_soc_ready_to_null( sink, passToDefault );
   }
   return result;
}

#include <dlfcn.h>
static void captureInit( GstWesterosSink *sink )
{
   const char *env= getenv("WESTEROSSINK_ENABLE_CAPTURE");
   if ( env )
   {
      GST_DEBUG_OBJECT(sink, "WESTEROSSINK_ENABLE_CAPTURE=(%s)",env);
      void *module= dlopen( "libmediacapture.so.0.0.0", RTLD_NOW );
      if ( module )
      {
         MediaCaptureCreateContext captureCreateContext= (MediaCaptureCreateContext)dlsym( module, "MediaCaptureCreateContext" );
         MediaCaptureDestroyContext captureDestroyContext= (MediaCaptureDestroyContext)dlsym( module, "MediaCaptureDestroyContext" );
         GST_DEBUG_OBJECT(sink, "mediacapture module %p create %p destroy %p", module, captureCreateContext, captureDestroyContext);

         if ( captureCreateContext && captureDestroyContext )
         {
            sink->mediaCaptureContext= (*captureCreateContext)( GST_ELEMENT(sink) );
            printf("westeros-sink: mediaCaptureContext: %p\n", sink->mediaCaptureContext);
            if ( sink->mediaCaptureContext )
            {
               sink->mediaCaptureModule= module;
               sink->mediaCaptureDestroyContext= captureDestroyContext;
               module= 0;
            }
         }

         if ( module )
         {
            dlclose( module );
         }
      }
      else
      {
         printf("Unable to load capture module: %s\n", dlerror());
      }
   }
}

static void captureTerm( GstWesterosSink *sink )
{
   if ( sink )
   {
      if ( sink->mediaCaptureContext && sink->mediaCaptureDestroyContext )
      {
         sink->mediaCaptureDestroyContext( sink->mediaCaptureContext );
         sink->mediaCaptureContext= 0;
      }
      if ( sink->mediaCaptureModule )
      {
         //we get crashes if we call this
         //dlclose(sink->mediaCaptureModule);
         sink->mediaCaptureModule= 0;
      }
   }
}

static void timeCodeAdd( GstWesterosSink *sink, guint64 pts, guint hours, guint minutes, guint seconds )
{
   #ifdef USE_GST_VIDEO
   int i;
   guint64 firstNano, position;
   LOCK(sink);
   if (
        (hours < sink->timeCodeActive.hours) ||
        ((hours == sink->timeCodeActive.hours) && (minutes < sink->timeCodeActive.minutes)) ||
        ((hours == sink->timeCodeActive.hours) && (minutes == sink->timeCodeActive.minutes) && (seconds <= sink->timeCodeActive.seconds))
      )
   {
      goto exit;
   }
   position= pts / (GST_SECOND/90000LL);
   if ( sink->timeCodeCount )
   {
      for( i= 0; i < sink->timeCodeCount; ++i )
      {
         if ( (sink->timeCodes[i].hours == hours) &&
              (sink->timeCodes[i].minutes == minutes) &&
              (sink->timeCodes[i].seconds == seconds) )
         {
            if ( position < sink->timeCodes[i].position )
            {
               GST_DEBUG("update time code: PTS %lld : %d:%d:%d : count %d capacity %d", position, hours, minutes, seconds, sink->timeCodeCount, sink->timeCodeCapacity);
               sink->timeCodes[i].position= position;
            }
            goto exit;
         }
      }
   }
   if ( sink->timeCodeCount+1 >= sink->timeCodeCapacity )
   {
      WstSinkTimeCode *newTC= 0;
      int newCapacity= (sink->timeCodeCapacity ? sink->timeCodeCapacity*2 : 30);
      newTC= (WstSinkTimeCode*)calloc( newCapacity, sizeof(WstSinkTimeCode) );
      if ( !newTC )
      {
         GST_ERROR("No memory to grow time code capacity");
         goto exit;
      }
      GST_DEBUG("grow time code set from %d to %d", sink->timeCodeCapacity, newCapacity);
      if ( sink->timeCodes )
      {
         memcpy( newTC, sink->timeCodes, sink->timeCodeCapacity*sizeof(WstSinkTimeCode) );
         free( sink->timeCodes );
         sink->timeCodes= 0;
      }
      sink->timeCodes= newTC;
      sink->timeCodeCapacity= newCapacity;
   }

   i= sink->timeCodeCount++;
   GST_DEBUG("add time code: PTS %lld : %d:%d:%d : count %d capacity %d", position, hours, minutes, seconds, sink->timeCodeCount, sink->timeCodeCapacity);
   sink->timeCodes[i].hours= hours;
   sink->timeCodes[i].minutes= minutes;
   sink->timeCodes[i].seconds= seconds;
   sink->timeCodes[i].position= position;

exit:
   UNLOCK(sink);
   #endif
   return;
}

static void timeCodeFlush( GstWesterosSink *sink )
{
   #ifdef USE_GST_VIDEO
   GST_DEBUG("flush time codes");
   LOCK(sink);
   if ( sink->timeCodes )
   {
      free( sink->timeCodes );
      sink->timeCodes= 0;
      sink->timeCodeCapacity= 0;
   }
   sink->timeCodeCount= 0;
   memset( &sink->timeCodeActive, 0, sizeof(WstSinkTimeCode));
   UNLOCK(sink);
   #endif
}

static void timeCodePresent( GstWesterosSink *sink, guint64 position, guint signal )
{
   /* Must be called with sink lock */
   #ifdef USE_GST_VIDEO
   int i;
   bool found= false;
   guint hours, minutes, seconds;
   position= sink->currentPTS;
   for( i= 0; i < sink->timeCodeCount; ++i )
   {
      if ( sink->timeCodes[i].position == position )
      {
         found= true;
         hours= sink->timeCodes[i].hours;
         minutes= sink->timeCodes[i].minutes;
         seconds= sink->timeCodes[i].seconds;
         if ( i < sink->timeCodeCount-1 )
         {
            memmove( &sink->timeCodes[0], &sink->timeCodes[i+1], (sink->timeCodeCount-i-1)*sizeof(WstSinkTimeCode) );
         }
         sink->timeCodeCount= (sink->timeCodeCount-i-1);
         break;
      }
   }
   if ( found )
   {
      sink->timeCodeActive.hours= hours;
      sink->timeCodeActive.minutes= minutes;
      sink->timeCodeActive.seconds= seconds;
      UNLOCK(sink);

      GST_DEBUG("emit time code signal: (%d:%d:%d) PTS %lld", hours, minutes, seconds, position);
      g_signal_emit( G_OBJECT(sink),
                     signal,
                     0,
                     hours,
                     minutes,
                     seconds
                   );

      LOCK(sink);
   }
   #endif
}

static void releaseWaylandResources( GstWesterosSink *sink )
{
   if ( sink->display )
   {
      if ( sink->vpcSurface )
      {
         wl_vpc_surface_destroy( sink->vpcSurface );
         sink->vpcSurface= 0;
      }
      if ( sink->output )
      {
         wl_output_destroy( sink->output );
         sink->output= 0;
      }
      if ( sink->vpc )
      {
         wl_vpc_destroy( sink->vpc );
         sink->vpc= 0;
      }
      if ( sink->surface )
      {
         wl_surface_destroy( sink->surface );
         sink->surface= 0;
      }
      if ( sink->display && sink->queue )
      {
         wl_display_flush(sink->display);
         wl_display_roundtrip_queue(sink->display, sink->queue);
      }
      if ( sink->compositor )
      {
         wl_compositor_destroy( sink->compositor );
         sink->compositor= 0;
      }
      if ( sink->shell )
      {
         wl_simple_shell_destroy( sink->shell );
         sink->shell= 0;
      }
      if ( sink->registry )
      {
         wl_registry_destroy(sink->registry);
         sink->registry= 0;
      }
      if ( sink->queue )
      {
         wl_event_queue_destroy( sink->queue );
         sink->queue= 0;
      }
      if ( sink->display )
      {
         printf("westeros-sink: paused-to-ready: display=%p\n", (void*)sink->display);
         wl_display_disconnect(sink->display);
         sink->display= 0;
      }
   }
}

#ifndef USE_GST1
static void gst_westeros_sink_base_init(gpointer g_class)
{
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (g_class);

  GST_DEBUG_CATEGORY_INIT (gst_westeros_sink_debug,
                           #ifdef USE_RAW_SINK
                           "westerosrawsink",
                           0,
                           "westerosrawsink element"
                           #else
                           "westerossink",
                           0,
                           "westerossink element"
                           #endif
                          );

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_westeros_sink_pad_template));
  gst_element_class_set_details_simple (gstelement_class, "Westeros Sink",
      #ifdef USE_RAW_SINK
      "Sink/Video",
      #else
      "Codec/Decoder/Video/Sink/Video",
      #endif
      "Writes buffers to the westeros wayland compositor",
      "Comcast");
}
#endif

static void gst_westeros_sink_class_init(GstWesterosSinkClass *klass)
{
   GObjectClass *gobject_class= (GObjectClass *) klass;
   GstElementClass *gstelement_class= (GstElementClass *) klass;
   GstBaseSinkClass *gstbasesink_class= (GstBaseSinkClass *) klass;
   
   gobject_class->finalize= gst_westeros_sink_finalize;
   gobject_class->set_property= gst_westeros_sink_set_property;
   gobject_class->get_property= gst_westeros_sink_get_property;
   
   gstelement_class->change_state= gst_westeros_sink_change_state;
   gstelement_class->query= gst_westeros_sink_query;
   
   gstbasesink_class->start= GST_DEBUG_FUNCPTR (gst_westeros_sink_start);
   gstbasesink_class->stop= GST_DEBUG_FUNCPTR (gst_westeros_sink_stop);
   gstbasesink_class->unlock= GST_DEBUG_FUNCPTR (gst_westeros_sink_unlock);
   gstbasesink_class->unlock_stop= GST_DEBUG_FUNCPTR (gst_westeros_sink_unlock_stop);
   gstbasesink_class->render= GST_DEBUG_FUNCPTR (gst_westeros_sink_render);
   gstbasesink_class->preroll= GST_DEBUG_FUNCPTR (gst_westeros_sink_preroll);   
   g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_WINDOW_SET,
       g_param_spec_string ("window_set", "window set",
           "Window Set Format: x,y,width,height",
           NULL, G_PARAM_WRITABLE));

   g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_WINDOW_SET,
       g_param_spec_string ("rectangle", "rectangle",
           "Window Set Format: x,y,width,height",
           NULL, G_PARAM_WRITABLE));

   g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_ZORDER,
       g_param_spec_float ("zorder", "zorder",
           "zorder from 0.0 (lowest) to 1.0 (highest)",
           0.0, 1.0, 0.0, G_PARAM_WRITABLE));

   g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_OPACITY,
       g_param_spec_float ("opacity", "opacity",
           "opacity from 0.0 (transparent) to 1.0 (opaque)",
           0.0, 1.0, 1.0, G_PARAM_WRITABLE));

   g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_VIDEO_WIDTH,
       g_param_spec_int ("video_width", "video_width",
           "current video frame width",
           0, G_MAXINT32, 0, G_PARAM_READABLE));

   g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_VIDEO_HEIGHT,
       g_param_spec_int ("video_height", "video_height",
           "current video frame height",
           0, G_MAXINT32, 0, G_PARAM_READABLE));

   #ifdef USE_GST_VIDEO
   g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_ENABLE_TIMECODE,
       g_param_spec_boolean ("enable-timecode",
           "enable timecode signal",
           "0: disable; 1: enable", FALSE, G_PARAM_READWRITE));
   #endif

   g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_VIDEO_PTS,
       g_param_spec_int64 ("video_pts", "video PTS",
           "current video PTS value",
           G_MININT64, G_MAXINT64, 0, G_PARAM_READABLE));

#ifdef USE_GST1
  GST_DEBUG_CATEGORY_INIT (gst_westeros_sink_debug,
                           #ifdef USE_RAW_SINK
                           "westerosrawsink",
                           0,
                           "westerosrawsink element"
                           #else
                           "westerossink",
                           0,
                           "westerossink element"
                           #endif
                          );

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_westeros_sink_pad_template));
  gst_element_class_set_details_simple (gstelement_class, "Westeros Sink",
      #ifdef USE_RAW_SINK
      "Sink/Video",
      #else
      "Codec/Decoder/Video/Sink/Video",
      #endif
      "Writes buffers to the westeros wayland compositor",
      "Comcast");
#endif

   klass->canUseResMgr= 0;
   gst_westeros_sink_soc_class_init(klass);

   if ( resMgrCheckUse(klass) )
   {
      g_object_class_install_property (gobject_class, PROP_RES_PRIORITY,
        g_param_spec_uint ("res-priority",
                           "res-priority",
                           "Priority of resource usage, with 0 the highest priority",
                           0, G_MAXUINT32, 0, G_PARAM_READWRITE));

      g_object_class_install_property (gobject_class, PROP_RES_USAGE,
        g_param_spec_flags ("res-usage", "res-usage", "Flags to indicate intended usage",
          GST_TYPE_USAGE_FLAGS, DEFAULT_USAGE,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
   }
}

static void 
#ifdef USE_GST1
gst_westeros_sink_init(GstWesterosSink *sink)
{
#else
gst_westeros_sink_init(GstWesterosSink *sink, GstWesterosSinkClass *gclass) 
{
   WESTEROS_UNUSED(gclass);
#endif
   
   sink->peerPad= NULL;
   
   sink->parentEventFunc = GST_PAD_EVENTFUNC(GST_BASE_SINK_PAD(sink));
   sink->defaultQueryFunc = GST_PAD_QUERYFUNC(GST_BASE_SINK_PAD(sink));
   if ( sink->defaultQueryFunc == NULL )
   {
      sink->defaultQueryFunc= gst_pad_query_default;
   }

   gst_pad_set_event_function(GST_BASE_SINK_PAD(sink), GST_DEBUG_FUNCPTR(gst_westeros_sink_event));
   gst_pad_set_link_function(GST_BASE_SINK_PAD(sink), GST_DEBUG_FUNCPTR(gst_westeros_sink_link));
   gst_pad_set_unlink_function(GST_BASE_SINK_PAD(sink), GST_DEBUG_FUNCPTR(gst_westeros_sink_unlink));
   gst_pad_set_query_function(GST_BASE_SINK_PAD(sink), GST_DEBUG_FUNCPTR(gst_westeros_sink_sink_query));
    
   gst_base_sink_set_sync(GST_BASE_SINK(sink), FALSE);
   gst_base_sink_set_async_enabled(GST_BASE_SINK(sink), FALSE);

   sink->initialized= TRUE;
   
   #ifdef GLIB_VERSION_2_32 
   g_mutex_init( &sink->mutex );
   #else
   sink->mutex= g_mutex_new();
   #endif

   sink->videoStarted= FALSE;
   sink->startAfterLink= FALSE;
   sink->startAfterCaps= FALSE;
   sink->flushStarted= FALSE;
   sink->passCaps= FALSE;
   sink->rejectPrerollBuffers= FALSE;
   
   sink->srcWidth= 0;
   sink->srcHeight= 0;
   sink->maxWidth= 0;
   sink->maxHeight= 0;

   sink->windowX= DEFAULT_WINDOW_X;
   sink->windowY= DEFAULT_WINDOW_Y;
   sink->windowWidth= DEFAULT_WINDOW_WIDTH;
   sink->windowHeight= DEFAULT_WINDOW_HEIGHT;
   sink->show= true;
   sink->windowSet= false;
   sink->windowChange= false;
   sink->windowSizeOverride= false;

   sink->displayWidth= -1;
   sink->displayHeight= -1;
   
   sink->visible= false;
   
   sink->opacity= 1.0;
   sink->zorder= 0.0;
   sink->playbackRate= 1.0;

   sink->transX= 0;
   sink->transY= 0;
   sink->scaleXNum= 1;
   sink->scaleXDenom= 1;
   sink->scaleYNum= 1;
   sink->scaleYDenom= 1;
   sink->outputWidth= DEFAULT_WINDOW_WIDTH;
   sink->outputHeight= DEFAULT_WINDOW_HEIGHT;
   
   sink->eosEventSeen= FALSE;
   sink->eosDetected= FALSE;
   sink->startPTS= 0;
   sink->firstPTS= 0;
   sink->currentPTS= 0;
   sink->position= 0;
   sink->positionSegmentStart= 0;
   sink->prevPositionSegmentStart= 0xFFFFFFFFFFFFFFFFLL;
   sink->segmentNumber= 0;
   sink->queryPositionFromPeer= FALSE;
   sink->useSegmentPosition= FALSE;

   sink->display= 0;
   sink->currentSegment = NULL;

   sink->processPadEvent= 0;

   sink->rm= 0;
   sink->resPriority= 0;
   sink->resUsage= DEFAULT_USAGE;
   sink->resAssignedId= -1;
   memset( &sink->resReqPrimary, 0, sizeof(WstSinkResReqInfo) );
   memset( &sink->resReqSecondary, 0, sizeof(WstSinkResReqInfo) );
   sink->acquireResources= 0;
   sink->releaseResources= 0;
   #ifdef ENABLE_SW_DECODE
   sink->swCtx= 0;
   sink->swInit= 0;
   sink->swTerm= 0;
   sink->swLink= 0;
   sink->swUnLink= 0;
   sink->swEvent= 0;
   sink->swDisplay= 0;
   #endif
   sink->enableTimeCodeSignal= FALSE;
   sink->timeCodeCapacity= 0;
   sink->timeCodeCount= 0;
   memset( &sink->timeCodeActive, 0, sizeof(WstSinkTimeCode));
   sink->timeCodes= 0;
   sink->timeCodePresent= timeCodePresent;

   sink->mediaCaptureModule= 0;
   sink->mediaCaptureContext= 0;
   sink->mediaCaptureDestroyContext= 0;

   if ( gst_westeros_sink_soc_init( sink ) == TRUE )
   {
      sink->registry= 0;
      sink->shell= 0;
      sink->compositor= 0;
      sink->surfaceId= 0;
      sink->vpc= 0;
      sink->vpcSurface= 0;
      sink->output= 0;
   }
   else
   {
      GST_ERROR("gst_westeros_sink_init: soc_init failed");
   }
}

static void gst_westeros_sink_term(GstWesterosSink *sink)
{
   sink->initialized= FALSE;

   gst_westeros_sink_soc_term( sink );

   #ifdef GLIB_VERSION_2_32 
   g_mutex_clear( &sink->mutex );
   #else
   g_mutex_free( sink->mutex );
   #endif  
}

static void gst_westeros_sink_finalize(GObject *object) 
{
   GstWesterosSink *sink = GST_WESTEROS_SINK(object);

   if ( sink->initialized )
   {
      gst_westeros_sink_term( sink );
   }

   GST_CALL_PARENT (G_OBJECT_CLASS, finalize, (object));
}

static void gst_westeros_sink_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) 
{
   GstWesterosSink *sink = GST_WESTEROS_SINK(object);
  
   WESTEROS_UNUSED(pspec);
   WESTEROS_UNUSED(value);
   WESTEROS_UNUSED(sink);
    
   switch (prop_id) 
   {
      case PROP_WINDOW_SET:
      {
         const gchar *str= g_value_get_string(value);
         gchar **parts= g_strsplit(str, ",", 4);
         
         if ( !parts[0] || !parts[1] || !parts[2] || !parts[3] )
         {
            GST_ERROR( "Bad window properties string" );
         }
         else
         {
            int nx, ny, nw, nh;
            nx= atoi( parts[0] );
            ny= atoi( parts[1] );
            nw= atoi( parts[2] );
            nh= atoi( parts[3] );

            if ( (sink->windowSet == false) ||
                 (nx != sink->windowX) ||
                 (ny != sink->windowY) ||
                 (nw != sink->windowWidth) ||
                 (nh != sink->windowHeight) )
            {
               LOCK( sink );
               sink->windowChange= true;
               sink->windowSet= true;
               sink->windowX= nx;
               sink->windowY= ny;
               sink->windowWidth= nw;
               sink->windowHeight= nh;
               if ( (sink->windowWidth != DEFAULT_WINDOW_WIDTH) ||
                    (sink->windowHeight != DEFAULT_WINDOW_HEIGHT) )
               {
                  sink->windowSizeOverride= true;
               }
               UNLOCK( sink );

               printf("gst_westeros_sink_set_property set window rect (%d,%d,%d,%d)\n",
                       sink->windowX, sink->windowY, sink->windowWidth, sink->windowHeight );

               if ( sink->vpcSurface )
               {
                  if ( sink->vpcSurface )
                  {
                     wl_vpc_surface_set_geometry( sink->vpcSurface, sink->windowX, sink->windowY, sink->windowWidth, sink->windowHeight );
                  }
               }
               if ( sink->shell && sink->surfaceId )
               {
                  wl_simple_shell_set_geometry( sink->shell, sink->surfaceId,sink->windowX, sink->windowY,sink->windowWidth, sink->windowHeight );
                  if ( (sink->windowWidth > 0) && (sink->windowHeight > 0 ) && sink->show )
                  {
                     wl_simple_shell_set_visible( sink->shell, sink->surfaceId, true);

                     wl_simple_shell_get_status( sink->shell, sink->surfaceId);

                     wl_display_flush( sink->display );
                  }
               }
            }
         }

         g_strfreev(parts);
         break;
      }
      
      case PROP_ZORDER:
      {
         sink->zorder= g_value_get_float(value);
         if ( sink->shell )
         {
            wl_fixed_t z= wl_fixed_from_double(sink->zorder);
            wl_simple_shell_set_zorder( sink->shell, sink->surfaceId, z);
         }
         break;
      }
      
      case PROP_OPACITY:
      {
         sink->opacity= g_value_get_float(value);
         if ( sink->shell )
         {
            wl_fixed_t op= wl_fixed_from_double(sink->opacity);
            wl_simple_shell_set_opacity( sink->shell, sink->surfaceId, op);
         }
         break;
      }

      case PROP_ENABLE_TIMECODE:
      {
         sink->enableTimeCodeSignal= g_value_get_boolean(value);
         if ( !sink->enableTimeCodeSignal )
         {
            timeCodeFlush( sink );
         }
         break;
      }

      case PROP_RES_PRIORITY:
      {
         guint priority= g_value_get_uint(value);
         LOCK(sink);
         if ( priority != sink->resPriority )
         {
            sink->resPriority= g_value_get_uint(value);
            if ( sink->rm )
            {
               EssRMgrRequestSetPriority( sink->rm,
                                          EssRMgrResType_videoDecoder,
                                          sink->resReqPrimary.resReq.requestId,
                                          sink->resPriority );
            }
         }
         UNLOCK(sink);
         break;
      }

      case PROP_RES_USAGE:
      {
         guint usage= g_value_get_flags(value);
         LOCK(sink);
         if ( sink->resUsage != usage )
         {
            sink->resUsage= g_value_get_flags(value);
            if ( sink->rm )
            {
               EssRMgrUsage newUsage;
               newUsage.usage= sink->resUsage;
               newUsage.info= sink->resReqPrimary.resReq.info;

               EssRMgrRequestSetUsage( sink->rm,
                                       EssRMgrResType_videoDecoder,
                                       sink->resReqPrimary.resReq.requestId,
                                       &newUsage );
            }
         }
         UNLOCK(sink);
         break;
      }
      
      default:
         gst_westeros_sink_soc_set_property(object, prop_id, value, pspec);
         break;
   }
}

static void gst_westeros_sink_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) 
{
   GstWesterosSink *sink = GST_WESTEROS_SINK(object);
  
   WESTEROS_UNUSED(pspec); 
   WESTEROS_UNUSED(value);
   WESTEROS_UNUSED(sink);
    
   switch (prop_id) 
   {
      case PROP_VIDEO_WIDTH:
         {
            LOCK(sink);
            g_value_set_int(value, sink->srcWidth);
            UNLOCK(sink);
         }
         break;
      case PROP_VIDEO_HEIGHT:
         {
            LOCK(sink);
            g_value_set_int(value, sink->srcHeight);
            UNLOCK(sink);
         }
         break;
      case PROP_ENABLE_TIMECODE:
         {
            g_value_set_boolean(value, sink->enableTimeCodeSignal);
         }
         break;
      case PROP_VIDEO_PTS:
         {
            LOCK(sink);
            gint64 currentPTS= sink->currentPTS;
            UNLOCK(sink);
            g_value_set_int64(value, currentPTS);
         }
         break;
      case PROP_RES_PRIORITY:
         {
            LOCK(sink);
            g_value_set_uint(value, sink->resPriority);
            UNLOCK(sink);
         }
         break;
      case PROP_RES_USAGE:
         {
            LOCK(sink);
            g_value_set_flags(value, sink->resUsage);
            UNLOCK(sink);
         }
         break;
      default:
         gst_westeros_sink_soc_get_property(object, prop_id, value, pspec);
         break;
   }
}

static GstStateChangeReturn gst_westeros_sink_change_state(GstElement *element, GstStateChange transition)
{
   GstStateChangeReturn result= GST_STATE_CHANGE_SUCCESS;
   GstWesterosSink *sink= GST_WESTEROS_SINK(element);
   gboolean passToDefault= true;

   GST_DEBUG_OBJECT(element, "westeros-sink: change state from %s to %s",
      gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT (transition)),
      gst_element_state_get_name (GST_STATE_TRANSITION_NEXT (transition)));

   sink->rejectPrerollBuffers= false;

   if (GST_STATE_TRANSITION_CURRENT(transition) == GST_STATE_TRANSITION_NEXT(transition))
   {
      return GST_STATE_CHANGE_SUCCESS;
   }

   switch (transition)
   {
      case GST_STATE_CHANGE_NULL_TO_READY:
      {
         printf("westeros (sink) version " WESTEROS_VERSION_FMT "\n", WESTEROS_VERSION );

         resMgrInit(sink);
         resMgrRequestDecoder(sink);

         sink->position= 0;         
         sink->eosDetected= FALSE;
         sink->eosEventSeen= FALSE;
         if ( !gst_westeros_sink_backend_null_to_ready(sink, &passToDefault) )
         {
            result= GST_STATE_CHANGE_FAILURE;
            break;
         }
         break;
      }

      case GST_STATE_CHANGE_READY_TO_PAUSED:
      {
         captureInit(sink);

         sink->eosEventSeen= FALSE;
         if ( gst_westeros_sink_backend_ready_to_paused(sink, &passToDefault) )
         {
            sink->rejectPrerollBuffers = !gst_base_sink_is_async_enabled(GST_BASE_SINK(sink));

            if ( !sink->display )
            {
               sink->display= wl_display_connect(NULL);
            }
            if ( sink->display )
            {
               sink->queue= wl_display_create_queue(sink->display);
               if ( sink->queue )
               {
                  sink->registry= wl_display_get_registry( sink->display );
                  if ( sink->registry )
                  {
                     wl_proxy_set_queue((struct wl_proxy*)sink->registry, sink->queue);
                     wl_registry_add_listener(sink->registry, &registryListener, sink);
                     wl_display_roundtrip_queue(sink->display,sink->queue);

                     sink->surface= wl_compositor_create_surface(sink->compositor);
                     printf("westeros-sink: ready-to-paused: surface=%p\n", (void*)sink->surface);
                     wl_proxy_set_queue((struct wl_proxy*)sink->surface, sink->queue);
                     wl_display_flush( sink->display );
                  }
                  else
                  {
                     GST_ERROR("westeros-sink: ready-to-paused: unable to get display registry\n");
                  }
               }
               else
               {
                  GST_ERROR("westeros-sink: ready-to-paused: unable to create queue\n");
               }
            }
            else
            {
               GST_ERROR("westeros-sink: ready-to-paused: unable to create display\n");
            }

            if ( sink->vpc && sink->surface )
            {
               sink->vpcSurface= wl_vpc_get_vpc_surface( sink->vpc, sink->surface );
               if ( sink->vpcSurface )
               {
                  wl_vpc_surface_add_listener( sink->vpcSurface, &vpcListener, sink );
                  wl_proxy_set_queue((struct wl_proxy*)sink->vpcSurface, sink->queue);
                  wl_vpc_surface_set_geometry( sink->vpcSurface, sink->windowX, sink->windowY, sink->windowWidth, sink->windowHeight );
                  wl_display_flush( sink->display );
                  printf("westeros-sink: ready-to-paused: done add vpcSurface listener\n");
               }
               else
               {
                  GST_ERROR("westeros-sink: ready-to-paused: failed to create vpcSurface\n");
               }
            }
            else
            {
               GST_ERROR("westeros-sink: ready-to-paused: can't create vpc surface: vpc %p surface %p\n",
                         sink->vpc, sink->surface);
            }
         }
         else
         {
            result= GST_STATE_CHANGE_FAILURE;
         }
         break;
      }

      case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      {
         if ( !gst_westeros_sink_backend_paused_to_playing( sink, &passToDefault) )
         {
            result= GST_STATE_CHANGE_FAILURE;
         }
         break;
      }

      default:
         break;
   }

   if ( gst_base_sink_get_sync(GST_BASE_SINK(sink)) == TRUE )
   {
      if (result == GST_STATE_CHANGE_FAILURE)
      {
         return result;
      }

      if ( passToDefault )
      {
         result= GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);
      }

      if (result == GST_STATE_CHANGE_FAILURE)
      {
         return result;
      }
   }

   switch (transition)
   {
      case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      {
         if ( gst_westeros_sink_backend_playing_to_paused( sink, &passToDefault ) )
         {
            sink->rejectPrerollBuffers = !gst_base_sink_is_async_enabled(GST_BASE_SINK(sink));
         }
         break;
      }

      case GST_STATE_CHANGE_PAUSED_TO_READY:
      {
         sink->eosEventSeen= FALSE;
         sink->eosDetected= FALSE;
         if ( gst_westeros_sink_backend_paused_to_ready( sink, &passToDefault ) )
         {
            sink->rejectPrerollBuffers = !gst_base_sink_is_async_enabled(GST_BASE_SINK(sink));
         }

         releaseWaylandResources( sink );

         timeCodeFlush( sink );

         captureTerm(sink);
         break;
      }

      case GST_STATE_CHANGE_READY_TO_NULL:
      {
         if ( sink->initialized )
         {
            if ( !gst_westeros_sink_backend_ready_to_null( sink, &passToDefault ) )
            {
               result= GST_STATE_CHANGE_FAILURE;
            }

            resMgrReleaseDecoder(sink);
            resMgrTerm(sink);
         }
         releaseWaylandResources( sink );
         break;
      }

      default:
         break;
   }
  
   if (result == GST_STATE_CHANGE_FAILURE)
   {
      return result;
   }

   if ( gst_base_sink_get_sync(GST_BASE_SINK(sink)) == FALSE )
   {
      if ( passToDefault )
      {
         result= GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);
      }
   }
 
   return result;
}

static gboolean gst_westeros_sink_query(GstElement *element, GstQuery *query)
{
   GstWesterosSink *sink= GST_WESTEROS_SINK(element);

   switch (GST_QUERY_TYPE(query)) 
   {
      case GST_QUERY_LATENCY:
         gst_query_set_latency(query, FALSE, 0, 10*1000*1000);
         return TRUE;
   
      case GST_QUERY_POSITION:
         {
            GstFormat format;
            
            gst_query_parse_position(query, &format, NULL);
            
            if ( GST_FORMAT_BYTES == format )
            {
               return GST_ELEMENT_CLASS(parent_class)->query(element, query);
            }
            else
            {
               if (sink->queryPositionFromPeer && sink->peerPad)
               {
                   if (gst_pad_query(sink->peerPad, query))
                   {
                       GST_DEBUG_OBJECT(sink, "Queried position from peer");
                       return TRUE;
                   }
               }
               LOCK( sink );
               gint64 position= sink->position;
               UNLOCK( sink );
               GST_LOG_OBJECT(sink, "POSITION: %" GST_TIME_FORMAT, GST_TIME_ARGS (position));
               gst_query_set_position(query, GST_FORMAT_TIME, position);
               return TRUE;
            }
         }
         break;
         
      case GST_QUERY_CUSTOM:
      case GST_QUERY_DURATION:
      case GST_QUERY_SEEKING:
      case GST_QUERY_RATE:
         if (sink->peerPad)
         {
            return gst_pad_query(sink->peerPad, query);
         }
              
      default:
         return GST_ELEMENT_CLASS(parent_class)->query (element, query);
   }
}

static gboolean gst_westeros_sink_start(GstBaseSink *base_sink)
{
   WESTEROS_UNUSED(base_sink);

   return TRUE;
}

static gboolean gst_westeros_sink_stop(GstBaseSink *base_sink)
{
   WESTEROS_UNUSED(base_sink);

   return TRUE;
}

static gboolean gst_westeros_sink_unlock(GstBaseSink *base_sink)
{
   WESTEROS_UNUSED(base_sink);
  
   return TRUE;
}

static gboolean gst_westeros_sink_unlock_stop(GstBaseSink *base_sink)
{
   WESTEROS_UNUSED(base_sink);

   return TRUE;
}

#ifdef USE_GST1
static gboolean gst_westeros_sink_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
   GstWesterosSink *sink= GST_WESTEROS_SINK(parent);
#else
static gboolean gst_westeros_sink_event(GstPad *pad, GstEvent *event)
{
   GstWesterosSink *sink= GST_WESTEROS_SINK(gst_pad_get_parent(pad));
#endif
   gboolean result= TRUE;
   gboolean passToDefault= FALSE;

   if ( sink->processPadEvent )
   {
      if ( sink->processPadEvent( sink, pad, event, &passToDefault ) )
      {
         goto done;
      }
   }

   GST_DEBUG_OBJECT (sink, "received event %p %" GST_PTR_FORMAT, event, event);

   switch (GST_EVENT_TYPE(event))
   {
      case GST_EVENT_CAPS:
         {
            GstCaps *caps;
            gst_event_parse_caps(event, &caps);
            if (sink->maxWidth && sink->maxHeight)
            {
               GstStructure* structure = gst_caps_get_structure(caps, 0);
               if (structure && (gst_structure_has_field(structure, "width") || gst_structure_has_field(structure, "height")))
               {
                  gint width, height;
                  gst_structure_get_int(structure, "width", &width);
                  gst_structure_get_int(structure, "height", &height);
                  if (width > sink->maxWidth || height > sink->maxHeight)
                  {
                     GST_ERROR("width=%d height=%d > maxWidth=%d maxHeight=%d", width, height, sink->maxWidth, sink->maxHeight);
                     const char *err_string = "Maximum video dimensions exceeded";
                     GError *error = g_error_new(GST_STREAM_ERROR, GST_STREAM_ERROR_WRONG_TYPE, "%s", err_string);
                     GstMessage *message = gst_message_new_error(GST_OBJECT_CAST(sink), error, err_string);
                     gst_element_post_message(GST_ELEMENT_CAST(sink), message);
                     g_error_free(error);
                  }
               }
            }
            #ifdef ENABLE_SW_DECODE
            if ( sink->rm && (sink->resCurrCaps.capabilities & EssRMgrVidCap_software) )
            {
               wstsw_process_caps( sink, caps );
            }
            else
            #endif
            if ( sink->passCaps || (!sink->videoStarted && sink->startAfterCaps) )
            {
               gst_westeros_sink_soc_accept_caps( sink, caps );
            }
         }
         break;
      case GST_EVENT_FLUSH_START:
         LOCK( sink );
         sink->eosEventSeen= FALSE;
         sink->flushStarted= TRUE;
         UNLOCK( sink );
         timeCodeFlush( sink );
         gst_westeros_sink_soc_flush( sink );
         passToDefault= TRUE;
         break;

      case GST_EVENT_FLUSH_STOP:
         {
            #ifdef ENABLE_SW_DECODE
            gboolean reset_time= FALSE;
            gst_event_parse_flush_stop( event, &reset_time );

            if ( sink->rm && (sink->resCurrCaps.capabilities & EssRMgrVidCap_software) )
            {
               if ( reset_time && sink->flushStarted == TRUE )
               {
                  wstsw_reset_time( sink );
               }
            }
            #endif

            LOCK( sink );
            sink->flushStarted= FALSE;
            UNLOCK( sink );

            passToDefault= TRUE;
         }
         break;

      case GST_EVENT_EOS:
         {
            LOCK( sink );
            gboolean eosDetected= sink->eosDetected;
            sink->eosEventSeen= TRUE;
            UNLOCK( sink );
            if ( eosDetected )
            {
               passToDefault= TRUE;
            }
            else
            {
               gst_westeros_sink_soc_eos_event( sink );
            }
         }
         break;
         
      #ifdef USE_GST1
      case GST_EVENT_SEGMENT:
      #else
      case GST_EVENT_NEWSEGMENT:
      #endif
         {
            gint64 segmentStart, segmentPosition;
            GstFormat segmentFormat;
            gdouble appliedRate = 1.0;
            gdouble playbackRate= 1.0;
            gboolean playbackRateChanged= FALSE;

            #ifdef USE_GST1
            const GstSegment *dataSegment;
            gst_event_parse_segment(event, &dataSegment);
            segmentFormat= dataSegment->format;
            segmentStart= dataSegment->start;
            segmentPosition= dataSegment->position;
            appliedRate= dataSegment->applied_rate;
            playbackRate= dataSegment->rate;
            #else
            gst_event_parse_new_segment(event, NULL, NULL, 
                                        &segmentFormat, &segmentStart, 
                                        NULL, &segmentPosition);
            #endif
            gst_event_copy_segment( event, &sink->segment );
            
            GST_LOG_OBJECT(sink, 
                           "segment: start %" GST_TIME_FORMAT ", position %" GST_TIME_FORMAT,
                            GST_TIME_ARGS(segmentStart), GST_TIME_ARGS(segmentPosition));

            
            LOCK( sink );
            playbackRateChanged= sink->playbackRate != playbackRate;
            sink->currentSegment = dataSegment;
            sink->flushStarted= FALSE;
            sink->playbackRate= playbackRate;
            sink->position= 0;
            sink->currentPTS= 0;
            sink->positionSegmentStart= 0;
            sink->prevPositionSegmentStart= 0xFFFFFFFFFFFFFFFFLL;

            if ( sink->useSegmentPosition &&
                 (segmentFormat == GST_FORMAT_TIME) )
            {
               GST_DEBUG("using segment position: start %lld position %lld", segmentStart, segmentPosition);
               sink->position= GST_TIME_AS_NSECONDS(segmentPosition);
               sink->positionSegmentStart= GST_TIME_AS_NSECONDS(segmentPosition);
            }

            if (appliedRate != 1.0)
            {
                GST_DEBUG_OBJECT(sink, "rate change done upstream");
                sink->queryPositionFromPeer= TRUE;
            }
            
            if ( 
                 (segmentFormat == GST_FORMAT_TIME) && 
                 ( (segmentStart != 0) || (sink->startPTS != 0) || playbackRateChanged )
               ) 
            {
               sink->segmentNumber++;
               sink->eosEventSeen= FALSE;
               sink->eosDetected= FALSE;
               sink->position= GST_TIME_AS_NSECONDS(segmentStart);
               sink->positionSegmentStart= GST_TIME_AS_NSECONDS(segmentStart);
               sink->startPTS= (GST_TIME_AS_MSECONDS(segmentStart)*90LL);
               if ( sink->useSegmentPosition &&
                    (segmentStart != segmentPosition) &&
                    (segmentPosition != -1LL) )
               {
                  sink->position= GST_TIME_AS_NSECONDS(segmentPosition);
                  sink->positionSegmentStart= GST_TIME_AS_NSECONDS(segmentPosition);
               }
               gst_westeros_sink_soc_set_startPTS( sink, sink->startPTS );
            }
            UNLOCK( sink );

            passToDefault= TRUE;
         }
         break;
       default:
         passToDefault= TRUE;
         break;
   }

done:
   if (passToDefault && sink->parentEventFunc)
   {
      #ifdef USE_GST1
      result= sink->parentEventFunc(pad, parent, event);
      #else
      result= sink->parentEventFunc(pad, event);
      #endif
   }
   else
   {
      gst_event_unref(event);
   }

   #ifndef USE_GST1
   gst_object_unref(sink);
   #endif
  
   return result;
}

#ifdef USE_GST1
static gboolean gst_westeros_sink_sink_query(GstPad *pad, GstObject *parent, GstQuery *query)
{
   GstWesterosSink *sink= GST_WESTEROS_SINK(parent);
#else
static gboolean gst_westeros_sink_sink_query(GstPad *pad, GstQuery *query)
{
   GstWesterosSink *sink= GST_WESTEROS_SINK(gst_pad_get_parent(pad));
#endif

   gboolean rv = FALSE;

   rv = gst_westeros_sink_soc_query(sink, query);

   if (rv == FALSE)
   {
      #ifdef USE_GST1
      rv= sink->defaultQueryFunc(pad, parent, query);
      #else
      rv= sink->defaultQueryFunc(pad, query);
      #endif
   }

   return rv;
}

static gboolean gst_westeros_sink_check_caps(GstWesterosSink *sink, GstPad *peer)
{
   WESTEROS_UNUSED(sink);

   gboolean result= TRUE;
   GstCaps* caps= NULL;

#ifdef USE_GST1
   caps= gst_pad_query_caps(peer, NULL);
#else
   caps= gst_pad_get_caps(peer);
#endif
  
   if (gst_caps_get_size(caps) == 0)
   {
      result= TRUE;
      goto exit;
   }

   if ( !gst_westeros_sink_soc_accept_caps( sink, caps ) )  
   {
      result= FALSE;
      goto exit;
   }

exit:
   if ( caps )
   {
      gst_caps_unref(caps);
   }

   return result;
}

#ifdef USE_GST1
GstPadLinkReturn gst_westeros_sink_link(GstPad *pad, GstObject *parent, GstPad *peer)
{
   GstWesterosSink *sink= GST_WESTEROS_SINK(parent);
#else
static GstPadLinkReturn gst_westeros_sink_link(GstPad *pad, GstPad *peer)
{
   GstWesterosSink *sink= GST_WESTEROS_SINK(gst_pad_get_parent(pad));
#endif

   GST_DEBUG_OBJECT(sink, "gst_westeros_sink_link: enter");

   if (gst_westeros_sink_check_caps(sink, peer) != TRUE)
   {
      GST_ERROR("Peer Caps is not supported");
   }

   sink->peerPad= peer;
   
   GST_DEBUG_OBJECT(sink, "gst_westeros_sink_link: startAfterLink %d", sink->startAfterLink);
   if ( sink->startAfterLink )
   {
      sink->startAfterLink= FALSE;
      if ( !gst_westeros_sink_soc_start_video( sink ) )
      {
         GST_ERROR("gst_westeros_sink_link: gst_westeros_sink_sock_start_video failed");
      }
   }

   return GST_PAD_LINK_OK;
}

#ifdef USE_GST1
static void gst_westeros_sink_unlink(GstPad *pad, GstObject *parent)
{
   WESTEROS_UNUSED(pad);
   GstWesterosSink *sink= GST_WESTEROS_SINK(parent);
#else
static void gst_westeros_sink_unlink(GstPad *pad)
{
   GstWesterosSink *sink= GST_WESTEROS_SINK(gst_pad_get_parent(pad));
#endif

   GST_DEBUG_OBJECT(sink, "gst_westeros_sink_unlink");
   sink->peerPad= NULL;
   
   return;
}

static GstFlowReturn gst_westeros_sink_render(GstBaseSink *base_sink, GstBuffer *buffer)
{  
   GstWesterosSink *sink= GST_WESTEROS_SINK(base_sink);
   
   LOCK( sink );
   sink->eosDetected= FALSE;
   UNLOCK( sink );

   #ifdef USE_GST_VIDEO
   if ( GST_BUFFER_PTS_IS_VALID(buffer) && sink->enableTimeCodeSignal )
   {
      guint64 pts= GST_BUFFER_PTS(buffer);
      GstVideoTimeCodeMeta *tcm= gst_buffer_get_video_time_code_meta(buffer);
      if ( tcm )
      {
         guint hours= tcm->tc.hours;
         guint minutes= tcm->tc.minutes;
         guint seconds= tcm->tc.seconds;
         timeCodeAdd( sink, pts, hours, minutes, seconds );
      }
   }
   #endif

   gst_westeros_sink_soc_render( sink, buffer );

   return GST_FLOW_OK;
}

static GstFlowReturn gst_westeros_sink_preroll(GstBaseSink *base_sink, GstBuffer *buffer)
{
   GstWesterosSink *sink= GST_WESTEROS_SINK(base_sink);

   WESTEROS_UNUSED(buffer);
   
   GST_DEBUG_OBJECT(sink, "gst_westeros_sink_preroll: enter: rejectPrerollBuffers: %d", sink->rejectPrerollBuffers);
   if (sink->rejectPrerollBuffers)
   {
      #ifdef USE_GST1
      return GST_FLOW_FLUSHING;
      #else
      return GST_FLOW_WRONG_STATE;
      #endif
   }

   return GST_FLOW_OK;
}

void gst_westeros_sink_eos_detected( GstWesterosSink *sink )
{
   LOCK( sink );
   gboolean eosEventSeen= sink->eosEventSeen;
   sink->eosDetected= TRUE;
   UNLOCK( sink );
   if (eosEventSeen)
   {
      GST_DEBUG_OBJECT(sink, "gst_westeros_sink_eos_detected: posting EOS");
      if (sink->parentEventFunc)
      {
         #ifdef USE_GST1
         sink->parentEventFunc(GST_BASE_SINK_PAD(sink), GST_OBJECT_CAST(sink), gst_event_new_eos());
         #else
         sink->parentEventFunc(GST_BASE_SINK_PAD(sink), gst_event_new_eos());
         #endif
      }
      else
      {
         GST_WARNING("gst_westeros_sink_eos_detected: no parentEventFunc: posting eos msg");
         gst_element_post_message (GST_ELEMENT_CAST(sink), gst_message_new_eos(GST_OBJECT_CAST(sink)));
      }
      LOCK( sink );
      sink->eosEventSeen= FALSE;
      UNLOCK( sink );
   }
}

static gboolean westeros_sink_init (GstPlugin * plugin)
{
   return gst_element_register (plugin,
                                #ifdef USE_RAW_SINK
                                "westerosrawsink",
                                #else
                                "westerossink",
                                #endif
                                GST_RANK_PRIMARY,
                                gst_westeros_sink_get_type ());
}

#ifndef PACKAGE
#define PACKAGE "mywesterossink"
#endif

#ifdef USE_GST1
GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    #ifdef USE_RAW_SINK
    westerosrawsink,
    #else
    westerossink,
    #endif
    "Writes buffers to the westeros wayland compositor",
    westeros_sink_init, 
    VERSION, 
    "LGPL", 
    PACKAGE_NAME,
    GST_PACKAGE_ORIGIN )
#else
GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    #ifdef USE_RAW_SINK
    "westerosrawsink",
    #else
    "westerossink",
    #endif
    "Writes buffers to the westeros wayland compositor",
    westeros_sink_init, 
    VERSION, 
    "LGPL", 
    PACKAGE_NAME,
    GST_PACKAGE_ORIGIN )
#endif


