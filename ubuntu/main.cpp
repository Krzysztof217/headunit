#include <glib.h>
#include <stdio.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/interfaces/xoverlay.h>
#include <gdk/gdk.h>
#include <SDL/SDL.h>
#include <SDL/SDL_syswm.h>
#include <time.h>
#include <glib-unix.h>


#include "hu_uti.h"
#include "hu_aap.h"
#include "generated/hu.pb.h"

typedef struct {
	GMainLoop *loop;
	GstPipeline *pipeline;
	GstAppSrc *src;
	GstElement *sink;
	GstElement *decoder;
	GstElement *convert;
	guint sourceid;
} gst_app_t;


int terminate_app = 0;
static gst_app_t gst_app;

GstElement *mic_pipeline, *mic_sink;

GstElement *aud_pipeline, *aud_src;

GstElement *au1_pipeline, *au1_src;


int mic_change_state = 0;

pthread_mutex_t mutexsend;

float g_dpi_scalefactor = 1.0f;


static void read_mic_data (GstElement * sink);

struct cmd_arg_struct {
    int retry;
    int chan;
    int cmd_len; 
    unsigned char *cmd_buf; 
    int res_max; 
    unsigned char *res_buf;
    int result;
};

static gboolean read_data(gst_app_t *app)
{
	GstBuffer *buffer;
	guint8 *ptr;
	GstFlowReturn ret;
	int iret;
	guint8 *vbuf;
	guint8 *abuf;
	int res_len = 0;

	pthread_mutex_lock (&mutexsend);
	iret = hu_aap_recv_process ();                       
	pthread_mutex_unlock (&mutexsend);
	
	if (iret != 0) {
		printf("hu_aap_recv_process() iret: %d\n", iret);
		g_main_loop_quit(app->loop);
		return FALSE;
	}

	/* Is there a video buffer queued? */
	vbuf = (guint8*)vid_read_head_buf_get (&res_len);
	if (vbuf != NULL) {
		
		buffer = gst_buffer_new();
		gst_buffer_set_data(buffer, vbuf, res_len);
		ret = gst_app_src_push_buffer((GstAppSrc *)app->src, buffer);

		if(ret !=  GST_FLOW_OK){
			printf("push buffer returned %d for %d bytes \n", ret, res_len);
			return FALSE;
		}
	}

	/* Is there an audio buffer queued? */
	abuf = (guint8*)aud_read_head_buf_get (&res_len);
	if (abuf != NULL) {
		
		buffer = gst_buffer_new();
		gst_buffer_set_data(buffer, abuf, res_len);
		
		if (res_len <= 2048 + 96)
			ret = gst_app_src_push_buffer((GstAppSrc *)au1_src, buffer);
		else
			ret = gst_app_src_push_buffer((GstAppSrc *)aud_src, buffer);

		if(ret !=  GST_FLOW_OK){
			printf("push buffer returned %d for %d bytes \n", ret, res_len);
			return FALSE;
		}
	}

	return TRUE;
}

static void start_feed (GstElement * pipeline, guint size, gst_app_t *app)
{
	if (app->sourceid == 0) {
		GST_DEBUG ("start feeding");
		app->sourceid = g_idle_add ((GSourceFunc) read_data, app);
	}
}

static void stop_feed (GstElement * pipeline, gst_app_t *app)
{
	if (app->sourceid != 0) {
		GST_DEBUG ("stop feeding");
		g_source_remove (app->sourceid);
		app->sourceid = 0;
	}
}

static void on_pad_added(GstElement *element, GstPad *pad)
{
	GstCaps *caps;
	GstStructure *str;
	gchar *name;
	GstPad *convertsink;
	GstPadLinkReturn ret;

	g_debug("pad added");

	g_assert(pad);

	caps = gst_pad_get_caps(pad);
	
	g_assert(caps);
	
	str = gst_caps_get_structure(caps, 0);

	g_assert(str);

	name = (gchar*)gst_structure_get_name(str);

	g_debug("pad name %s", name);

	if(g_strrstr(name, "video")){

		convertsink = gst_element_get_static_pad(gst_app.convert, "sink");
		g_assert(convertsink);
		ret = gst_pad_link(pad, convertsink);
		g_debug("pad_link returned %d\n", ret);
		gst_object_unref(convertsink);
	}
	gst_caps_unref(caps);
}

