/* 
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005/2006, Anthony Minessale II <anthmct@yahoo.com>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthmct@yahoo.com>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * 
 * Anthony Minessale II <anthmct@yahoo.com>
 *
 *
 * mod_loopback.c -- Loopback Endpoint Module
 *
 */
#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

SWITCH_MODULE_LOAD_FUNCTION(mod_loopback_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_loopback_shutdown);
SWITCH_MODULE_DEFINITION(mod_loopback, mod_loopback_load, mod_loopback_shutdown, NULL);

static switch_endpoint_interface_t *loopback_endpoint_interface = NULL;

static switch_memory_pool_t *module_pool = NULL;


typedef enum {
	TFLAG_LINKED = (1 << 0),
	TFLAG_OUTBOUND = (1 << 1),
	TFLAG_WRITE = (1 << 2),
	TFLAG_CNG = (1 << 3)
} TFLAGS;

struct private_object {
	unsigned int flags;
	switch_mutex_t *flag_mutex;
	switch_core_session_t *session;
	switch_core_session_t *other_session;
	struct private_object *other_tech_pvt;
	switch_channel_t *other_channel;
	switch_codec_t read_codec;
	switch_codec_t write_codec;
	switch_frame_t read_frame;
	unsigned char databuf[SWITCH_RECOMMENDED_BUFFER_SIZE];

	switch_frame_t *x_write_frame;
	switch_frame_t write_frame;
	unsigned char write_databuf[SWITCH_RECOMMENDED_BUFFER_SIZE];

	switch_frame_t cng_frame;
	unsigned char cng_databuf[10];

	switch_caller_profile_t *caller_profile;
};
typedef struct private_object private_t;

static struct {
	int debug;
} globals;


static switch_status_t channel_on_init(switch_core_session_t *session);
static switch_status_t channel_on_hangup(switch_core_session_t *session);
static switch_status_t channel_on_routing(switch_core_session_t *session);
static switch_status_t channel_on_exchange_media(switch_core_session_t *session);
static switch_status_t channel_on_soft_execute(switch_core_session_t *session);
static switch_call_cause_t channel_outgoing_channel(switch_core_session_t *session, switch_event_t *var_event,
													switch_caller_profile_t *outbound_profile,
													switch_core_session_t **new_session, switch_memory_pool_t **pool, switch_originate_flag_t flags);
static switch_status_t channel_read_frame(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id);
static switch_status_t channel_write_frame(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id);
static switch_status_t channel_kill_channel(switch_core_session_t *session, int sig);

static switch_status_t tech_init(private_t *tech_pvt, switch_core_session_t *session, switch_codec_t *codec)
{
	const char *iananame = "L16";
	int rate = 8000;
	int interval = 20;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	

	if (codec) {
		iananame = codec->implementation->iananame;
		rate = codec->implementation->actual_samples_per_second;
		interval = codec->implementation->microseconds_per_frame / 1000;
	}
	
	status = switch_core_codec_init(&tech_pvt->read_codec,
									iananame,
									NULL,
									rate,
									interval,
									1, 
									SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
									NULL, 
									switch_core_session_get_pool(session));

	if (status != SWITCH_STATUS_SUCCESS) {
		goto end;
	}

	status = switch_core_codec_init(&tech_pvt->write_codec,
									iananame,
									NULL,
									rate,
									interval,
									1, 
									SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
									NULL, 
									switch_core_session_get_pool(session));
	

	if (status != SWITCH_STATUS_SUCCESS) {
		switch_core_codec_destroy(&tech_pvt->read_codec);
		goto end;
	}

	
	tech_pvt->read_frame.data = tech_pvt->databuf;
	tech_pvt->read_frame.buflen = sizeof(tech_pvt->databuf);
	tech_pvt->read_frame.codec = &tech_pvt->read_codec;


	tech_pvt->write_frame.data = tech_pvt->write_databuf;
	tech_pvt->write_frame.buflen = sizeof(tech_pvt->write_databuf);
	
	tech_pvt->cng_frame.data = tech_pvt->cng_databuf;
	tech_pvt->cng_frame.buflen = sizeof(tech_pvt->cng_databuf);
	switch_set_flag((&tech_pvt->cng_frame), SFF_CNG);
	tech_pvt->cng_frame.datalen = 2;


	switch_core_session_set_read_codec(session, &tech_pvt->read_codec);
	switch_core_session_set_write_codec(session, &tech_pvt->write_codec);
	


	switch_mutex_init(&tech_pvt->flag_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));
	switch_core_session_set_private(session, tech_pvt);
	tech_pvt->session = session;

 end:

	return status;
}




