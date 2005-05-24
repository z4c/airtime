/*------------------------------------------------------------------------------

    Copyright (c) 2004 Media Development Loan Fund
 
    This file is part of the LiveSupport project.
    http://livesupport.campware.org/
    To report bugs, send an e-mail to bugs@campware.org
 
    LiveSupport is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
  
    LiveSupport is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
 
    You should have received a copy of the GNU General Public License
    along with LiveSupport; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 
 
    Author   : $Author: maroy $
    Version  : $Revision: 1.1 $
    Location : $Source: /home/paul/cvs2svn-livesupport/newcvsrepo/livesupport/modules/playlistExecutor/src/GstreamerPlayer.cxx,v $

------------------------------------------------------------------------------*/

/* ============================================================ include files */

#ifdef HAVE_CONFIG_H
#include "configure.h"
#endif

#include "LiveSupport/Core/TimeConversion.h"
#include "GstreamerPlayer.h"


using namespace boost::posix_time;
using namespace LiveSupport::Core;
using namespace LiveSupport::PlaylistExecutor;

/* ===================================================  local data structures */


/* ================================================  local constants & macros */

/**
 *  The name of the config element for this class
 */
const std::string GstreamerPlayer::configElementNameStr = "gstreamerPlayer";

/**
 *  The name of the audio device attribute.
 */
static const std::string    audioDeviceName = "audioDevice";

/**
 *  The factories considered when creating the pipeline
 */
GList * GstreamerPlayer::factories = 0;


/* ===============================================  local function prototypes */


/* =============================================================  module code */

/*------------------------------------------------------------------------------
 *  Configure the Helix Player.
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: configure(const xmlpp::Element   &  element)
                                                throw (std::invalid_argument,
                                                       std::logic_error)
{
    if (element.get_name() != configElementNameStr) {
        std::string eMsg = "Bad configuration element ";
        eMsg += element.get_name();
        throw std::invalid_argument(eMsg);
    }

    const xmlpp::Attribute    * attribute;

    if ((attribute = element.get_attribute(audioDeviceName))) {
        audioDevice = attribute->get_value();
    }
}


/*------------------------------------------------------------------------------
 *  Initialize the Helix Player
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: initialize(void)                 throw (std::exception)
{
    if (initialized) {
        return;
    }

    // initialize the gstreamer library
    if (!gst_init_check(0, 0)) {
        throw std::runtime_error("couldn't initialize the gstreamer library");
    }
    initFactories();

    // initialize the pipeline
    pipeline   = gst_thread_new("audio-player");
    // take ownership of the pipeline object
    gst_object_ref(GST_OBJECT(pipeline));
    gst_object_sink(GST_OBJECT(pipeline));

    filesrc    = gst_element_factory_make("filesrc", "file-source");
    typefinder = gst_element_factory_make("typefind", "typefind");
    gst_element_link(filesrc, typefinder);
    gst_bin_add_many(GST_BIN(pipeline), filesrc, typefinder, NULL);

    g_signal_connect(pipeline, "error", G_CALLBACK(errorHandler), this);
    g_signal_connect(pipeline, "state-change", G_CALLBACK(stateChange), this);
    g_signal_connect(typefinder, "have-type", G_CALLBACK(typeFound), this);

    audiosink = gst_element_factory_make("alsasink", "audiosink");
    setAudioDevice(audioDevice);

    // set up other variables
    initialized = true;
}


/*------------------------------------------------------------------------------
 *  Initialize the list of factories
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: initFactories(void)                      throw ()
{
    if (factories) {
        return;
    }

    factories = gst_registry_pool_feature_filter(
                        (GstPluginFeatureFilter) featureFilter, FALSE, NULL);
    // sort the factories according to their ranks
    factories = g_list_sort(factories, (GCompareFunc) compareRanks);
}


/*------------------------------------------------------------------------------
 *  Filter plugins so that only demixers, decoders and parsers are considered
 *----------------------------------------------------------------------------*/