static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer *ptr)
{
	gst_app_t *app = (gst_app_t*)ptr;

	switch(GST_MESSAGE_TYPE(message)){

		case GST_MESSAGE_ERROR:{
					       gchar *debug;
					       GError *err;

					       gst_message_parse_error(message, &err, &debug);
					       g_print("Error %s\n", err->message);
					       g_error_free(err);
					       g_free(debug);
					       g_main_loop_quit(app->loop);
				       }
				       break;

		case GST_MESSAGE_WARNING:{
						 gchar *debug;
						 GError *err;
						 gchar *name;

						 gst_message_parse_warning(message, &err, &debug);
						 g_print("Warning %s\nDebug %s\n", err->message, debug);

						 name = (gchar *)GST_MESSAGE_SRC_NAME(message);

						 g_print("Name of src %s\n", name ? name : "nil");
						 g_error_free(err);
						 g_free(debug);
					 }
					 break;

		case GST_MESSAGE_EOS:
					 g_print("End of stream\n");
					 g_main_loop_quit(app->loop);
					 break;

		case GST_MESSAGE_STATE_CHANGED:
					 break;

		default:
//					 g_print("got message %s\n", \
							 gst_message_type_get_name (GST_MESSAGE_TYPE (message)));
					 break;
	}

	return TRUE;
}

static int gst_pipeline_init(gst_app_t *app)
{
	GstBus *bus;
	GstStateChangeReturn state_ret;

	GError *error = NULL;

	gst_init(NULL, NULL);


    app->pipeline = (GstPipeline*)gst_parse_launch("appsrc name=mysrc is-live=true block=false max-latency=100000 do-timestamp=true ! video/x-h264, width=800,height=480,framerate=30/1 ! decodebin2 name=mydecoder ! videoscale name=myconvert ! xvimagesink name=mysink", &error);

	bus = gst_pipeline_get_bus(app->pipeline);
	gst_bus_add_watch(bus, (GstBusFunc)bus_callback, app);
	gst_object_unref(bus);

	app->src = (GstAppSrc*)gst_bin_get_by_name (GST_BIN (app->pipeline), "mysrc");
	app->decoder = gst_bin_get_by_name (GST_BIN (app->pipeline), "mydecoder");
	app->convert = gst_bin_get_by_name (GST_BIN (app->pipeline), "myconvert");
	app->sink = gst_bin_get_by_name (GST_BIN (app->pipeline), "mysink");

	g_assert(app->src);
	g_assert(app->decoder);
	g_assert(app->convert);
	g_assert(app->sink);

	g_signal_connect(app->src, "need-data", G_CALLBACK(start_feed), app);
	g_signal_connect(app->src, "enough-data", G_CALLBACK(stop_feed), app);
	g_signal_connect(app->decoder, "pad-added",
			G_CALLBACK(on_pad_added), app->decoder);

	
	aud_pipeline = gst_parse_launch("appsrc name=audsrc ! audio/x-raw-int, signed=true, endianness=1234, depth=16, width=16, rate=48000, channels=2 ! volume volume=0.5 ! alsasink buffer-time=400000",&error);

	if (error != NULL) {
		printf("could not construct pipeline: %s\n", error->message);
		g_clear_error (&error);	
		return -1;
	}	

	aud_src = gst_bin_get_by_name (GST_BIN (aud_pipeline), "audsrc");
	
	gst_app_src_set_stream_type((GstAppSrc *)aud_src, GST_APP_STREAM_TYPE_STREAM);


	au1_pipeline = gst_parse_launch("appsrc name=au1src ! audio/x-raw-int, signed=true, endianness=1234, depth=16, width=16, rate=16000, channels=1 ! volume volume=0.5 ! alsasink buffer-time=400000 ",&error);

	if (error != NULL) {
		printf("could not construct pipeline: %s\n", error->message);
		g_clear_error (&error);	
		return -1;
	}	

	au1_src = gst_bin_get_by_name (GST_BIN (au1_pipeline), "au1src");
	
	gst_app_src_set_stream_type((GstAppSrc *)au1_src, GST_APP_STREAM_TYPE_STREAM);
	
	
	mic_pipeline = gst_parse_launch("alsasrc name=micsrc ! audioconvert ! audio/x-raw-int, signed=true, endianness=1234, depth=16, width=16, channels=1, rate=16000 ! queue !appsink name=micsink async=false emit-signals=true blocksize=8192",&error);
	
	if (error != NULL) {
		printf("could not construct pipeline: %s\n", error->message);
		g_clear_error (&error);	
		return -1;
	}
		
	mic_sink = gst_bin_get_by_name (GST_BIN (mic_pipeline), "micsink");
	
	g_object_set(G_OBJECT(mic_sink), "throttle-time", 3000000, NULL);
		
	g_signal_connect(mic_sink, "new-buffer", G_CALLBACK(read_mic_data), NULL);
	
	gst_element_set_state (mic_pipeline, GST_STATE_READY);

	return 0;
}