/* 
   State methods they get called when the state changes to the specific state 
   returning SWITCH_STATUS_SUCCESS tells the core to execute the standard state method next
   so if you fully implement the state you can return SWITCH_STATUS_FALSE to skip it.
*/
static switch_status_t channel_on_init(switch_core_session_t *session)
{
	switch_channel_t *channel, *b_channel;
	private_t *tech_pvt = NULL, *b_tech_pvt = NULL;
	switch_core_session_t *b_session;
	char name[128];
	switch_caller_profile_t *caller_profile;

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	if (!switch_test_flag(tech_pvt, TFLAG_OUTBOUND)) {
		
		if (!(b_session = switch_core_session_request(loopback_endpoint_interface, NULL))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "failure.\n");
			goto end;
		}
	
		
		switch_core_session_add_stream(b_session, NULL);
		b_channel = switch_core_session_get_channel(b_session);
		b_tech_pvt = (private_t *) switch_core_session_alloc(b_session, sizeof(*b_tech_pvt));
		if (tech_init(b_tech_pvt, b_session, switch_core_session_get_read_codec(session)) != SWITCH_STATUS_SUCCESS) {
			switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
			switch_core_session_destroy(&b_session);
			goto end;
		}

		switch_snprintf(name, sizeof(name), "Loopback/%s-b", tech_pvt->caller_profile->destination_number);
		switch_channel_set_name(b_channel, name);
		caller_profile = switch_caller_profile_clone(b_session, tech_pvt->caller_profile);
		switch_channel_set_caller_profile(b_channel, caller_profile);
		b_tech_pvt->caller_profile = caller_profile;
		switch_channel_set_state(b_channel, CS_INIT);

		tech_pvt->other_session = b_session;
		tech_pvt->other_tech_pvt = b_tech_pvt;
		tech_pvt->other_channel = b_channel;

		b_tech_pvt->other_session = session;
		b_tech_pvt->other_tech_pvt = tech_pvt;
		b_tech_pvt->other_channel = channel;
		
		switch_set_flag_locked(tech_pvt, TFLAG_LINKED);
		switch_set_flag_locked(b_tech_pvt, TFLAG_LINKED);
		switch_set_flag_locked(b_tech_pvt, TFLAG_OUTBOUND);
	
		switch_channel_set_flag(channel, CF_ACCEPT_CNG);	

		if (switch_core_session_thread_launch(b_session) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Error spawning thread\n");
			switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
			goto end;
		}
	}
	
	if (tech_pvt->other_session) {
		switch_core_session_read_lock(tech_pvt->other_session);
	} else {
		switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
		goto end;
	}

	
	
	//switch_channel_set_flag(tech_pvt->other_channel, CF_ACCEPT_CNG);

	switch_channel_set_state(channel, CS_ROUTING);
	


 end:

	return SWITCH_STATUS_SUCCESS;

}

