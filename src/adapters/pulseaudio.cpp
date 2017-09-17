#include "adapters/pulseaudio.hpp"

// TODO possibly move all the callback functions to lambda functions
// create base volume backend class (mixer/control, pulseaudio inherits from base class)
POLYBAR_NS

/* Multichannel volumes:
 * use pa_cvolume_max(), and pa_cvolume_scale()
 *
 * see https://www.freedesktop.org/wiki/Software/PulseAudio/Documentation/Developer/Clients/WritingVolumeControlUIs/
 */

/**
 * Construct pulseaudio object
 */
pulseaudio::pulseaudio(string&& sink_name) : spec_s_name(sink_name) {
  m_mainloop = pa_threaded_mainloop_new();
  if (!m_mainloop) {
    throw pulseaudio_error("Could not create pulseaudio threaded mainloop.");
  }
  pa_threaded_mainloop_lock(m_mainloop);

  m_context = pa_context_new(pa_threaded_mainloop_get_api(m_mainloop), "polybar");
  if (!m_context) {
    pa_threaded_mainloop_unlock(m_mainloop);
    pa_threaded_mainloop_free(m_mainloop);
    throw pulseaudio_error("Could not create pulseaudio context.");
  }
  
  pa_context_set_state_callback(m_context, context_state_callback, this);

  if (pa_context_connect(m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
    pa_context_disconnect(m_context);
    pa_context_unref(m_context);
    pa_threaded_mainloop_unlock(m_mainloop);
    pa_threaded_mainloop_free(m_mainloop);
    throw pulseaudio_error("Could not connect pulseaudio context.");
  }

  if (pa_threaded_mainloop_start(m_mainloop) < 0) {
    pa_context_disconnect(m_context);
    pa_context_unref(m_context);
    pa_threaded_mainloop_unlock(m_mainloop);
    pa_threaded_mainloop_free(m_mainloop);
    throw pulseaudio_error("Could not start pulseaudio mainloop.");
  }

  pa_threaded_mainloop_wait(m_mainloop);
  if (pa_context_get_state(m_context) != PA_CONTEXT_READY) {
    pa_threaded_mainloop_unlock(m_mainloop);
    pa_threaded_mainloop_stop(m_mainloop);
    pa_context_disconnect(m_context);
    pa_context_unref(m_context);
    pa_threaded_mainloop_free(m_mainloop);
    throw pulseaudio_error("Could not connect to pulseaudio server.");
  }

  pa_operation* op = pa_context_get_sink_info_by_name(m_context, sink_name.c_str(), sink_info_callback, this);
  wait_loop(op, m_mainloop);
  if (!exists) {
    op = pa_context_get_server_info(m_context, get_default_sink_callback, this);
    if (!op) {
      throw pulseaudio_error("Failed to get pulseaudio server info.");
    }
    wait_loop(op, m_mainloop);
    // get the sink index
    op = pa_context_get_sink_info_by_name(m_context, def_s_name.c_str(), sink_info_callback, this);
    wait_loop(op, m_mainloop);
  }

  op = pa_context_subscribe(m_context, PA_SUBSCRIPTION_MASK_SINK, simple_callback, this);
  wait_loop(op, m_mainloop);
  pa_context_set_subscribe_callback(m_context, subscribe_callback, this);

  pa_threaded_mainloop_unlock(m_mainloop);

}

/**
 * Deconstruct pulseaudio
 */
pulseaudio::~pulseaudio() {
  pa_threaded_mainloop_stop(m_mainloop);
  pa_context_disconnect(m_context);
  pa_context_unref(m_context);
  pa_threaded_mainloop_free(m_mainloop);

}

/**
 * Get sink name
 */
const string& pulseaudio::get_name() {
  return spec_s_name;
}

/**
 * Wait for events (timeout in ms)
 */
bool pulseaudio::wait(int timeout) {
  // TODO wait for specified timeout
  (void) timeout;
  return m_events.size() > 0;
}

/**
 * Process queued pulseaudio events
 */
int pulseaudio::process_events() {
  int ret = m_events.size();
  pa_threaded_mainloop_lock(m_mainloop);
  pa_operation *o{nullptr};
  // clear the queue
  while (!m_events.empty()) {
    switch (m_events.front()) {
      // try to get specified sink
      case evtype::NEW:
        // redundant if already using specified sink
        o = pa_context_get_sink_info_by_name(m_context, spec_s_name.c_str(), sink_info_callback, this);
        wait_loop(o, m_mainloop);
        break;
      // get volume
      case evtype::CHANGE:
        o = pa_context_get_sink_info_by_index(m_context, m_index, get_sink_volume_callback, this);
        wait_loop(o, m_mainloop);
        break;
      // get default sink 
      case evtype::REMOVE:
        o = pa_context_get_server_info(m_context, get_default_sink_callback, this);
        wait_loop(o, m_mainloop);
        o = pa_context_get_sink_info_by_name(m_context, def_s_name.c_str(), sink_info_callback, this);
        wait_loop(o, m_mainloop);
        break;
    }
    m_events.pop();
  }
  pa_threaded_mainloop_unlock(m_mainloop);
  return ret;
}

/**
 * Get volume in percentage
 */
int pulseaudio::get_volume() {
  pa_threaded_mainloop_lock(m_mainloop);
  pa_operation *op = pa_context_get_sink_info_by_index(m_context, m_index, get_sink_volume_callback, this);
  wait_loop(op, m_mainloop);
  pa_threaded_mainloop_unlock(m_mainloop);
  // alternatively, user pa_cvolume_avg_mask() to average selected channels
  return static_cast<int>(pa_cvolume_max(&cv) * 100.0f / PA_VOLUME_NORM + 0.5f);
}

/**
 * Set volume to given percentage
 */
void pulseaudio::set_volume(float percentage) {
  pa_threaded_mainloop_lock(m_mainloop);
  pa_operation *op = pa_context_get_sink_info_by_index(m_context, m_index, get_sink_volume_callback, this);
  wait_loop(op, m_mainloop);
  pa_volume_t vol = math_util::percentage_to_value<pa_volume_t>(percentage, PA_VOLUME_MUTED, PA_VOLUME_NORM);
  pa_cvolume_scale(&cv, vol);
  op = pa_context_set_sink_volume_by_index(m_context, m_index, &cv, simple_callback, this);
  wait_loop(op, m_mainloop);
  pa_threaded_mainloop_unlock(m_mainloop);
}

/**
 * Increment or decrement volume by given percentage (prevents accumulation of rounding errors from get_volume)
 */
void pulseaudio::inc_volume(int delta_perc) {
  // set max value?
  pa_threaded_mainloop_lock(m_mainloop);
  pa_operation *op = pa_context_get_sink_info_by_index(m_context, m_index, get_sink_volume_callback, this);
  wait_loop(op, m_mainloop);
  pa_volume_t vol = math_util::percentage_to_value<pa_volume_t>(abs(delta_perc), PA_VOLUME_NORM);
  if (delta_perc > 0)
    pa_cvolume_inc(&cv, vol);
  else
    pa_cvolume_dec(&cv, vol);
  op = pa_context_set_sink_volume_by_index(m_context, m_index, &cv, simple_callback, this);
  wait_loop(op, m_mainloop);
  pa_threaded_mainloop_unlock(m_mainloop);
}

/**
 * Set mute state
 */
void pulseaudio::set_mute(bool mode) {
  pa_threaded_mainloop_lock(m_mainloop);
  pa_operation *op = pa_context_set_sink_mute_by_index(m_context, m_index, mode, simple_callback, this);
  wait_loop(op, m_mainloop);
  pa_threaded_mainloop_unlock(m_mainloop);
}

/**
 * Toggle mute state
 */
void pulseaudio::toggle_mute() {
  set_mute(!is_muted());
}

/**
 * Get current mute state
 */
bool pulseaudio::is_muted() {
  pa_threaded_mainloop_lock(m_mainloop);
  pa_operation *op = pa_context_get_sink_info_by_index(m_context, m_index, check_mute_callback, this);
  wait_loop(op, m_mainloop);
  pa_threaded_mainloop_unlock(m_mainloop);
  return muted;
}

/**
 * Callback when getting current mute state
 */
void pulseaudio::check_mute_callback(pa_context *context, const pa_sink_info *info, int eol, void *userdata) {
  if (eol < 0) {
    throw pulseaudio_error("Failed to get sink information: " + string{pa_strerror(pa_context_errno(context))});
  }
  if (eol)
    return;
  pulseaudio* This = static_cast<pulseaudio *>(userdata);
  if (info)
    This->muted = info->mute;
  pa_threaded_mainloop_signal(This->m_mainloop, 0);
}

/**
 * Callback when getting volume
 */
void pulseaudio::get_sink_volume_callback(pa_context *context, const pa_sink_info *info, int eol, void *userdata) {
  if (eol < 0) {
    throw pulseaudio_error("Failed to get sink information: " + string{pa_strerror(pa_context_errno(context))});
  }
  if (eol)
    return;
  //pa_assert(info);
  pulseaudio* This = static_cast<pulseaudio *>(userdata);
  if (info)
    This->cv = info->volume;
  pa_threaded_mainloop_signal(This->m_mainloop, 0);
}

/**
 * Callback when subscribing to changes
 */
void pulseaudio::subscribe_callback(pa_context* context, pa_subscription_event_type_t t, uint32_t idx, void* userdata) {
  pulseaudio *This = static_cast<pulseaudio *>(userdata);
  if (idx == PA_INVALID_INDEX)
    throw pulseaudio_error("Invalid index given: " + string{pa_strerror(pa_context_errno(context))});
  switch(t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
    case PA_SUBSCRIPTION_EVENT_SINK:
      switch(t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) {
        case PA_SUBSCRIPTION_EVENT_NEW:
	  // if using the default sink, check if the new sink matches our specified sink
          //if (This->s_name == This->def_s_name && This->spec_s_name != This->def_s_name) {
	  printf("NEW\n");
            This->m_events.emplace(evtype::NEW);
          //}
	  break;
        case PA_SUBSCRIPTION_EVENT_CHANGE:
          if (idx == This->m_index) {
	  printf("CHANGE\n");
            This->m_events.emplace(evtype::CHANGE);
          }
          break;
        case PA_SUBSCRIPTION_EVENT_REMOVE:
          if (idx == This->m_index) {
	  printf("REMOVE\n");
            This->m_events.emplace(evtype::REMOVE);
          }
          break;
      }
      break;
  }
  pa_threaded_mainloop_signal(This->m_mainloop, 0);
}

/**
 * Simple callback to check for success
 */
void pulseaudio::simple_callback(pa_context *context, int success, void *userdata) {
  if (!success)
    throw pulseaudio_error("Something failed: %s" + string{pa_strerror(pa_context_errno(context))});
  pulseaudio *This = static_cast<pulseaudio *>(userdata);
  pa_threaded_mainloop_signal(This->m_mainloop, 0);
}

/**
 * Callback when getting default sink name
 */
void pulseaudio::get_default_sink_callback(pa_context *context, const pa_server_info *info, void *userdata) {
  pulseaudio *This = static_cast<pulseaudio *>(userdata);
  if (!info) {
    throw pulseaudio_error("Failed to get server information: %s" + string{pa_strerror(pa_context_errno(context))});
  } else {
    This->def_s_name = info->default_sink_name; 
  }
  pa_threaded_mainloop_signal(This->m_mainloop, 0);
}

/**
 * Callback when getting sink info & existence
 */
void pulseaudio::sink_info_callback(pa_context *context, const pa_sink_info *info, int eol, void *userdata) {
  (void) context;
  pulseaudio *This = static_cast<pulseaudio *>(userdata);
  if (eol || !info) {
    This->exists = false;
  } else {
    This->exists = true;
    This->m_index = info->index;
  }
  pa_threaded_mainloop_signal(This->m_mainloop, 0);
}

/**
 * Callback when context state changes
 */
void pulseaudio::context_state_callback(pa_context *context, void *userdata) {
  pulseaudio* This = static_cast<pulseaudio *>(userdata);
  switch (pa_context_get_state(context)) {
    case PA_CONTEXT_READY:
    case PA_CONTEXT_TERMINATED:
    case PA_CONTEXT_FAILED:
      pa_threaded_mainloop_signal(This->m_mainloop, 0);
      break;

    case PA_CONTEXT_UNCONNECTED:
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
      break;
  }
}

inline void pulseaudio::wait_loop(pa_operation *op, pa_threaded_mainloop *loop) {
  while (pa_operation_get_state(op) != PA_OPERATION_DONE)
    pa_threaded_mainloop_wait(loop);
  pa_operation_unref(op);
}

POLYBAR_NS_END
