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
 * mod_spidermonkey_DB.c -- DB Javascript Module
 *
 */
#include "mod_spidermonkey.h"

static const char modname[] = "coreDB";

struct db_obj {
    switch_memory_pool_t *pool;
    switch_core_db_t *db;
    switch_core_db_stmt_t *stmt;
    char *dbname;
    char code_buffer[2048];
    JSContext *cx;
    JSObject *obj;
};


/* DB Object */
/*********************************************************************************/
static JSBool db_construct(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
	switch_memory_pool_t *pool;
	switch_core_db_t *db;
	struct db_obj *dbo;

	if (argc > 0) {
		char *dbname = JS_GetStringBytes(JS_ValueToString(cx, argv[0]));
		switch_core_new_memory_pool(&pool);
		if (! (db = switch_core_db_open_file(dbname))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Cannot Open DB!\n");
			switch_core_destroy_memory_pool(&pool);
			return JS_FALSE;
		}
		dbo = switch_core_alloc(pool, sizeof(*dbo));
		dbo->pool = pool;
		dbo->dbname = switch_core_strdup(pool, dbname);
		dbo->cx = cx;
		dbo->obj = obj;
		dbo->db = db;
		JS_SetPrivate(cx, obj, dbo);
		return JS_TRUE;
	}

	return JS_FALSE;
}

static void db_destroy(JSContext *cx, JSObject *obj)
{
	struct db_obj *dbo = JS_GetPrivate(cx, obj);
	
	if (dbo) {
		switch_memory_pool_t *pool = dbo->pool;
		if (dbo->stmt) {
			switch_core_db_finalize(dbo->stmt);
			dbo->stmt = NULL;
		}
		switch_core_db_close(dbo->db);
		switch_core_destroy_memory_pool(&pool);
        pool = NULL;
	}
}


static int db_callback(void *pArg, int argc, char **argv, char **columnNames)
{
	struct db_obj *dbo = pArg;
	char code[1024];
	jsval rval;
	int x = 0;

	snprintf(code, sizeof(code), "~var _Db_RoW_ = {}");
	eval_some_js(code, dbo->cx, dbo->obj, &rval);

	for(x=0; x < argc; x++) {
		snprintf(code, sizeof(code), "~_Db_RoW_[\"%s\"] = \"%s\"", columnNames[x], argv[x]);
		eval_some_js(code, dbo->cx, dbo->obj, &rval);
	}

	snprintf(code, sizeof(code), "~%s(_Db_RoW_)", dbo->code_buffer);
	eval_some_js(code, dbo->cx, dbo->obj, &rval);

	snprintf(code, sizeof(code), "~delete _Db_RoW_");
	eval_some_js(code, dbo->cx, dbo->obj, &rval);
	
	return 0;
}

static JSBool db_exec(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    struct db_obj *dbo = JS_GetPrivate(cx, obj);
    *rval = BOOLEAN_TO_JSVAL( JS_TRUE );

    if (argc > 0) {
        char *sql = JS_GetStringBytes(JS_ValueToString(cx, argv[0]));
        char *err = NULL;
        void *arg = NULL;
        switch_core_db_callback_func_t cb_func = NULL;


        if (argc > 1) {
            char *js_func = JS_GetStringBytes(JS_ValueToString(cx, argv[1]));
            switch_copy_string(dbo->code_buffer, js_func, sizeof(dbo->code_buffer));
            cb_func = db_callback;
            arg = dbo;
        }

        switch_core_db_exec(dbo->db, sql, cb_func, arg, &err);
        if (err) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error %s\n", err);
            switch_core_db_free(err);
            *rval = BOOLEAN_TO_JSVAL( JS_FALSE );
        }
    }
    return JS_TRUE;
}


static JSBool db_next(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
	struct db_obj *dbo = JS_GetPrivate(cx, obj);
	*rval = BOOLEAN_TO_JSVAL( JS_FALSE );

    if (dbo->stmt) {
		int running = 1;
		while (running < 5000) {
			int result = switch_core_db_step(dbo->stmt);
			if (result == SQLITE_ROW) {
				*rval = BOOLEAN_TO_JSVAL( JS_TRUE );	
				break;
			} else if (result == SQLITE_BUSY) {
				running++;
				continue;
			}
			switch_core_db_finalize(dbo->stmt);
			dbo->stmt = NULL;
			break;
		}
	}

	return JS_TRUE;
}

