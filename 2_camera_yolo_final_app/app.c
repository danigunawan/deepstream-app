# include <gst/gst.h>
# include <glib.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include "includes/gstnvdsmeta.h"

static GMainLoop *loop;

#define MAX_DISPLAY_LEN 64
#define PGIE_CLASS_ID_VEHICLE 2
#define PGIE_CLASS_ID_PERSON 0
#define MUXER_OUTPUT_WIDTH 640
#define MUXER_OUTPUT_HEIGHT 480
#define MUXER_BATCH_TIMOUT 4000000
#define MODEL_CONFIG_FILE "config_model.txt"





gint frame_number = 0;
gchar pgie_classes_str[4][32] = { "Vehicle", "TwoWheeler", "Person",
  "Roadsign"
};

/*
Function to deal with batch meta and to draw the results in the image.
*/
static GstPadProbeReturn
osd_sink_pad_buffer_probe (GstPad * pad, GstPadProbeInfo * info,
    gpointer u_data)
{
    GstBuffer *buf = (GstBuffer *) info->data;
    guint num_rects = 0; 
    NvDsObjectMeta *obj_meta = NULL;
    guint vehicle_count = 0;
    guint person_count = 0;
    NvDsMetaList * l_frame = NULL;
    NvDsMetaList * l_obj = NULL;
    NvDsDisplayMeta *display_meta = NULL;

    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (buf);

    for (l_frame = batch_meta->frame_meta_list; l_frame != NULL;
      l_frame = l_frame->next) {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *) (l_frame->data);
        int offset = 0;
        for (l_obj = frame_meta->obj_meta_list; l_obj != NULL;
                l_obj = l_obj->next) {
            obj_meta = (NvDsObjectMeta *) (l_obj->data);
            if (obj_meta->class_id == PGIE_CLASS_ID_VEHICLE) {
                vehicle_count++;
                num_rects++;
            }
            if (obj_meta->class_id == PGIE_CLASS_ID_PERSON) {
                person_count++;
                num_rects++;
            }
        }
        display_meta = nvds_acquire_display_meta_from_pool(batch_meta);
        NvOSD_TextParams *txt_params  = &display_meta->text_params[0];
        display_meta->num_labels = 1;
        txt_params->display_text = g_malloc0 (MAX_DISPLAY_LEN);
        offset = snprintf(txt_params->display_text, MAX_DISPLAY_LEN, "Person = %d ", person_count);
        offset = snprintf(txt_params->display_text + offset , MAX_DISPLAY_LEN, "Vehicle = %d ", vehicle_count);

        /* Now set the offsets where the string should appear */
        txt_params->x_offset = 10;
        txt_params->y_offset = 12;

        /* Font , font-color and font-size */
        txt_params->font_params.font_name = "Courier";
        txt_params->font_params.font_size = 24;
        txt_params->font_params.font_color.red = 1.0;
        txt_params->font_params.font_color.green = 1.0;
        txt_params->font_params.font_color.blue = 1.0;
        txt_params->font_params.font_color.alpha = 1.0;

        /* Text background color */
        txt_params->set_bg_clr = 1;
        txt_params->text_bg_clr.red = 0.0;
        txt_params->text_bg_clr.green = 0.0;
        txt_params->text_bg_clr.blue = 0.0;
        txt_params->text_bg_clr.alpha = 1.0;

        nvds_add_display_meta_to_frame(frame_meta, display_meta);
    }
    frame_number++;
    return GST_PAD_PROBE_OK;
}


/*
Bus messages function
*/

static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer data){
    switch (GST_MESSAGE_TYPE(msg)){
        case GST_MESSAGE_INFO:{
            GError *error = NULL;
            gchar *debug_info = NULL;
            gst_message_parse_info(msg, &error, &debug_info);
            g_printerr("INFO from %s, %s.\n", GST_OBJECT_NAME(msg->src), error->message);
            if (debug_info){
                g_printerr("Debuf info: %s.\n.",debug_info);
            }
            g_error_free(error);
            g_free(debug_info);
            break;
        }
        case GST_MESSAGE_WARNING:{
            GError *error = NULL;
            gchar *debug_info = NULL;
            gst_message_parse_warning(msg, &error, &debug_info);
            g_printerr("WARNING from %s: %s.\n", GST_OBJECT_NAME(msg->src), error->message);

            if (debug_info){
                g_printerr("Debuf info: %s.\n.",debug_info);
            }
            g_error_free(error);
            g_free(debug_info);
            break; 
        }
        case GST_MESSAGE_ERROR:{
            GError *err;
            gchar *debug_info;
            gst_message_parse_error(msg, &err, &debug_info);
            g_print("Error: %s\n", err->message);
            g_error_free(err);
            g_free(debug_info);
            g_main_loop_quit(loop);
            break;
        }
        case GST_MESSAGE_EOS:{
            g_print("End of stream \n");
            g_main_loop_quit(loop);
            break;
        }
        default:
            break;
    }
    return TRUE;
}