static size_t uleb128_encode(uint64_t value, uint8_t *data)
{
	uint8_t cbyte;
	size_t enc_size = 0;

	do {
		cbyte = value & 0x7f;
		value >>= 7;
		if (value != 0)
			cbyte |= 0x80;
		data[enc_size++] = cbyte;
	} while (value != 0);

	return enc_size;
}

static size_t varint_encode (uint64_t val, uint8_t *ba, int idx) {
	
	if (val >= 0x7fffffffffffffff) {
		return 1;
	}

	uint64_t left = val;
	int idx2 = 0;
	
	for (idx2 = 0; idx2 < 9; idx2 ++) {
		ba [idx+idx2] = (uint8_t) (0x7f & left);
		left = left >> 7;
		if (left == 0) {
			return (idx2 + 1);
		}
		else if (idx2 < 9 - 1) {
			ba [idx+idx2] |= 0x80;
		}
	}
	
	return 9;
}

#define ACTION_DOWN	0
#define ACTION_UP	1
#define ACTION_MOVE	2
#define TS_MAX_REQ_SZ	32
static const uint8_t ts_header[] ={0x80, 0x01, 0x08};
static const uint8_t ts_sizes[] = {0x1a, 0x09, 0x0a, 0x03};
static const uint8_t ts_footer[] = {0x10, 0x00, 0x18};

static int aa_touch_event(uint8_t action, int x, int y) {
	struct timespec tp;
	uint8_t *buf;
	int idx;
	int siz_arr = 0;
	int size1_idx, size2_idx, i;
	int axis = 0;
	int coordinates[3] = {x, y, 0};
	int ret;

	buf = (uint8_t *)malloc(TS_MAX_REQ_SZ);
	if(!buf) {
		printf("Failed to allocate touchscreen event buffer\n");
		return -1;
	}

	/* Fetch the time stamp */
	clock_gettime(CLOCK_REALTIME, &tp);

	/* Copy header */
	memcpy(buf, ts_header, sizeof(ts_header));
//	idx = sizeof(ts_header) +
//	      uleb128_encode(tp.tv_nsec, buf + sizeof(ts_header));
	idx = sizeof(ts_header) +
	      varint_encode(tp.tv_sec * 1000000000 +tp.tv_nsec, buf + sizeof(ts_header),0);
	size1_idx = idx + 1;
	size2_idx = idx + 3;

	/* Copy sizes */
	memcpy(buf+idx, ts_sizes, sizeof(ts_sizes));
	idx += sizeof(ts_sizes);

	/* Set magnitude of each axis */
	for (i=0; i<3; i++) {
		axis += 0x08;
		buf[idx++] = axis;
		/* FIXME The following can be optimzed to update size1/2 at end of loop */
		siz_arr = uleb128_encode(coordinates[i], &buf[idx]);
		idx += siz_arr;
		buf[size1_idx] += siz_arr;
		buf[size2_idx] += siz_arr;
	}

	/* Copy footer */
	memcpy(buf+idx, ts_footer, sizeof(ts_footer));
	idx += sizeof(ts_footer);

	buf[idx++] = action;

	/* Send touch event */
	
	pthread_mutex_lock (&mutexsend);
	ret = hu_aap_enc_send (0, AA_CH_TOU, buf, idx);
	pthread_mutex_unlock (&mutexsend);
	
	if (ret < 0) {
		printf("aa_touch_event(): hu_aap_enc_send() failed with (%d)\n", ret);
	}

	free(buf);
		
	return ret;
}