static switch_status_t channel_on_routing(switch_core_session_t *session)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s CHANNEL ROUTING\n", switch_channel_get_name(channel));

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_execute(switch_core_session_t *session)
{

	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s CHANNEL EXECUTE\n", switch_channel_get_name(channel));


	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_hangup(switch_core_session_t *session)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s CHANNEL HANGUP\n", switch_channel_get_name(channel));

	switch_clear_flag_locked(tech_pvt, TFLAG_LINKED);
	if (tech_pvt->other_tech_pvt) {
		switch_clear_flag_locked(tech_pvt->other_tech_pvt, TFLAG_LINKED);
	}
	
	switch_channel_hangup(tech_pvt->other_channel, switch_channel_get_cause(channel));
	switch_core_session_rwunlock(tech_pvt->other_session);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_kill_channel(switch_core_session_t *session, int sig)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

	switch (sig) {
	case SWITCH_SIG_BREAK:
		switch_set_flag_locked(tech_pvt, TFLAG_CNG);
		if (tech_pvt->other_tech_pvt) {
			switch_set_flag_locked(tech_pvt->other_tech_pvt, TFLAG_CNG);
		}
		break;
	case SWITCH_SIG_KILL:
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_clear_flag_locked(tech_pvt, TFLAG_LINKED);
		if (tech_pvt->other_tech_pvt) {
			switch_clear_flag_locked(tech_pvt->other_tech_pvt, TFLAG_LINKED);
		}
		break;
	default:
		break;
	}
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s CHANNEL KILL\n", switch_channel_get_name(channel));


	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_soft_execute(switch_core_session_t *session)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CHANNEL TRANSMIT\n");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_exchange_media(switch_core_session_t *session)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CHANNEL LOOPBACK\n");

	return SWITCH_STATUS_SUCCESS;
}



static switch_status_t channel_on_consume_media(switch_core_session_t *session)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "CHANNEL CONSUME_MEDIA\n");

	return SWITCH_STATUS_FALSE;
}


static switch_status_t channel_send_dtmf(switch_core_session_t *session, const switch_dtmf_t *dtmf)
{
	private_t *tech_pvt = NULL;

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_read_frame(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;
	switch_status_t status = SWITCH_STATUS_FALSE;
	
	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

	if (!switch_test_flag(tech_pvt, TFLAG_LINKED)) {
		goto end;
	}

	while(tech_pvt->other_tech_pvt && !switch_test_flag(tech_pvt->other_tech_pvt, TFLAG_WRITE)) {
		if (!switch_channel_ready(channel)) {
			goto end;
		}
		if (switch_test_flag(tech_pvt, TFLAG_CNG)) {
			*frame = &tech_pvt->cng_frame;
			status = SWITCH_STATUS_SUCCESS;
			switch_clear_flag_locked(tech_pvt, TFLAG_CNG);
			goto end;
		}
		switch_yield(1000);
	}
	
	if (tech_pvt->other_tech_pvt && switch_test_flag(tech_pvt->other_tech_pvt, TFLAG_WRITE)) {
		*frame = &tech_pvt->other_tech_pvt->write_frame;
		switch_clear_flag_locked(tech_pvt->other_tech_pvt, TFLAG_WRITE);
	}
	
	//printf("READ %s %d\n", switch_channel_get_name(channel), (*frame)->datalen);
	status = SWITCH_STATUS_SUCCESS;
	

 end:

	return status;
}

static switch_status_t channel_write_frame(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id)
{
	switch_channel_t *channel = NULL;
	private_t *tech_pvt = NULL;
	switch_status_t status = SWITCH_STATUS_FALSE;
	
	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

	if (switch_test_flag(tech_pvt, TFLAG_LINKED)) {
		memcpy(&tech_pvt->write_frame, frame, sizeof(*frame));
		tech_pvt->write_frame.data = tech_pvt->write_databuf;
		tech_pvt->write_frame.buflen = sizeof(tech_pvt->write_databuf);
		tech_pvt->write_frame.codec = frame->codec;
		memcpy(tech_pvt->write_frame.data, frame->data, frame->datalen);
		switch_set_flag_locked(tech_pvt, TFLAG_WRITE);
		status = SWITCH_STATUS_SUCCESS;
	}

	return status;
}

static switch_status_t channel_receive_message(switch_core_session_t *session, switch_core_session_message_t *msg)
{
	switch_channel_t *channel;
	private_t *tech_pvt;

	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

	
	switch (msg->message_id) {
	case SWITCH_MESSAGE_INDICATE_ANSWER:
		if (switch_test_flag(tech_pvt, TFLAG_OUTBOUND)) {
			switch_channel_answer(tech_pvt->other_channel);
		}
		break;
	case SWITCH_MESSAGE_INDICATE_PROGRESS:
		if (switch_test_flag(tech_pvt, TFLAG_OUTBOUND)) {
			switch_channel_pre_answer(tech_pvt->other_channel);
		}
		break;
	default:
		break;
	}
	return SWITCH_STATUS_SUCCESS;
}


static switch_call_cause_t channel_outgoing_channel(switch_core_session_t *session, switch_event_t *var_event,
													switch_caller_profile_t *outbound_profile,
													switch_core_session_t **new_session, switch_memory_pool_t **pool, switch_originate_flag_t flags)
{
	if (session) {
		switch_channel_pre_answer(switch_core_session_get_channel(session));
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "You can only use this channel as a B leg\n");
		return SWITCH_CAUSE_FACILITY_REJECTED;
	}

	if ((*new_session = switch_core_session_request(loopback_endpoint_interface, pool)) != 0) {
		private_t *tech_pvt;
		switch_channel_t *channel;
		switch_caller_profile_t *caller_profile;
		
		switch_core_session_add_stream(*new_session, NULL);

		if ((tech_pvt = (private_t *) switch_core_session_alloc(*new_session, sizeof(private_t))) != 0) {
			channel = switch_core_session_get_channel(*new_session);
			if (tech_init(tech_pvt, *new_session, session ? switch_core_session_get_read_codec(session) : NULL) != SWITCH_STATUS_SUCCESS) {
				switch_core_session_destroy(new_session);
				return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
			}
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Hey where is my memory pool?\n");
			switch_core_session_destroy(new_session);
			return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
		}
		
		if (outbound_profile) {
			char name[128];
			char *dialplan = NULL, *context = NULL;

			caller_profile = switch_caller_profile_clone(*new_session, outbound_profile);
			if ((context = strchr(caller_profile->destination_number, '/'))) {
				*context++ = '\0';
				
				if ((dialplan = strchr(context, '/'))) {
					*dialplan++ = '\0';
				}

				if (!switch_strlen_zero(context)) {
					caller_profile->context = switch_core_strdup(caller_profile->pool, context);
				}

				if (!switch_strlen_zero(dialplan)) {
					caller_profile->dialplan = switch_core_strdup(caller_profile->pool, dialplan);
				}
			}
			
			switch_snprintf(name, sizeof(name), "Loopback/%s-a", caller_profile->destination_number);
			switch_channel_set_name(channel, name);
			switch_channel_set_caller_profile(channel, caller_profile);
			tech_pvt->caller_profile = caller_profile;
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Doh! no caller profile\n");
			switch_core_session_destroy(new_session);
			return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
		}
		
		switch_channel_set_state(channel, CS_INIT);
		
		return SWITCH_CAUSE_SUCCESS;
	}

	return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;

}