gint main(gint argc, gchar *argv[]){

    GstElement *pipeline = NULL, 
               *source = NULL, 
               *vidconv_src = NULL, 
               *nvvidconv_src = NULL, 
               *filter_src = NULL,
               *streammux = NULL,
               *pgie = NULL,
               *nvvidconv = NULL, 
               *nvosd = NULL,
               *transform = NULL,
               *cap_filter = NULL, 
               *sink = NULL;
    GstCaps *caps_filter_src = NULL, 
            *caps = NULL;
    GstBus *bus = NULL;
    GstPad *osd_sink_pad = NULL, 
           *sinkpad = NULL,
           *srcpad = NULL;
    guint bus_watch_id;


    gst_init(&argc, &argv);

    /* Creating and setting the elements */
    pipeline = gst_pipeline_new("camera-player");
    source = gst_element_factory_make("v4l2src", "camera-source");
    g_object_set (G_OBJECT (source), "device", "/dev/video0", NULL);
    vidconv_src = gst_element_factory_make("videoconvert", "vidconv-src");
    nvvidconv_src = gst_element_factory_make("nvvideoconvert","nvvidconv-src");
    g_object_set(G_OBJECT(nvvidconv_src), "nvbuf-memory-type", 0, NULL);
    filter_src = gst_element_factory_make("capsfilter", "filter-src");
    caps_filter_src = gst_caps_from_string("video/x-raw(memory:NVMM), format=NV12, width = 640, height = 480, framerate=30/1");
    if (!caps_filter_src){
        g_printerr("The initial caps filter could not be created!\n");
        return -1;
    }
    g_object_set(G_OBJECT(filter_src), "caps", caps_filter_src, NULL);
    gst_caps_unref(caps_filter_src);
    streammux = gst_element_factory_make("nvstreammux", "stream-muxer");
    g_object_set(G_OBJECT(streammux), "width", MUXER_OUTPUT_WIDTH, "height", MUXER_OUTPUT_HEIGHT, "batch-size", 1, "batched-push-timeout", MUXER_BATCH_TIMOUT, NULL);
    pgie = gst_element_factory_make("nvinfer", "detector");
    g_object_set(G_OBJECT(pgie), "config-file-path", MODEL_CONFIG_FILE, NULL);
    nvvidconv = gst_element_factory_make("nvvideoconvert", "video-coverter"); /*from NV12 to RGBA as required by nvosd*/
    nvosd = gst_element_factory_make("nvdsosd", "nv-onscreendisplay"); /*create a OSD to draw on the converted RGBA buffer*/
    transform = gst_element_factory_make("nvvideoconvert", "transform");
    cap_filter = gst_element_factory_make("capsfilter", "filter");
    if (!cap_filter){
        g_printerr("The final cap filter could not be created!\n");
        return -1;
    }
    caps = gst_caps_from_string("video/x-raw(memory:NVMM), format:I420");
    g_object_set(G_OBJECT(cap_filter), "caps", caps, NULL);
    sink = gst_element_factory_make("nveglglessink", "display");
    g_object_set(G_OBJECT(sink), "sync", 0, NULL); /*Important to have real time*/

    if (!pipeline || !source || !streammux || !pgie || !nvvidconv || !nvosd || !transform ||!sink){
        g_printerr("One of the elements could not be created!\n");
        return -1;
    }

    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    bus_watch_id = gst_bus_add_watch(bus, bus_callback, NULL);
    gst_object_unref(bus);

    /* Adding the elements to the pipeline */
    gst_bin_add_many(GST_BIN(pipeline), source, vidconv_src, nvvidconv_src, filter_src, streammux, pgie, nvvidconv, nvosd, transform, cap_filter, sink, NULL);


    gchar pad_name_sink[16] = "sink_0";
    gchar pad_name_src [16] = "src"; 
    sinkpad = gst_element_get_request_pad(streammux, pad_name_sink);
    srcpad = gst_element_get_static_pad(filter_src, pad_name_src);
    if (!sinkpad){
        g_printerr("Streammux request sink pad failed. Exiting...\n");
        return -1;
    } 
    if (!srcpad){
        g_printerr("Decoder request src pad failed!Exiting...\n");
        return -1;
    }
    if (gst_pad_link(srcpad,sinkpad)!=GST_PAD_LINK_OK){
        g_printerr("Failed to link decoder to stream muxer. Exiting...\n");
        return -1;
    }
    gst_object_unref(sinkpad);
    gst_object_unref(srcpad);
    
    /* Linking the elements together */
    if (!gst_element_link_many(source, vidconv_src, nvvidconv_src, filter_src, NULL)){
        g_printerr("Failed to link firsts elements. Exiting...\n");
        return -1;
    }
    if (!gst_element_link_many(streammux, pgie, nvvidconv, nvosd, transform, cap_filter, sink, NULL)){
        g_printerr("Failed to link seconds elements. Exiting...\n");
        return -1;
    }

    osd_sink_pad = gst_element_get_static_pad(nvosd, "sink");
    if (!osd_sink_pad)
        g_printerr("Unable to sink. Exiting...\n");
    else
        gst_pad_add_probe(osd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER, osd_sink_pad_buffer_probe, NULL, NULL);


    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    loop = g_main_loop_new(NULL, FALSE);
    g_print("Playing application...\n");
    g_main_loop_run(loop);


    /* Cleaning up */
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(pipeline));
    g_source_remove(bus_watch_id);
    g_main_loop_unref(loop);
    gst_print("Exiting...\n");
    return 0;
}