static size_t uptime_encode(uint64_t value, uint8_t *data)
{
	int ctr = 0;
	for (ctr = 7; ctr >= 0; ctr --) {                           // Fill 8 bytes backwards
		data [6 + ctr] = (uint8_t)(value & 0xFF);
		value = value >> 8;
	}

	return 8;
}

static const uint8_t mic_header[] ={0x00, 0x00};
static const int max_size = 8192;


static void read_mic_data (GstElement * sink)
{
		
	GstBuffer *gstbuf;
	int ret;
	
	g_signal_emit_by_name (sink, "pull-buffer", &gstbuf,NULL);
	
	if (gstbuf) {

		struct timespec tp;

		/* if mic is stopped, don't bother sending */	

		if (mic_change_state == 0) {
			printf("Mic stopped.. dropping buffers \n");
			gst_buffer_unref(gstbuf);
			return;
		}
		
		/* Fetch the time stamp */
		clock_gettime(CLOCK_REALTIME, &tp);
		
		gint mic_buf_sz = GST_BUFFER_SIZE (gstbuf);
		
		int idx;
		
		if (mic_buf_sz <= 64) {
			printf("Mic data < 64 \n");
			return;
		}
				
		uint8_t *mic_buffer = (uint8_t *)malloc(14 + mic_buf_sz);
		
		/* Copy header */
		memcpy(mic_buffer, mic_header, sizeof(mic_header));
		
		idx = sizeof(mic_header) + uptime_encode(tp.tv_nsec * 0.001, mic_buffer);

		/* Copy PCM Audio Data */
		memcpy(mic_buffer+idx, GST_BUFFER_DATA(gstbuf), mic_buf_sz);
		idx += mic_buf_sz;
						
		pthread_mutex_lock (&mutexsend);
		ret = hu_aap_enc_send (1, AA_CH_MIC, mic_buffer, idx);
		pthread_mutex_unlock (&mutexsend);
		
		if (ret < 0) {
			printf("read_mic_data(): hu_aap_enc_send() failed with (%d)\n", ret);
		}
		
		gst_buffer_unref(gstbuf);
		free(mic_buffer);
	}
}

int nightmode = 0;


    /* Print all information about a key event */
    void PrintKeyInfo( SDL_KeyboardEvent *key ){
        /* Is it a release or a press? */
        if( key->type == SDL_KEYUP )
            printf( "Release:- " );
        else
            printf( "Press:- " );

        /* Print the hardware scancode first */
        printf( "Scancode: 0x%02X", key->keysym.scancode );
        /* Print the name of the key */
        printf( ", Name: %s", SDL_GetKeyName( key->keysym.sym ) );
        /* We want to print the unicode info, but we need to make */
        /* sure its a press event first (remember, release events */
        /* don't have unicode info                                */
        if( key->type == SDL_KEYDOWN ){
            /* If the Unicode value is less than 0x80 then the    */
            /* unicode value can be used to get a printable       */
            /* representation of the key, using (char)unicode.    */
            printf(", Unicode: " );
            if( key->keysym.unicode < 0x80 && key->keysym.unicode > 0 ){
                printf( "%c (0x%04X)", (char)key->keysym.unicode,
                        key->keysym.unicode );
            }
            else{
                printf( "? (0x%04X)", key->keysym.unicode );
            }
        }
        printf( "\n" );
    }