gboolean
GstreamerPlayer :: featureFilter(GstPluginFeature   * feature,
                                 gpointer             data)
                                                                    throw ()
{
    const gchar * klass;
    guint         rank;

    // we only care about element factories
    if (!GST_IS_ELEMENT_FACTORY(feature)) {
        return FALSE;
    }

    // only parsers, demuxers and decoders
    klass = gst_element_factory_get_klass(GST_ELEMENT_FACTORY(feature));
    if (g_strrstr(klass, "Demux") == NULL &&
        g_strrstr(klass, "Decoder") == NULL &&
        g_strrstr(klass, "Parse") == NULL) {

        return FALSE;
    }

    // only select elements with autoplugging rank
    rank = gst_plugin_feature_get_rank(feature);
    if (rank < GST_RANK_MARGINAL) {
        return FALSE;
    }

    return TRUE;
}


/*------------------------------------------------------------------------------
 *  Compare two plugins according to their ranks
 *----------------------------------------------------------------------------*/
gint
GstreamerPlayer :: compareRanks(GstPluginFeature   * feature1,
                                GstPluginFeature   * feature2)
                                                                throw ()
{
    return gst_plugin_feature_get_rank(feature1)
         - gst_plugin_feature_get_rank(feature2);
}


/*------------------------------------------------------------------------------
 *  Handler for gstreamer errors.
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: errorHandler(GstElement   * pipeline,
                                GstElement   * source,
                                GError       * error,
                                gchar        * debug,
                                gpointer       self)
                                                                throw ()
{
    // TODO: handle error
    std::cerr << "gstreamer error: " << error->message << std::endl;
}


/*------------------------------------------------------------------------------
 *  Handler for the event when a matching type has been found
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: typeFound(GstElement      * typefinder,
                             guint             probability,
                             GstCaps         * caps,
                             gpointer          self)
                                                                throw ()
{
    // actually plug now
    GstreamerPlayer   * player = (GstreamerPlayer*) self;
    try {
        player->tryToPlug(gst_element_get_pad(typefinder, "src"), caps);
    } catch (std::logic_error &e) {
        // TODO: handle error
        std::cerr << e.what() << std::endl;
    }
}


/*------------------------------------------------------------------------------
 *  Try to plug a matching element to the specified pad
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: tryToPlug(GstPad           * pad,
                             const GstCaps    * caps)
                                                    throw (std::logic_error)
{
    GstObject    * parent = GST_OBJECT(gst_pad_get_parent(pad));
    const gchar  * mime;
    const GList  * item;
    GstCaps      * res;
    GstCaps      * audiocaps;

    // don't plug if we're already plugged
    if (GST_PAD_IS_LINKED(gst_element_get_pad(audiosink, "sink"))) {
        throw std::logic_error(std::string("Omitting link for pad ")
                              + gst_object_get_name(parent) + ":"
                              + gst_pad_get_name(pad)
                              + " because we're alreadey linked");
    }

    // only plug audio
    mime = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    if (!g_strrstr (mime, "audio")) {
        throw std::logic_error(std::string("Omitting link for pad ")
                              + gst_object_get_name(parent) + ":"
                              + gst_pad_get_name(pad)
                              + " because mimetype "
                              + mime
                              + " is non-audio");
    }

    // can it link to the audiopad?
    audiocaps = gst_pad_get_caps(gst_element_get_pad(audiosink, "sink"));
    res       = gst_caps_intersect(caps, audiocaps);
    if (res && !gst_caps_is_empty(res)) {
        closeLink(pad, audiosink, "sink", NULL);
        gst_caps_free(audiocaps);
        gst_caps_free(res);
        return;
    }
    gst_caps_free(audiocaps);
    gst_caps_free(res);

    // try to plug from our list
    for (item = factories; item != NULL; item = item->next) {
        GstElementFactory * factory = GST_ELEMENT_FACTORY(item->data);
        const GList       * pads;

        for (pads = gst_element_factory_get_pad_templates (factory);
             pads != NULL;
             pads = pads->next) {

            GstPadTemplate * templ = GST_PAD_TEMPLATE(pads->data);

            // find the sink template - need an always pad
            if (templ->direction != GST_PAD_SINK ||
                templ->presence != GST_PAD_ALWAYS) {
                continue;
            }

            // can it link?
            res = gst_caps_intersect(caps, templ->caps);
            if (res && !gst_caps_is_empty(res)) {
                GstElement *element;

                // close link and return
                gst_caps_free(res);
                element = gst_element_factory_create(factory, NULL);
                closeLink(pad,
                          element,
                          templ->name_template,
                          gst_element_factory_get_pad_templates(factory));

                const gchar *klass =
                    gst_element_factory_get_klass(GST_ELEMENT_FACTORY(factory));
                if (g_strrstr(klass, "Decoder")) {
                    // if a decoder element, store it
                    decoder = element;
                    decoderSrc = gst_element_get_pad(decoder, "src");
                }
                return;
            }
            gst_caps_free(res);

            // we only check one sink template per factory, so move on to the
            // next factory now
            break;
        }
    }

    throw std::logic_error(std::string("No compatible pad found to decode ")
                         + mime
                         + " on "
                         + gst_object_get_name (parent) + ":"
                         + gst_pad_get_name (pad));
}


/*------------------------------------------------------------------------------
 *  Close the element links
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: closeLink(GstPad       * srcpad,
                             GstElement   * sinkelement,
                             const gchar  * padname,
                             const GList  * templlist)
                                                                throw ()
{
    gboolean has_dynamic_pads = FALSE;

    // add the element to the pipeline and set correct state
    gst_element_set_state(sinkelement, GST_STATE_PAUSED);
    gst_bin_add(GST_BIN(pipeline), sinkelement);
    gst_pad_link(srcpad, gst_element_get_pad(sinkelement, padname));
    gst_bin_sync_children_state(GST_BIN(pipeline));

    // if we have static source pads, link those. If we have dynamic
    // source pads, listen for new-pad signals on the element
    for ( ; templlist != NULL; templlist = templlist->next) {
        GstPadTemplate *templ = GST_PAD_TEMPLATE(templlist->data);

        // only sourcepads, no request pads
        if (templ->direction != GST_PAD_SRC ||
            templ->presence == GST_PAD_REQUEST) {

            continue;
        }

        switch (templ->presence) {
            case GST_PAD_ALWAYS: {
                GstPad *pad = gst_element_get_pad(sinkelement,
                                                  templ->name_template);
                GstCaps *caps = gst_pad_get_caps(pad);

                // link
                tryToPlug(pad, caps);
                gst_caps_free(caps);
                break;
            }

            case GST_PAD_SOMETIMES:
                has_dynamic_pads = TRUE;
                break;

            default:
                break;
        }
    }

    // listen for newly created pads if this element supports that
    if (has_dynamic_pads) {
        g_signal_connect(sinkelement, "new-pad", G_CALLBACK(newPad), this);
    }
}


/*------------------------------------------------------------------------------
 *  Event handler for when a new dynamic pad is created
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: newPad(GstElement     * element,
                          GstPad         * pad,
                          gpointer         self)
                                                                    throw ()
{
    GstreamerPlayer   * player = (GstreamerPlayer*) self;
    GstCaps           * caps   = gst_pad_get_caps(pad);

    player->tryToPlug(pad, caps);
    gst_caps_free(caps);
}


/*------------------------------------------------------------------------------
 *  Event handler for when the state of the pipeline changes
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: stateChange(GstElement    * element,
                               gint            oldState,
                               gint            newState,
                               gpointer        self)
                                                                    throw ()
{
    GstreamerPlayer   * player = (GstreamerPlayer*) self;

    if (oldState == GST_STATE_PLAYING && newState != GST_STATE_PLAYING) {
        player->fireOnStopEvent();
    }
}


/*------------------------------------------------------------------------------
 *  De-initialize the Gstreamer Player
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: deInitialize(void)                       throw ()
{
    if (initialized) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_bin_sync_children_state(GST_BIN(pipeline));

        if (!gst_element_get_parent(audiosink)) {
            // delete manually, if audiosink wasn't added to the pipeline
            // for some reason
            gst_object_unref(GST_OBJECT(audiosink));
        }
        gst_object_unref(GST_OBJECT(pipeline));

        initialized = false;
    }
}


/*------------------------------------------------------------------------------
 *  Attach an event listener.
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: attachListener(AudioPlayerEventListener*     eventListener)
                                                                    throw ()
{
    listeners.push_back(eventListener);
}


/*------------------------------------------------------------------------------
 *  Detach an event listener.
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: detachListener(AudioPlayerEventListener*     eventListener)
                                                throw (std::invalid_argument)
{
    ListenerVector::iterator    it  = listeners.begin();
    ListenerVector::iterator    end = listeners.end();

    while (it != end) {
        if (*it == eventListener) {
            listeners.erase(it);
            return;
        }
        ++it;
    }

    throw std::invalid_argument("supplied event listener not found");
}


/*------------------------------------------------------------------------------
 *  Send the onStop event to all attached listeners.
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: fireOnStopEvent(void)                        throw ()
{
    ListenerVector::iterator    it  = listeners.begin();
    ListenerVector::iterator    end = listeners.end();

    while (it != end) {
        (*it)->onStop();
        ++it;
    }
}


/*------------------------------------------------------------------------------
 *  Specify which file to play
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: open(const std::string   fileUrl)
                                                throw (std::invalid_argument)
{
    std::string     filePath;

    if (fileUrl.find("file:") == 0) {
        filePath = fileUrl.substr(5, fileUrl.size());
    } else if (fileUrl.find("file://") == 0) {
        filePath = fileUrl.substr(7, fileUrl.size());
    } else {
        throw std::invalid_argument("badly formed URL or unsupported protocol");
    }

    g_object_set(G_OBJECT(filesrc), "location", filePath.c_str(), NULL);
}


/*------------------------------------------------------------------------------
 *  Tell if we've been opened.
 *----------------------------------------------------------------------------*/
