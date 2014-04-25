#include <iostream>
#include <gst/gst.h>
#include <glib.h>
#include <string.h>
#include <stdio.h>
// TO COMPILE
//
// libtool --mode=link g++ poc-streamrecorder.c -o poc-streamrecorder `pkg-config --cflags --libs  gstreamer-1.0`

typedef struct
{
    GMainLoop * loop;
    GstElement *pipeline;
    GstElement * rtspsrc;
    GstElement * p_rtph264depay;
    GstElement * p_h264parse;
    GstElement * p_matroskamux;
    GstElement * p_matroskamux_next;
    GstElement * p_filesink;
    GstElement * p_filesink_next;
    GstElement * p_queue;
    gulong      idProbe;
    gulong      indexFile;
    char*      filePrefix;
    gulong     idMux;
} CustomData;

static gboolean my_bus_callback (GstBus     *bus,
		 GstMessage *message,
		 gpointer    p_data)
{
  //g_print ("Got %s message from %s \n", GST_MESSAGE_TYPE_NAME (message), GST_MESSAGE_SRC_NAME(message));
  CustomData* data = (CustomData*) p_data;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR: {
      GError *err;
      gchar *debug;

      gst_message_parse_error (message, &err, &debug);
      g_print ("Error: %s\n", err->message);
      g_error_free (err);
      g_free (debug);
      g_main_loop_quit (data->loop);
      break;
    }
    case GST_MESSAGE_EOS:
      /* end-of-stream */
      if (data->idMux == 0) {
          gst_element_set_state (data->p_matroskamux, GST_STATE_NULL);
          gst_bin_remove(GST_BIN(data->pipeline), data->p_matroskamux);
          g_print("remove p_matroskamux \n");
          data->p_matroskamux = NULL;
          gst_element_set_state (data->p_filesink, GST_STATE_NULL);
          gst_bin_remove(GST_BIN(data->pipeline), data->p_filesink);
          g_print("remove p_filesink \n");
          data->p_filesink = NULL;
          data->idMux = 1;
      }
      else{
          gst_element_set_state (data->p_matroskamux_next, GST_STATE_NULL);
          gst_bin_remove(GST_BIN(data->pipeline), data->p_matroskamux_next);
          g_print("remove p_matroskamux_next \n");
          data->p_matroskamux_next = NULL;
          gst_element_set_state (data->p_filesink_next, GST_STATE_NULL);
          gst_bin_remove(GST_BIN(data->pipeline), data->p_filesink_next);
          g_print("remove p_filesink_next \n");
          data->p_filesink_next= NULL;
          data->idMux = 0;
      }

      break;
    default:
      /* unhandled message */
      break;
  }
}