//LEFT turn
uint8_t cd_lefturn[] = { -128,0x01,0x08,0,0,0,0,0,0,0,0,0x14,0x32,0x11,0x0A,0x0F,0x08,-128,-128,0x04,0x10,-1,-1,-1,-1,-1,-1,-1,-1,-1,0x01 };

//RIGHT turn
uint8_t cd_rightturn[] =  { -128,0x01,0x08,0,0,0,0,0,0,0,0,0x14,0x32,0x08,0x0A,0x06,0x08,-128,-128,0x04,0x10,0x01 };


gboolean sdl_poll_event(gpointer data)
{
	gst_app_t *app = (gst_app_t *)data;
	
	int mic_ret = hu_aap_mic_get ();
	
	if (mic_change_state == 0 && mic_ret == 2) {
		printf("SHAI1 : Mic Started\n");
		mic_change_state = 2;
		gst_element_set_state (mic_pipeline, GST_STATE_PLAYING);
	}
		
	if (mic_change_state == 2 && mic_ret == 1) {
		printf("SHAI1 : Mic Stopped\n");
		mic_change_state = 0;
		gst_element_set_state (mic_pipeline, GST_STATE_READY);
	}	
	
	SDL_Event event;
	SDL_MouseButtonEvent *mbevent;
	SDL_KeyboardEvent *key;
	struct timespec tp;

	int ret;
    uint8_t keyTempBuffer[1024];
    size_t keyTempSize = 0;
    static int baseIdx = 90;

	if (SDL_PollEvent(&event) >= 0) {
		switch (event.type) {
		case SDL_MOUSEMOTION:	
			break;
		case SDL_MOUSEBUTTONDOWN:
			mbevent = &event.button;
			if (mbevent->button == SDL_BUTTON_LEFT) {
				ret = aa_touch_event(ACTION_DOWN, (int)((float)mbevent->x/g_dpi_scalefactor ), (int)((float)mbevent->y/g_dpi_scalefactor));
				if (ret == -1) {
					g_main_loop_quit(app->loop);
					SDL_Quit();
					return FALSE;
				}
			}
			break;
		case SDL_MOUSEBUTTONUP:
			mbevent = &event.button;
			if (mbevent->button == SDL_BUTTON_LEFT) {
				ret = aa_touch_event(ACTION_UP, (int)((float)mbevent->x/g_dpi_scalefactor), (int)((float)mbevent->y/g_dpi_scalefactor));
				if (ret == -1) {
					g_main_loop_quit(app->loop);
					SDL_Quit();
					return FALSE;
				}
			} else if (mbevent->button == SDL_BUTTON_RIGHT) {
				printf("Quitting...\n");
				g_main_loop_quit(app->loop);
//				SDL_Quit();
				return FALSE;
			}
			break;
			case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (event.key.keysym.scancode != 0)
                {
                    static uint64_t timestamp = 0;
                    timestamp += 10;
                    PrintKeyInfo( &event.key );
                    key = &event.key;
                    char *cmdkey = SDL_GetKeyName( key->keysym.sym );
                    if (strcmp(cmdkey, "up") == 0) {
                        clock_gettime(CLOCK_REALTIME, &tp);
                        keyTempSize = hu_fill_button_message(keyTempBuffer, timestamp, HUIB_UP, event.type == SDL_KEYDOWN);
                        hu_aap_enc_send (0,AA_CH_TOU, keyTempBuffer, keyTempSize);
                    }			
                    if (strcmp(cmdkey, "down") == 0) {
                        clock_gettime(CLOCK_REALTIME, &tp);
                        keyTempSize = hu_fill_button_message(keyTempBuffer, timestamp, HUIB_DOWN, event.type == SDL_KEYDOWN);
                        hu_aap_enc_send (0,AA_CH_TOU, keyTempBuffer, keyTempSize);

                    }
                                    
                    if (strcmp(cmdkey, "left") == 0) {
                        clock_gettime(CLOCK_REALTIME, &tp);
                        keyTempSize = hu_fill_button_message(keyTempBuffer, timestamp, HUIB_LEFT, event.type == SDL_KEYDOWN);
                        hu_aap_enc_send (0,AA_CH_TOU, keyTempBuffer, keyTempSize);
                    }
                    
                    if (strcmp(cmdkey, "right") == 0) {
                        clock_gettime(CLOCK_REALTIME, &tp);
                        keyTempSize = hu_fill_button_message(keyTempBuffer, timestamp, HUIB_RIGHT, event.type == SDL_KEYDOWN);
                        hu_aap_enc_send (0,AA_CH_TOU, keyTempBuffer, keyTempSize);

                    }
                    
//                    if (strcmp(cmdkey, "1") == 0) {
//                        clock_gettime(CLOCK_REALTIME, &tp);
//                        //varint_encode(timestamp,cd_lefturn,3);
//                        hu_aap_enc_send (0,AA_CH_TOU, cd_lefturn, sizeof(cd_lefturn));
//                    }
//                    
//                    if (strcmp(cmdkey, "2") == 0) {
//                        clock_gettime(CLOCK_REALTIME, &tp);
//                        //varint_encode(timestamp,cd_rightturn,3);
//                        hu_aap_enc_send (0,AA_CH_TOU, cd_rightturn, sizeof(cd_rightturn));
//                    }


                    if (strcmp(cmdkey, "q") == 0 && event.type == SDL_KEYDOWN) {
                        baseIdx+=10;
                        printf("nextTen %i\n", baseIdx);
                    }
                    
                    if (cmdkey[0] >= '0' && cmdkey[0] <= '9')
                    {
                        int code = baseIdx + cmdkey[0] - '0';
                        printf("%i\n", code);
                        clock_gettime(CLOCK_REALTIME, &tp);
                        keyTempSize = hu_fill_button_message(keyTempBuffer, timestamp, (HU_INPUT_BUTTON) code, event.type == SDL_KEYDOWN);
                        hu_aap_enc_send (0,AA_CH_TOU, keyTempBuffer, keyTempSize);
                    }

                    if (strcmp(cmdkey, "m") == 0) {
                        clock_gettime(CLOCK_REALTIME, &tp);
                        keyTempSize = hu_fill_button_message(keyTempBuffer, timestamp, HUIB_MIC, event.type == SDL_KEYDOWN);
                        hu_aap_enc_send (0,AA_CH_TOU, keyTempBuffer, keyTempSize);
                    }

                    if (strcmp(cmdkey, "p") == 0) {
                        clock_gettime(CLOCK_REALTIME, &tp);
                        keyTempSize = hu_fill_button_message(keyTempBuffer, timestamp, HUIB_PREV, event.type == SDL_KEYDOWN);
                        hu_aap_enc_send (0,AA_CH_TOU, keyTempBuffer, keyTempSize);

                    }

                    if (strcmp(cmdkey, "n") == 0) {
                        clock_gettime(CLOCK_REALTIME, &tp);
                        keyTempSize = hu_fill_button_message(keyTempBuffer, timestamp, HUIB_NEXT, event.type == SDL_KEYDOWN);
                        hu_aap_enc_send (0,AA_CH_TOU, keyTempBuffer, keyTempSize);
                    }

                    printf("\n # %s #\n",cmdkey);

                    if (strcmp(cmdkey, "return") == 0) {
                        printf("\n enter pressed \n");
                        clock_gettime(CLOCK_REALTIME, &tp);
                        keyTempSize = hu_fill_button_message(keyTempBuffer, timestamp, HUIB_ENTER, event.type == SDL_KEYDOWN);
                        hu_aap_enc_send (0,AA_CH_TOU, keyTempBuffer, keyTempSize);
                    }

                    if (strcmp(cmdkey, "backspace") == 0) {
                        clock_gettime(CLOCK_REALTIME, &tp);
                        keyTempSize = hu_fill_button_message(keyTempBuffer, timestamp, HUIB_BACK, event.type == SDL_KEYDOWN);
                        hu_aap_enc_send (0,AA_CH_TOU, keyTempBuffer, keyTempSize);

                    }
				}												
				break;

		case SDL_QUIT:
			printf("Quitting...\n");
			g_main_loop_quit(app->loop);
//			SDL_Quit();
			return FALSE;
			break;
		}
	}
	
//  CHECK NIGHT MODE	
	time_t rawtime;
	struct tm *timenow;

	time( &rawtime );
	timenow = localtime( &rawtime );
	
	int nightmodenow = 1;
	
	if (timenow->tm_hour >= 6 && timenow->tm_hour <= 18)
		nightmodenow = 0;
	
	if (nightmode != nightmodenow) {
		nightmode = nightmodenow;
		byte rspds [] = {-128, 0x03, 0x52, 0x02, 0x08, 0x01}; 	// Day = 0, Night = 1 
		if (nightmode == 0)
			rspds[5]= 0x00;
		hu_aap_enc_send (0,AA_CH_SEN, rspds, sizeof (rspds)); 	// Send Sensor Night mode
	}
	
	return TRUE;
}





