#include <switch.h>
#include <switch_types.h>

typedef struct {
	switch_core_session_t *session;
	switch_codec_implementation_t *read_impl;
	switch_media_bug_t *read_bug;
	switch_audio_resampler_t *read_resampler;

	int talking;
	int talked;
	int talk_hits;
	int listen_hits;
	int hangover;
	int hangover_len;
	int divisor;
	int thresh;
	int channels;
	int sample_rate;
	int debug;
	int _hangover_len;
	int _thresh;
	int _listen_hits;
	switch_vad_state_t vad_state;
	switch_vad_t *svad;
} wavin_vad_t;

#define VAD_MODULE_DESC "module voice activity detection(vad)"

#define VAD_PRIVATE "_vad_"
#define VAD_EVENT_SUBCLASS "vad::detection"

SWITCH_MODULE_LOAD_FUNCTION(mod_vad_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_vad_shutdown);
SWITCH_MODULE_DEFINITION(mod_vad, mod_vad_load, mod_vad_shutdown, NULL);

// define vad functions
SWITCH_STANDARD_APP(vad_app_function);
static switch_bool_t fire_vad_event(switch_core_session_t *session, switch_vad_state_t vad_state);
SWITCH_DECLARE(const char *) get_vad_state(switch_vad_state_t state);
static switch_bool_t vad_audio_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type);
SWITCH_STANDARD_API(vad_api_function);

#define VAD_SYNTAX "<start|stop> <uuid>"
SWITCH_STANDARD_API(vad_api_function)
{
	switch_core_session_t *target_session;

	char *lbuf = NULL, *argv[2];
	int argc = 0;

	if (!zstr(cmd) && (lbuf = strdup(cmd)) &&
		(argc = switch_separate_string(lbuf, ' ', argv, (sizeof(argv) / sizeof(argv[0])))) == 2) {
		if ((target_session = switch_core_session_locate(argv[1]))) {
			vad_app_function(target_session, argv[0]);
			switch_core_session_rwunlock(target_session);
		}
	} else {
		stream->write_function(stream, "-USAGE: %s\n", VAD_SYNTAX);
		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_SUCCESS;
}
// implements vad functions
SWITCH_STANDARD_APP(vad_app_function)
{
	switch_status_t status;
	wavin_vad_t *s_vad = NULL;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_codec_implementation_t imp = {0};
	int flags = 0;
	int mode = -1;
	const char *var = NULL;
	int tmp;

	if (!zstr(data)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "VAD input parameter %s\n", data);
	}

	if ((var = switch_channel_get_variable(channel, "vad_mode"))) {
		mode = atoi(var);
		if (mode > 3) mode = 3;
	}

	if (mode == -1) { mode = 0; }

	if ((s_vad = (wavin_vad_t *)switch_channel_get_private(channel, VAD_PRIVATE))) {
		if (!zstr(data) && !strcasecmp(data, "stop")) {
			switch_channel_set_private(channel, VAD_PRIVATE, NULL);
			if (s_vad->read_bug) {
				switch_core_media_bug_remove(session, &s_vad->read_bug);
				s_vad->read_bug = NULL;
				switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
			}
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Stopped VAD detection\n");
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
							  "Cannot run vad detection 2 times on the same session!\n");
		}
		return;
	}

	s_vad = switch_core_session_alloc(session, sizeof(*s_vad));
	switch_assert(s_vad);
	memset(s_vad, 0, sizeof(*s_vad));
	s_vad->session = session;

	switch_core_session_raw_read(session);
	switch_core_session_get_read_impl(session, &imp);
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Read imp sample:[%u] channels:[%u].\n",
					  imp.samples_per_second, imp.number_of_channels);
	s_vad->sample_rate = imp.samples_per_second ? imp.samples_per_second : 8000;
	s_vad->channels = imp.number_of_channels;

	s_vad->svad = switch_vad_init(s_vad->sample_rate, s_vad->channels);
	switch_assert(s_vad->svad);

	switch_vad_set_mode(s_vad->svad, mode);

	if ((var = switch_channel_get_variable(channel, "vad_debug"))) {
		tmp = atoi(var);

		if (tmp < 0) tmp = 0;
		if (tmp > 1) tmp = 1;

		switch_vad_set_param(s_vad->svad, "debug", tmp);
	}

	if ((var = switch_channel_get_variable(channel, "vad_silence_ms"))) {
		tmp = atoi(var);

		if (tmp > 0) switch_vad_set_param(s_vad->svad, "sicence_ms", tmp);
	}

	if ((var = switch_channel_get_variable(channel, "vad_thresh"))) {
		tmp = atoi(var);

		if (tmp > 0) switch_vad_set_param(s_vad->svad, "thresh", tmp);
	}

	if ((var = switch_channel_get_variable(channel, "vad_voice_ms"))) {
		tmp = atoi(var);

		if (tmp > 0) switch_vad_set_param(s_vad->svad, "voice_ms", tmp);
	}

	flags = SMBF_READ_REPLACE | SMBF_ANSWER_REQ;
	status =
		switch_core_media_bug_add(session, "vad_read", NULL, vad_audio_callback, s_vad, 0, flags, &s_vad->read_bug);

	if (status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "Failed to attach vad to media stream!\n");
		return;
	}

	switch_channel_set_private(channel, VAD_PRIVATE, s_vad);
}

