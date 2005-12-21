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
 * switch_event.c -- Event System
 *
 */
#include <switch_event.h>

static switch_event *EVENT_QUEUE_HEAD;
static switch_event *EVENT_QUEUE_WORK;
static switch_thread_cond_t *COND;
static switch_event_node *EVENT_NODES[SWITCH_EVENT_ALL+1] = {NULL};
static switch_mutex_t *BLOCK = NULL;
static switch_mutex_t *QLOCK = NULL;
static switch_memory_pool *EPOOL = NULL;
static switch_hash *CUSTOM_HASH = NULL;
static int THREAD_RUNNING = 0;

/* make sure this is synced with the switch_event_t enum in switch_types.h
   also never put any new ones before EVENT_ALL
*/
static char *EVENT_NAMES[] = {
	"CUSTOM",
	"INBOUND_CHAN",
	"OUTBOUND_CHAN",
	"ANSWER_CHAN",
	"HANGUP_CHAN",
	"STARTUP",
	"EVENT_SHUTDOWN",
	"SHUTDOWN",
	"ALL"
};


#if 0
static void debug_hash(void) {
	switch_hash_index_t* hi;
	void *val;
	const void *var;
	for (hi = switch_hash_first(EPOOL, CUSTOM_HASH); hi; hi = switch_hash_next(hi)) {
		switch_event_subclass *subclass;
		switch_hash_this(hi, &var, NULL, &val);
		subclass = val;
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "***WTF %s=%s\n", (char *) var, subclass->name);
	}
}
#endif



static int switch_events_match(switch_event *event, switch_event_node *node)
{
	int match = 0;

	
	if (node->event_id == SWITCH_EVENT_ALL) {
		match++;

		if (!node->subclass) {
			return match;
		}
	}

	if (match || event->event_id == node->event_id) {
		if (event->subclass && node->subclass) {
			if (!strncasecmp(node->subclass->name, "file:", 5)) {
				char *file_header;
				if ((file_header = switch_event_get_header(event, "file"))) {
					match = strstr(node->subclass->name + 5, file_header) ? 1 : 0;
				}
			} else if (!strncasecmp(node->subclass->name, "func:", 5)) {
				char *func_header;
				if ((func_header = switch_event_get_header(event, "function"))) {
					match = strstr(node->subclass->name + 5, func_header) ? 1 : 0;
				}
			} else {
				match = strstr(event->subclass->name, node->subclass->name) ? 1 : 0;
			}
		} else if (event->subclass && !node->subclass) {
			match = 1;
		} else {
			match = 0;
		}
	}

	return match;
}

static void * SWITCH_THREAD_FUNC switch_event_thread(switch_thread *thread, void *obj) 
{
	switch_event_node *node;
	switch_event *event = NULL, *out_event = NULL;
	switch_event_t e;
	switch_mutex_t *mutex = NULL;

	switch_mutex_init(&mutex, SWITCH_MUTEX_NESTED, EPOOL);
	switch_thread_cond_create(&COND, EPOOL);	
	switch_mutex_lock(mutex);

	assert(QLOCK != NULL);
	assert(EPOOL != NULL);

	THREAD_RUNNING = 1;
	while(THREAD_RUNNING == 1) {
		switch_thread_cond_wait(COND, mutex);
		switch_mutex_lock(QLOCK);
		/* <LOCKED> -----------------------------------------------*/
		EVENT_QUEUE_WORK = EVENT_QUEUE_HEAD;
		EVENT_QUEUE_HEAD = NULL;
		switch_mutex_unlock(QLOCK);
		/* </LOCKED> -----------------------------------------------*/

		for(event = EVENT_QUEUE_WORK; event;) {
			out_event = event;
			event = event->next;
			out_event->next = NULL;
			for(e = out_event->event_id;; e = SWITCH_EVENT_ALL) {
				for(node = EVENT_NODES[e]; node; node = node->next) {
					if (switch_events_match(out_event, node)) {
						out_event->bind_user_data = node->user_data;
						node->callback(out_event);
					}
				}
		
				if (e == SWITCH_EVENT_ALL) {
					break;
				}
			}

			switch_event_destroy(&out_event);
		}

	
	}
	THREAD_RUNNING = 0;
	return NULL;
}



SWITCH_DECLARE(char *) switch_event_name(switch_event_t event)
{
	assert(BLOCK != NULL);
	assert(EPOOL != NULL);

	return EVENT_NAMES[event];
}