gboolean commander_poll_event(gpointer data)
{
	
	gst_app_t *app = (gst_app_t *)data;
		
	SDL_Event event;
	SDL_MouseButtonEvent *mbevent;
	int ret;

	if (SDL_PollEvent(&event) >= 0) {
		switch (event.type) {
		case SDL_MOUSEMOTION:	
			break;
		case SDL_MOUSEBUTTONDOWN:
			mbevent = &event.button;
			if (mbevent->button == SDL_BUTTON_LEFT) {
				ret = aa_touch_event(ACTION_DOWN, (int)((float)mbevent->x/g_dpi_scalefactor ), (int)((float)mbevent->y/g_dpi_scalefactor));
				if (ret == -1) {
					g_main_loop_quit(app->loop);
					SDL_Quit();
					return FALSE;
				}
			}
			break;
		case SDL_MOUSEBUTTONUP:
			mbevent = &event.button;
			if (mbevent->button == SDL_BUTTON_LEFT) {
				ret = aa_touch_event(ACTION_UP, (int)((float)mbevent->x/g_dpi_scalefactor), (int)((float)mbevent->y/g_dpi_scalefactor));
				if (ret == -1) {
					g_main_loop_quit(app->loop);
					SDL_Quit();
					return FALSE;
				}
			} else if (mbevent->button == SDL_BUTTON_RIGHT) {
				printf("Quitting...\n");
				g_main_loop_quit(app->loop);
//				SDL_Quit();
				return FALSE;
			}
			break;
		case SDL_QUIT:
			printf("Quitting...\n");
			g_main_loop_quit(app->loop);
//			SDL_Quit();
			return FALSE;
			break;
		}
	}
	
	return TRUE;
}