static JSBool db_fetch(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
	struct db_obj *dbo = JS_GetPrivate(cx, obj);
	int colcount = switch_core_db_column_count(dbo->stmt);
	char code[1024];
	int x;

	snprintf(code, sizeof(code), "~var _dB_RoW_DaTa_ = {}");
	eval_some_js(code, dbo->cx, dbo->obj, rval);
	if (*rval == JS_FALSE) {
		return JS_TRUE; 
	}
	for (x = 0; x < colcount; x++) {
		snprintf(code, sizeof(code), "~_dB_RoW_DaTa_[\"%s\"] = \"%s\"", 
				 (char *) switch_core_db_column_name(dbo->stmt, x),
				 (char *) switch_core_db_column_text(dbo->stmt, x));

		eval_some_js(code, dbo->cx, dbo->obj, rval);
		if (*rval == JS_FALSE) {
			return JS_TRUE; 
		}
	}

	JS_GetProperty(cx, obj, "_dB_RoW_DaTa_", rval);

	return JS_TRUE;
}


static JSBool db_prepare(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
	struct db_obj *dbo = JS_GetPrivate(cx, obj);

	*rval = BOOLEAN_TO_JSVAL( JS_FALSE );	

	if (dbo->stmt) {
		switch_core_db_finalize(dbo->stmt);
		dbo->stmt = NULL;
	}

	if (argc > 0) {
		char *sql = JS_GetStringBytes(JS_ValueToString(cx, argv[0]));
		if(switch_core_db_prepare(dbo->db, sql, -1, &dbo->stmt, 0)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error %s\n", switch_core_db_errmsg(dbo->db));
		} else {
			*rval = BOOLEAN_TO_JSVAL( JS_TRUE );
		}
	}
	return JS_TRUE;
}

enum db_tinyid {
	DB_NAME
};

static JSFunctionSpec db_methods[] = {
	{"exec", db_exec, 1},
	{"next", db_next, 0},
	{"fetch", db_fetch, 1},
	{"prepare", db_prepare, 0},
	{0}
};


static JSPropertySpec db_props[] = {
	{"path", DB_NAME, JSPROP_READONLY|JSPROP_PERMANENT}, 
	{0}
};


static JSBool db_getProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
	JSBool res = JS_TRUE;
	struct db_obj *dbo = JS_GetPrivate(cx, obj);
	char *name;
	int param = 0;
	
	name = JS_GetStringBytes(JS_ValueToString(cx, id));
    /* numbers are our props anything else is a method */
    if (name[0] >= 48 && name[0] <= 57) {
        param = atoi(name);
    } else {
        return JS_TRUE;
    }
	
	switch(param) {
	case DB_NAME:
		*vp = STRING_TO_JSVAL(JS_NewStringCopyZ(cx, dbo->dbname));
		break;
	}

	return res;
}

JSClass db_class = {
	modname, JSCLASS_HAS_PRIVATE, 
	JS_PropertyStub,  JS_PropertyStub,	db_getProperty,  JS_PropertyStub, 
	JS_EnumerateStub, JS_ResolveStub,	JS_ConvertStub,	  db_destroy, NULL, NULL, NULL,
	db_construct
};


switch_status_t db_load(JSContext *cx, JSObject *obj)
{

	JS_InitClass(cx,
				 obj,
				 NULL,
				 &db_class,
				 db_construct,
				 3,
				 db_props,
				 db_methods,
				 db_props,
				 db_methods
				 );
	return SWITCH_STATUS_SUCCESS;
}


const sm_module_interface_t DB_module_interface = {
	/*.name = */ modname,
	/*.spidermonkey_load*/ db_load,
	/*.next*/ NULL
};

SWITCH_MOD_DECLARE(switch_status_t) spidermonkey_init(const sm_module_interface_t **module_interface)
{
	*module_interface = &DB_module_interface;
	return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:nil
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 expandtab:
 */