static switch_state_handler_table_t channel_event_handlers = {
	/*.on_init */ channel_on_init,
	/*.on_routing */ channel_on_routing,
	/*.on_execute */ channel_on_execute,
	/*.on_hangup */ channel_on_hangup,
	/*.on_exchange_media */ channel_on_exchange_media,
	/*.on_soft_execute */ channel_on_soft_execute,
	/*.on_consume_media */ channel_on_consume_media
};

static switch_io_routines_t channel_io_routines = {
	/*.outgoing_channel */ channel_outgoing_channel,
	/*.read_frame */ channel_read_frame,
	/*.write_frame */ channel_write_frame,
	/*.kill_channel */ channel_kill_channel,
	/*.send_dtmf */ channel_send_dtmf,
	/*.receive_message */ channel_receive_message
};



SWITCH_MODULE_LOAD_FUNCTION(mod_loopback_load)
{

	//switch_status_t status;

	if (switch_core_new_memory_pool(&module_pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "OH OH no pool\n");
		return SWITCH_STATUS_TERM;
	}

	memset(&globals, 0, sizeof(globals));

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	loopback_endpoint_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_ENDPOINT_INTERFACE);
	loopback_endpoint_interface->interface_name = "loopback";
	loopback_endpoint_interface->io_routines = &channel_io_routines;
	loopback_endpoint_interface->state_handler = &channel_event_handlers;


	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}


SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_loopback_shutdown)
{
	return SWITCH_STATUS_SUCCESS;
}



/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 expandtab:
 */