static gboolean on_sig_received (gpointer data)
{
	gst_app_t *app = (gst_app_t *)data;
	g_main_loop_quit(app->loop);
	return FALSE;
}

static int gst_loop(gst_app_t *app)
{
	int ret;
	GstStateChangeReturn state_ret;

	state_ret = gst_element_set_state((GstElement*)app->pipeline, GST_STATE_PLAYING);
	state_ret = gst_element_set_state((GstElement*)aud_pipeline, GST_STATE_PLAYING);
	state_ret = gst_element_set_state((GstElement*)au1_pipeline, GST_STATE_PLAYING);
//	g_warning("set state returned %d\n", state_ret);

	app->loop = g_main_loop_new (NULL, FALSE);
	g_unix_signal_add (SIGTERM, on_sig_received, app);
	g_timeout_add_full(G_PRIORITY_HIGH, 100, sdl_poll_event, (gpointer)app, NULL);
	printf("Starting Android Auto...\n");
  	g_main_loop_run (app->loop);


	state_ret = gst_element_set_state((GstElement*)app->pipeline, GST_STATE_NULL);
//	g_warning("set state null returned %d\n", state_ret);

	gst_object_unref(app->pipeline);
	gst_object_unref(mic_pipeline);
	gst_object_unref(aud_pipeline);
	gst_object_unref(au1_pipeline);
	
	ms_sleep(100);
	
	printf("here we are \n");
	/* Should not reach this? */
	SDL_Quit();

	return ret;
}