SWITCH_MODULE_LOAD_FUNCTION(mod_vad_load)
{
	switch_application_interface_t *app_interface;
	switch_api_interface_t *api_interface;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "mod_vad loading...\n");

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	SWITCH_ADD_APP(app_interface, "vad", "voice activity detection(vad)", VAD_MODULE_DESC, vad_app_function,
				   "<start|stop>", SAF_MEDIA_TAP);

	SWITCH_ADD_API(api_interface, "uuid_vad", "voice activity detection(vad)", vad_api_function, VAD_SYNTAX);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "mod_vad loaded successful...\n");
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_vad_shutdown)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "mod_vad shutdown...\n");
	return SWITCH_STATUS_SUCCESS;
}

// ·¢ËÍÊÂ¼þ
static switch_bool_t fire_vad_event(switch_core_session_t *session, switch_vad_state_t vad_state)
{
	switch_event_t *event = NULL;
	switch_channel_t *channel = switch_core_session_get_channel(session);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Fire VAD event %s\n",
					  get_vad_state(vad_state));
	switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, VAD_EVENT_SUBCLASS);
	if (event) {
		switch (vad_state) {
		case SWITCH_VAD_STATE_START_TALKING:
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "vad_state", "start_talking");
			break;
		case SWITCH_VAD_STATE_STOP_TALKING:
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "vad_state", "stop_talking");
			break;
		default:
			break;
		}
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "vender", "wavin");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "product_name", "vad");
		switch_channel_event_set_data(channel, event);
		switch_event_fire(&event);
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "Failed to fire VAD Complete event %d\n", vad_state);
	}
	switch_event_destroy(&event);
	return SWITCH_TRUE;
}

SWITCH_DECLARE(const char *) get_vad_state(switch_vad_state_t state)
{
	switch (state) {
	case SWITCH_VAD_STATE_NONE:
		return "none";
	case SWITCH_VAD_STATE_START_TALKING:
		return "start_talking";
	case SWITCH_VAD_STATE_TALKING:
		return "talking";
	case SWITCH_VAD_STATE_STOP_TALKING:
		return "stop_talking";
	default:
		return "error";
	}
}

static switch_bool_t vad_audio_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	wavin_vad_t *vad = (wavin_vad_t *)user_data;
	switch_core_session_t *session = vad->session;
	switch_vad_state_t vad_state;
	switch_frame_t *linear_frame;
	uint32_t linear_len = 0;
	switch_channel_t *channel = switch_core_session_get_channel(session);

	if (!switch_channel_ready(channel)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Channel isn't ready\n");
		return SWITCH_FALSE;
	}

	if (!switch_channel_media_ready(channel)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Channel codec isn't ready\n");
		return SWITCH_FALSE;
	}

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
						  "Starting VAD detection for audio stream\n");
		break;
	case SWITCH_ABC_TYPE_CLOSE:
		if (vad->read_resampler) { switch_resample_destroy(&vad->read_resampler); }

		if (vad->svad) {
			switch_vad_destroy(&vad->svad);
			vad->svad = NULL;
		}

		switch_core_media_bug_flush(bug);
		switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
						  "Stopping VAD detection for audio stream\n");
		break;
	case SWITCH_ABC_TYPE_READ:
	case SWITCH_ABC_TYPE_READ_REPLACE:
		linear_frame = switch_core_media_bug_get_read_replace_frame(bug);
		linear_len = linear_frame->datalen;

		vad_state = switch_vad_process(vad->svad, linear_frame->data, linear_len / 2);
		if (vad_state == SWITCH_VAD_STATE_START_TALKING) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "START TALKING\n");
			fire_vad_event(session, vad_state);
		} else if (vad_state == SWITCH_VAD_STATE_STOP_TALKING) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "STOP TALKING\n");
			fire_vad_event(session, vad_state);
			switch_vad_reset(vad->svad);
		} else if (vad_state == SWITCH_VAD_STATE_TALKING) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "State - TALKING\n");
		}
		break;
	default:
		break;
	}

	return SWITCH_TRUE;
}