/* called when a source pad of queue is blocked */
static GstPadProbeReturn
cb_blocked (GstPad *pad,GstPadProbeInfo *info, gpointer p_data)
{
    GstPad * queuesrcpad, *muxsinkpad, *filesinkpad, *muxnextsinkpad;
    gchar* filename;
    GstBuffer* buffer;
    buffer = GST_PAD_PROBE_INFO_BUFFER (info);

    if (  GST_BUFFER_FLAG_IS_SET ( buffer, GST_BUFFER_FLAG_DELTA_UNIT)){
        // not a keyframe
        //g_print("Not a Keyframe ! Pass it \n");
        return GST_PAD_PROBE_PASS;
    }


    /*
     * Pipeline modification
     *
        [rtspsrc]-->[rtph264depay]-->[h264parse]-->[queue]|-{unlink}>[matroskamux]-->[filesink]
                                                          |
                                                          -->[matroskamux_next]-->[filesink_next]
                 --> TODO: audio
     * */
    CustomData* data = (CustomData*) p_data;

    if (data->idMux == 0) {

        // 2a) unlink : [queue]|-->[matroskamux]
        queuesrcpad = gst_element_get_static_pad(data->p_queue,"src");
        muxsinkpad = gst_element_get_static_pad(data->p_matroskamux,"video_0");
        if(FALSE == gst_pad_unlink (queuesrcpad, muxsinkpad))
        {
            g_print("failed to unlink elements [queue]|-->[matroskamux] \n");
        }
        
        // 2b) send eos to matroskamux sink pad to flush buffers
        gst_pad_send_event(muxsinkpad, gst_event_new_eos ());
        // 2b) send eos to matroskamux sink pad to flush buffers
        filesinkpad = gst_element_get_static_pad(data->p_filesink,"sink");
        gst_pad_send_event(filesinkpad, gst_event_new_eos ());

        // 2c) Create [matroskamux_next]-->[filesink_next]
        //data->p_matroskamux_next = gst_element_factory_make("matroskamux","matroskamux_next");
        data->p_matroskamux_next = gst_element_factory_make("matroskamux","matroskamux_next");
        data->p_filesink_next = gst_element_factory_make("filesink","filesink_next");

        filename = (gchar*) g_malloc(strlen(data->filePrefix)+sizeof(data->indexFile)+strlen(".mkv")+1);
        sprintf(filename,"%s%d.mkv",data->filePrefix,data->indexFile);
        data->indexFile++;
        g_object_set(G_OBJECT(data->p_filesink_next), "location", filename, NULL);
        g_free(filename);

        // 2d) add them to pipeline
        gst_bin_add_many(GST_BIN(data->pipeline), data->p_matroskamux_next, data->p_filesink_next, NULL);

        // 2d) link [matroskamux_next]-->[filesink_next]
        if(gst_element_link_pads(data->p_matroskamux_next, "src", data->p_filesink_next, "sink") ==false)
        {
            g_print("failed to link elements [matroskamux_next]-->[filesink_next] \n");
        }


        // 2e) sync with parents 
        gst_element_sync_state_with_parent  (data->p_matroskamux_next);
        gst_element_sync_state_with_parent  (data->p_filesink_next);


        // 2f) link new mux to queue : [queue]-->[matroskamux_next]
        muxnextsinkpad = gst_element_get_request_pad(data->p_matroskamux_next,"video_0");
        if(GST_PAD_LINK_FAILED(gst_pad_link (queuesrcpad, muxnextsinkpad)))
        {
            g_print("failed to unlink elements [queue]-->[matroskamux_next] \n");
        }


    }else if ( data->idMux == 1 )
    {
        // 2a) unlink : [queue]|-->[matroskamux]
        queuesrcpad = gst_element_get_static_pad(data->p_queue,"src");
        muxsinkpad = gst_element_get_static_pad(data->p_matroskamux_next,"video_0");
        if(FALSE == gst_pad_unlink (queuesrcpad, muxsinkpad))
        {
            g_print("failed to unlink elements [queue]|-->[matroskamux] \n");
        }
        
        // 2b) send eos to matroskamux sink pad to flush buffers
        gst_pad_send_event(muxsinkpad, gst_event_new_eos ());
        // 2b) send eos to matroskamux sink pad to flush buffers
        filesinkpad = gst_element_get_static_pad(data->p_filesink_next,"sink");
        gst_pad_send_event(filesinkpad, gst_event_new_eos ());

        // 2c) Create [matroskamux_next]-->[filesink_next]
        data->p_matroskamux = gst_element_factory_make("matroskamux","matroskamux");
        data->p_filesink = gst_element_factory_make("filesink","filesink");

        filename = (gchar*) g_malloc(strlen(data->filePrefix)+sizeof(data->indexFile)+strlen(".mkv")+1);
        sprintf(filename,"%s%d.mkv",data->filePrefix,data->indexFile);
        data->indexFile++;
        g_object_set(G_OBJECT(data->p_filesink), "location", filename, NULL);
        g_free(filename);

        // 2d) add them to pipeline
        gst_bin_add_many(GST_BIN(data->pipeline), data->p_matroskamux, data->p_filesink, NULL);

        // 2d) link [matroskamux_next]-->[filesink_next]
        if(gst_element_link_pads(data->p_matroskamux, "src", data->p_filesink, "sink") ==false)
        {
            g_print("failed to link elements [matroskamux]-->[filesink] \n");
        }


        // 2e) sync with parents 
        gst_element_sync_state_with_parent  (data->p_matroskamux);
        gst_element_sync_state_with_parent  (data->p_filesink);


        // 2f) link new mux to queue : [queue]-->[matroskamux_next]
        muxnextsinkpad = gst_element_get_request_pad(data->p_matroskamux,"video_0");
        if(GST_PAD_LINK_FAILED(gst_pad_link (queuesrcpad, muxnextsinkpad)))
        {
            g_print("failed to unlink elements [queue]-->[matroskamux_next] \n");
        }

    }
    //TODO : clean pads ?

    
    return GST_PAD_PROBE_REMOVE;
}


static gboolean cb_change_filesink
(
 gpointer    p_data)
{
    CustomData* data = (CustomData*) p_data;
    g_print("> cb_change_filesink \n");
    GstPad * queuesrcpad = gst_element_get_static_pad(data->p_queue,"src");
    /* We have to chose GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, 
     * otherwise we gonna call 2 times the callback */
    data->idProbe = gst_pad_add_probe (queuesrcpad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                  (GstPadProbeCallback) cb_blocked, p_data, NULL);
    return TRUE;
}


static void cb_new_rtspsrc_pad
(
 GstElement * element,
 GstPad     * pad,
 gpointer    data)
{
        gchar *name;

        name = gst_pad_get_name(pad);
        g_print("A new pad %s was created\n", name);

        // here, you would setup a new pad link for the newly created pad
        // sooo, now find that rtph264depay is needed and link them?
        GstCaps * p_caps = gst_pad_get_pad_template_caps (pad);

        gchar * description = gst_caps_to_string(p_caps);
        std::cout << p_caps << ", " << description << std::endl;
        g_free(description);

        GstElement * p_rtph264depay = GST_ELEMENT(data);

        // try to link the pads then ...
        if(gst_element_link_pads(element, name, p_rtph264depay, "sink") == false)
        {
                g_print("cb_new_rtspsrc_pad : failed to link elements \n");
        }

        g_free(name);
}