SWITCH_DECLARE(switch_status) switch_event_reserve_subclass_detailed(char *owner, char *subclass_name)
{

	switch_event_subclass *subclass;

	assert(EPOOL != NULL);
	assert(CUSTOM_HASH != NULL);

	if (switch_core_hash_find(CUSTOM_HASH, subclass_name)) {
		return SWITCH_STATUS_INUSE;
	}

	if (!(subclass = switch_core_alloc(EPOOL, sizeof(*subclass)))) {
		return SWITCH_STATUS_MEMERR;
	}

	subclass->owner = switch_core_strdup(EPOOL, owner);
	subclass->name = switch_core_strdup(EPOOL, subclass_name);
	
	switch_core_hash_insert(CUSTOM_HASH, subclass->name, subclass);
	
	return SWITCH_STATUS_SUCCESS;

}

SWITCH_DECLARE(switch_status) switch_event_shutdown(void)
{
	switch_event *event;
	THREAD_RUNNING = -1;

	if (switch_event_create(&event, SWITCH_EVENT_EVENT_SHUTDOWN) == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header(event, "event_info", "Event System Shutting Down");
		switch_event_fire(&event);
	}
	while(THREAD_RUNNING) {
		switch_yield(1000);
	}
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status) switch_event_init(switch_memory_pool *pool)
{
    switch_thread *thread;
    switch_threadattr_t *thd_attr;;
    switch_threadattr_create(&thd_attr, pool);
    switch_threadattr_detach_set(thd_attr, 1);

	assert(pool != NULL);
	EPOOL = pool;
	switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Activate Eventing Engine.\n");
	switch_mutex_init(&BLOCK, SWITCH_MUTEX_NESTED, EPOOL);
	switch_mutex_init(&QLOCK, SWITCH_MUTEX_NESTED, EPOOL);
	switch_core_hash_init(&CUSTOM_HASH, EPOOL);
    switch_thread_create(&thread,
						 thd_attr,
						 switch_event_thread,
						 NULL,
						 EPOOL
						 );

	while(!THREAD_RUNNING) {
		switch_yield(1000);
	}
	return SWITCH_STATUS_SUCCESS;

}