bool
GstreamerPlayer :: isOpened(void)                               throw ()
{
    gchar     * str;

    g_object_get(G_OBJECT(filesrc), "location", &str, NULL);

    return str != 0;
}


/*------------------------------------------------------------------------------
 *  Get the length of the current audio clip.
 *----------------------------------------------------------------------------*/
Ptr<time_duration>::Ref
GstreamerPlayer :: getPlaylength(void)                      throw ()
{
    Ptr<time_duration>::Ref   length;
    gint64                    ns;
    GstFormat                 format = GST_FORMAT_TIME;

    if (decoderSrc
     && gst_pad_query(decoderSrc, GST_QUERY_TOTAL, &format, &ns)
     && format == GST_FORMAT_TIME) {

        // use microsec, as nanosec() is not found by the compiler (?)
        length.reset(new time_duration(microsec(ns / 1000LL)));
    }
    
    return length;
}


/*------------------------------------------------------------------------------
 *  Start playing
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: start(void)                      throw (std::logic_error)
{
    if (!isOpened()) {
        throw std::logic_error("GstreamerPlayer not opened yet");
    }

    if (!isPlaying()) {
        gst_element_set_state(audiosink, GST_STATE_PAUSED);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
    }
}


/*------------------------------------------------------------------------------
 *  Pause the player
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: pause(void)                      throw (std::logic_error)
{
    if (isPlaying()) {
        gst_element_set_state(pipeline, GST_STATE_PAUSED);
    }
}


/*------------------------------------------------------------------------------
 *  Tell if we're playing
 *----------------------------------------------------------------------------*/