int
main (int   argc, char *argv[])
{
        // init GStreamer
        gst_init (&argc, &argv);

        // Enable debug logs
        /*
        gst_debug_set_active(true);
        gst_debug_set_threshold_for_name ("*", GST_LEVEL_INFO );
        */

        /*
         * PIPELINE DESCRIPTION
                     
        [rtspsrc]-->[rtph264depay]-->[h264parse]-->[queue]-->[matroskamux]-->[filesink]
                 --> TODO: audio
         *
         * */

        CustomData data;
        GstBus *bus;
        guint bus_watch_id;
        data.pipeline = gst_pipeline_new("my_pipeline");
        data.indexFile = 0;
        data.filePrefix = "file_part_";
        data.idMux = 0;

        // create rtsp source
        data.rtspsrc = gst_element_factory_make("rtspsrc", "rtspsrc");
        g_object_set(G_OBJECT(data.rtspsrc), "location","rtsp://10.33.40.227:554/udpstream_ch1_stream1_h264", NULL);
        //g_object_set(G_OBJECT(data.rtspsrc), "location","rtsp://127.0.0.1:8554/test", NULL);

        // create pipeline elements
        data.p_rtph264depay = gst_element_factory_make("rtph264depay","rtph264depay");
        data.p_h264parse = gst_element_factory_make("h264parse","h264parse");
        //data.p_matroskamux = gst_element_factory_make("matroskamux","matroskamux");
        data.p_matroskamux = gst_element_factory_make("matroskamux","matroskamux");
        data.p_filesink = gst_element_factory_make("filesink","filesink");
        data.p_queue = gst_element_factory_make("queue","queue");

        gchar* filename = (gchar*) g_malloc(strlen(data.filePrefix)+sizeof(data.indexFile)+strlen(".mkv")+1);
        sprintf(filename,"%s%d.mkv",data.filePrefix,data.indexFile);
        data.indexFile++;
        g_object_set(G_OBJECT(data.p_filesink), "location", filename, NULL);
        g_free(filename);

        // listen for newly created pads
        // will connect [rtspsrc]-->[rtph264depay] dynamically
        g_signal_connect(data.rtspsrc, "pad-added", G_CALLBACK(cb_new_rtspsrc_pad),data.p_rtph264depay);

        // put together a pipeline
        gst_bin_add_many(GST_BIN(data.pipeline), 
                data.rtspsrc, 
                data.p_rtph264depay,
                data.p_h264parse,
                data.p_queue,
                data.p_matroskamux,
                data.p_filesink,
                NULL);

        // link elements
        // link: [rtph264depay]-->[h264parse]
        if(gst_element_link_pads(data.p_rtph264depay, "src", data.p_h264parse, "sink") == false)
        {
            g_print("failed to link elements [rtph264depay]-->[h264parse] \n");
            return 1;
        }

        // link: [h264parse]-->[queue]
        GstPad * h264parsesrcpad = gst_element_get_static_pad(data.p_h264parse, "src");
        GstPad * queuesinkpad = gst_element_get_static_pad(data.p_queue,"sink");
        if(GST_PAD_LINK_FAILED(gst_pad_link (h264parsesrcpad, queuesinkpad)))
        {
            g_print("failed to link elements [h264parse]-->[queue] \n");
            return 1;
        }

        // link: [queue]-->[matroskamux]
        GstPad * queuesrcpad = gst_element_get_static_pad(data.p_queue,"src");
        GstPad * muxsinkpad = gst_element_get_request_pad(data.p_matroskamux,"video_0");
        if(GST_PAD_LINK_FAILED(gst_pad_link (queuesrcpad, muxsinkpad)))
        {
            g_print("failed to link elements  [queue]-->[matroskamux] \n");
            return 1;
        }

        // link: [matroskamux]-->[filesink]
        if(gst_element_link_pads(data.p_matroskamux, "src", data.p_filesink, "sink") == false)
        {
            g_print("failed to link elements  p_matroskamux video_0 and p_filesink \n");
            return 1;
        }

        // 10 secondes
        g_timeout_add (30000, cb_change_filesink,&data);

        // add bus message
        bus = gst_pipeline_get_bus (GST_PIPELINE (data.pipeline));
        bus_watch_id = gst_bus_add_watch (bus, my_bus_callback, &data);
        gst_object_unref (bus);

        //TODO : Debug DOT File not working
        //GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(data.pipeline), GST_DEBUG_GRAPH_SHOW_ALL , "/home/cctvuser/DEV/Gstreamer-dev/mydotgraph");
        // start the pipeline
        gst_element_set_state(GST_ELEMENT(data.pipeline), GST_STATE_PLAYING);
        data.loop = g_main_loop_new(NULL, FALSE);
        g_main_loop_run(data.loop);

        // TODO: clean up
        //gst_object_unref(GST_OBJECT(element));

        return 0;
}