SWITCH_DECLARE(switch_status) switch_event_create_subclass(switch_event **event, switch_event_t event_id, char *subclass_name)
{

	if (event_id != SWITCH_EVENT_CUSTOM && subclass_name) {
		return SWITCH_STATUS_GENERR;
	}

	if(!(*event = malloc(sizeof(switch_event)))) {
		return SWITCH_STATUS_MEMERR;
	}

	memset(*event, 0, sizeof(switch_event));

	(*event)->event_id = event_id;

	if (subclass_name) {
		(*event)->subclass = switch_core_hash_find(CUSTOM_HASH, subclass_name);
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(char *) switch_event_get_header(switch_event *event, char *header_name)
{
	switch_event_header *hp;
	if (header_name) {
		for(hp = event->headers; hp; hp = hp->next) {
			if (!strcasecmp(hp->name, header_name)) {
				return hp->value;
			}
		}
	}
	return NULL;
}

SWITCH_DECLARE(switch_status) switch_event_add_header(switch_event *event, char *header_name, char *fmt, ...)
{
    char *data;
    int ret = 0;
    va_list ap;

	va_start(ap, fmt);
	
#ifdef HAVE_VASPRINTF
	ret = vasprintf(&data, fmt, ap);
#else
	data = (char *) malloc(2048);
	vsnprintf(data, 2048, fmt, ap);
#endif
	va_end(ap);
	if (ret == -1) {
		return SWITCH_STATUS_MEMERR;
	} else {
		switch_event_header *header, *hp;
		

		if (!(header = malloc(sizeof(*header)))) {
			return SWITCH_STATUS_MEMERR;
		}
		memset(header, 0, sizeof(*header));
		header->name = strdup(header_name);
		header->value = data;

		for (hp = event->headers; hp && hp->next; hp = hp->next);
		
		if (hp) {
			hp->next = header;
		} else {
			event->headers = header;
		}
		
		return SWITCH_STATUS_SUCCESS;

	}
	
	return (ret >= 0) ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_GENERR;
	
}

SWITCH_DECLARE(void) switch_event_destroy(switch_event **event)
{
	switch_event_header *hp, *tofree;

	for (hp = (*event)->headers; hp && hp->next;) {
		tofree = hp;
		hp = hp->next;
		free(tofree->name);
		free(tofree->value);
		free(tofree);
	}

	free((*event));
	*event = NULL;
}

SWITCH_DECLARE(switch_status) switch_event_dup(switch_event **event, switch_event *todup)
{
	switch_event_header *header, *hp, *hp2;

	if (switch_event_create_subclass(event, todup->event_id, todup->subclass->name) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_GENERR;
	}
	
	(*event)->subclass = todup->subclass;
	(*event)->event_user_data = todup->event_user_data;
	(*event)->bind_user_data = todup->bind_user_data;
	
	for (hp = todup->headers; hp && hp->next;) {
		if (!(header = malloc(sizeof(*header)))) {
			return SWITCH_STATUS_MEMERR;
		}
		memset(header, 0, sizeof(*header));
		header->name = strdup(hp->name);
		header->value = strdup(hp->value);

		for (hp2 = todup->headers; hp2 && hp2->next; hp2 = hp2->next);

		if (hp2) {
			hp2->next = header;
		} else {
			(*event)->headers = header;
		}
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status) switch_event_serialize(switch_event *event, char *buf, size_t buflen, char *fmt, ...)
{
	size_t len;
	switch_event_header *hp;
    char *data = NULL;
    int ret = 0;
    va_list ap;

	if (fmt) {
		va_start(ap, fmt);
#ifdef HAVE_VASPRINTF
		ret = vasprintf(&data, fmt, ap);
#else
		data = (char *) malloc(2048);
		vsnprintf(data, 2048, fmt, ap);
#endif
		va_end(ap);
		if (ret == -1) {
			return SWITCH_STATUS_MEMERR;
		}
	}

	snprintf(buf, buflen, "name: %s\n", switch_event_name(event->event_id));
	len = strlen(buf);
	if (event->subclass) {
		snprintf(buf+len, buflen-len, "subclass-name: %s\nowner: %s", event->subclass->name, event->subclass->owner);
	}
	len = strlen(buf);
	for (hp = event->headers; hp; hp = hp->next) {
		snprintf(buf+len, buflen-len, "%s: %s\n", hp->name, hp->value);
		len = strlen(buf);
	}
	if (data) {
		snprintf(buf+len, buflen-len, "Content-Length: %d\n\n%s", (int)strlen(data), data);
		free(data);
	} else {
		snprintf(buf+len, buflen-len, "\n");
	}
	
	return SWITCH_STATUS_SUCCESS;
}


SWITCH_DECLARE(switch_status) switch_event_fire_detailed(char *file, char *func, int line, switch_event **event, void *user_data)
{

	switch_event *ep;

	assert(BLOCK != NULL);
	assert(EPOOL != NULL);

	switch_event_add_header(*event, "file", file);
	switch_event_add_header(*event, "function", func);
	switch_event_add_header(*event, "line_number", "%d", line);
	
	if (user_data) {
		(*event)->event_user_data = user_data;
	}
	
	switch_mutex_lock(QLOCK);
	/* <LOCKED> -----------------------------------------------*/
	for(ep = EVENT_QUEUE_HEAD; ep && ep->next; ep = ep->next);

	if (ep) {
		ep->next = *event;
	} else {
		EVENT_QUEUE_HEAD = *event;
	}
	switch_mutex_unlock(QLOCK);
	/* </LOCKED> -----------------------------------------------*/

	*event = NULL;

	switch_thread_cond_signal(COND);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status) switch_event_bind(char *id, switch_event_t event, char *subclass_name, switch_event_callback_t callback, void *user_data)
{
	switch_event_node *event_node;
	switch_event_subclass *subclass = NULL;

	assert(BLOCK != NULL);
	assert(EPOOL != NULL);

	if (subclass_name) {
		if (!(subclass = switch_core_hash_find(CUSTOM_HASH, subclass_name))) {
			if (!(subclass = switch_core_alloc(EPOOL, sizeof(*subclass)))) {
				return SWITCH_STATUS_MEMERR;
			} else {
				subclass->owner = switch_core_strdup(EPOOL, id);
				subclass->name = switch_core_strdup(EPOOL, subclass_name);
			}
		}
	}

	if (event <= SWITCH_EVENT_ALL && (event_node = switch_core_alloc(EPOOL, sizeof(switch_event_node)))) {
		switch_mutex_lock(BLOCK);
		/* <LOCKED> -----------------------------------------------*/
		event_node->id = switch_core_strdup(EPOOL, id);
		event_node->event_id = event;
		event_node->subclass = subclass;
		event_node->callback = callback;
		event_node->user_data = user_data;

		if (EVENT_NODES[event]) {
			event_node->next = EVENT_NODES[event];
		}

		EVENT_NODES[event] = event_node;
		switch_mutex_unlock(BLOCK);
		/* </LOCKED> -----------------------------------------------*/
		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_MEMERR;
}