bool
GstreamerPlayer :: isPlaying(void)                  throw ()
{
    return gst_element_get_state(pipeline) == GST_STATE_PLAYING;
}


/*------------------------------------------------------------------------------
 *  Stop playing
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: stop(void)                       throw (std::logic_error)
{
    if (!isOpened()) {
        throw std::logic_error("GstreamerPlayer not opened yet");
    }

    if (isPlaying()) {
        gst_element_set_state(pipeline, GST_STATE_READY);
    }
}
 

/*------------------------------------------------------------------------------
 *  Close the currently opened audio file.
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: close(void)                       throw ()
{
    if (isPlaying()) {
        stop();
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
}


/*------------------------------------------------------------------------------
 *  Get the volume of the player.
 *----------------------------------------------------------------------------*/
unsigned int
GstreamerPlayer :: getVolume(void)                                  throw ()
{
    return 0;
}


/*------------------------------------------------------------------------------
 *  Set the volume of the player.
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: setVolume(unsigned int   volume)                 throw ()
{
}


/*------------------------------------------------------------------------------
 *  Open a playlist, with simulated fading.
 *----------------------------------------------------------------------------*/
void
GstreamerPlayer :: openAndStart(Ptr<Playlist>::Ref  playlist)       
                                                throw (std::invalid_argument,
                                                       std::logic_error,
                                                       std::runtime_error)
{
}


/*------------------------------------------------------------------------------
 *  Set the audio device.
 *----------------------------------------------------------------------------*/
bool
GstreamerPlayer :: setAudioDevice(const std::string &deviceName)       
                                                                throw ()
{
    // TODO: support OSS as well
    if (deviceName.size() > 0) {
        g_object_set(G_OBJECT(audiosink), "device", deviceName.c_str(), NULL);
    }

    return true;
}