namespace google
{
	namespace protobuf
	{
		void ShutdownProtobufLibrary()
		{
			printf("Do nothing!\n");
		}
	}
}

int main (int argc, char *argv[])
{	

	GOOGLE_PROTOBUF_VERIFY_VERSION;
	//Assuming we are on Gnome, what's the DPI scale factor?
	gdk_init(&argc, &argv);

	GdkScreen * primaryDisplay = gdk_screen_get_default();
	if (primaryDisplay)
	{
		g_dpi_scalefactor = (float)gdk_screen_get_monitor_scale_factor(primaryDisplay, 0);
		printf("Got gdk_screen_get_monitor_scale_factor() == %f\n", g_dpi_scalefactor);
	}

	gst_app_t *app = &gst_app;
	int ret = 0;
	errno = 0;
	byte ep_in_addr  = -2;
	byte ep_out_addr = -2;
	SDL_Cursor *cursor;

	/* Init gstreamer pipelien */
	ret = gst_pipeline_init(app);
	if (ret < 0) {
		printf("STATUS:gst_pipeline_init() ret: %d\n", ret);
		return (ret);
	}


	/* Overlay gst sink on the Qt window */
//	WId xwinid = window->winId();
	
//#endif


	/* Start AA processing */
	ret = hu_aap_start (ep_in_addr, ep_out_addr);
	if (ret == -1)
	{
		printf("Phone switched to accessory mode. Attempting once more.\n");
		sleep(1);
		ret = hu_aap_start (ep_in_addr, ep_out_addr);
	}	
	if (ret < 0) {
		if (ret == -2)
			printf("STATUS:Phone is not connected. Connect a supported phone and restart.\n");
		else if (ret == -1)
			printf("STATUS:Phone switched to accessory mode. Restart to enter AA mode.\n");
		else
			printf("STATUS:hu_app_start() ret: %d\n", ret);
		
		return (ret);
	}
	
	SDL_SysWMinfo info;

	SDL_Init(SDL_INIT_EVERYTHING);
	SDL_WM_SetCaption("Android Auto", NULL);
	SDL_Surface *screen = SDL_SetVideoMode((int)(800*g_dpi_scalefactor), (int)(480*g_dpi_scalefactor), 32, SDL_HWSURFACE);

	struct SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);

	if(-1 == SDL_GetWMInfo(&wmInfo))
		printf("STATUS:errorxxxxx \n");
	
	
	SDL_EnableUNICODE(1);
	
	SDL_EventState( SDL_MOUSEMOTION, SDL_IGNORE );

	gst_x_overlay_set_window_handle(GST_X_OVERLAY(app->sink), wmInfo.info.x11.window);

	//Don't use SDL's weird cursor, too small on HiDPI
	XUndefineCursor(wmInfo.info.x11.display, wmInfo.info.x11.window);

	pthread_mutex_init(&mutexsend, NULL);

	/* Start gstreamer pipeline and main loop */
	ret = gst_loop(app);
	if (ret < 0) {
		printf("STATUS:gst_loop() ret: %d\n", ret);
	}

	/* Stop AA processing */
	ret = hu_aap_stop ();
	if (ret < 0) {
		printf("STATUS:hu_aap_stop() ret: %d\n", ret);
		SDL_Quit();
		pthread_mutex_destroy(&mutexsend);
		pthread_exit(NULL);
		return (ret);
	}

	SDL_Quit();
	
	if (ret == 0) {
		printf("STATUS:Press Back or Home button to close\n");
	}
	
	pthread_mutex_destroy(&mutexsend);

	return (ret);
}
