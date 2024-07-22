#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <json-c/json.h>

#define AFB_BINDING_VERSION 4
#include <afb/afb-binding.h>
#include <CANopen/CANopenXchg.h>

/***************
EVENT MANAGEMENT 
****************/

typedef struct event_s event_t;

struct event_s
{
	/* link to next */
	event_t *next;
	/* generated event */
	afb_event_t event;
	/* timer object of the event */
	afb_timer_t timer;
	/* currently used data for requests */
	afb_data_t dreq;
	/* currently used data for values */
	afb_data_t dval;
	/* allocated items for requests */
	canopen_xchg_v1_req_t *requests;
	/* allocated items for values */
	canopen_xchg_v1_value_t *values;
	/* used count */
	unsigned count;
	/* name of the event */
	char name[];
};


event_t *events_list = NULL;

void lock() {}
void unlock() {}


/* creates a new event */
int make_event(const char *name, event_t **ev, afb_api_t api)
{
	event_t *e;
	int rc = 0;

	*ev = NULL;
	lock();
	e = events_list;
	while (e != NULL && strcmp(name, e->name))
		e = e->next;
	if (e != NULL)
		rc = AFB_ERRNO_BAD_STATE;
	else {
		e = calloc(1, strlen(name) + 1 + sizeof *e);
		if (e == NULL)
			rc = AFB_ERRNO_OUT_OF_MEMORY;
		else {
			rc = afb_api_new_event(api, name, &e->event);
			if (rc < 0) {
				free(e);
				rc = AFB_ERRNO_OUT_OF_MEMORY;
			}
			else {
				strcpy(e->name, name);
				e->next = events_list;
				*ev = events_list = e;
			}
		}
	}
	unlock();
	return rc;
}


/* remove an existing event */
int remove_event(const char *name)
{
	event_t *e, **pe;
	int rc = 0;

	lock();

	pe = &events_list;
	while (*pe != NULL && strcmp(name, (*pe)->name))
		pe = &(*pe)->next;
	if (*pe == NULL)
		rc = AFB_ERRNO_BAD_STATE;
	else {
		e = *pe;
		*pe = e->next;
		if (e->timer != NULL)
			afb_timer_unref(e->timer);
		afb_event_unref(e->event);
		if (e->dreq != NULL)
			afb_data_unref(e->dreq);
		if (e->dval != NULL)
			afb_data_unref(e->dval);
		free(e->requests);
		free(e->values);
		free(e);
	}
	unlock();
	return rc;
}



/***************
PREPARE REQUEST
****************/

#define HAS_ITF       1
#define HAS_ID        2
#define HAS_REG       4
#define HAS_SUBREG    8
#define HAS_TYPE     16
#define HAS_TPDO     32
#define REQFLAGS     (HAS_ITF | HAS_ID | HAS_REG | HAS_SUBREG | HAS_TYPE)

struct prepa
{
	canopen_xchg_v1_req_t *reqs;
	unsigned nreqs;
};

int getbool8(struct json_object *item, uint8_t *val)
{
	if (json_object_is_type(item, json_type_boolean)) {
		*val = (uint8_t)json_object_get_boolean(item);
		return 0;
	}
	return AFB_ERRNO_INVALID_REQUEST;
}

int gettype8(struct json_object *item, uint8_t *val)
{
	if (json_object_is_type(item, json_type_int)) {
		int x = json_object_get_int(item);
		if (x >= 0 && x <= 7) {
			*val = (uint8_t)x;
			return 0;
		}
	}
	else if (json_object_is_type(item, json_type_string)) {
		const char *x = json_object_get_string(item);
		if (!strcmp(x, "u8"))
			*val = canopen_xchg_u8;
		else if (!strcmp(x, "i8"))
			*val = canopen_xchg_i8;
		else if (!strcmp(x, "u16"))
			*val = canopen_xchg_u16;
		else if (!strcmp(x, "i16"))
			*val = canopen_xchg_i16;
		else if (!strcmp(x, "u32"))
			*val = canopen_xchg_u32;
		else if (!strcmp(x, "i32"))
			*val = canopen_xchg_i32;
		else if (!strcmp(x, "u64"))
			*val = canopen_xchg_u64;
		else if (!strcmp(x, "i64"))
			*val = canopen_xchg_i64;
		else
			return AFB_ERRNO_INVALID_REQUEST;
		return 0;
	}
	return AFB_ERRNO_INVALID_REQUEST;
}

int getuint8(struct json_object *item, uint8_t *val)
{
	if (json_object_is_type(item, json_type_int)) {
		int x = json_object_get_int(item);
		if (x >= 0 && x <= 255) {
			*val = (uint8_t)x;
			return 0;
		}
	}
	return AFB_ERRNO_INVALID_REQUEST;
}

int getuint16(struct json_object *item, uint16_t *val)
{

	if (json_object_is_type(item, json_type_int)) {
		int x = json_object_get_int(item);
		if (x >= 0 && x <= 65535) {
			*val = (uint16_t)x;
			return 0;
		}
	}
	return AFB_ERRNO_INVALID_REQUEST;
}

int prep(struct json_object *desc, struct prepa *prepa, unsigned flags, canopen_xchg_v1_req_t cureq)
{
	int rc = 0;

	if (json_object_is_type(desc, json_type_array)) {
		unsigned idx = (unsigned)json_object_array_length(desc);
		while (rc >= 0 && idx > 0)
			rc = prep(json_object_array_get_idx(desc, --idx), prepa, flags, cureq);
	}
	else if (json_object_is_type(desc, json_type_object)) {
		struct json_object *item;
		if (json_object_object_get_ex(desc, "itf", &item)) {
			rc = getuint8(item, &cureq.itf);
			flags |= HAS_ITF;
		}
		if (rc == 0 && json_object_object_get_ex(desc, "id", &item)) {
			rc = getuint8(item, &cureq.id);
			flags |= HAS_ID;
		}
		if (rc == 0 && json_object_object_get_ex(desc, "reg", &item)) {
			rc = getuint16(item, &cureq.reg);
			flags |= HAS_REG;
		}
		if (rc == 0 && json_object_object_get_ex(desc, "subreg", &item)) {
			rc = getuint8(item, &cureq.subreg);
			flags |= HAS_SUBREG;
		}
		if (rc == 0 && json_object_object_get_ex(desc, "type", &item)) {
			rc = gettype8(item, &cureq.type);
			flags |= HAS_TYPE;
		}
		if (rc == 0 && json_object_object_get_ex(desc, "tpdo", &item)) {
			rc = getbool8(item, &cureq.itf);
			flags |= HAS_TPDO;
		}
		if (rc == 0) {
			if (json_object_object_get_ex(desc, "items", &item))
				rc = prep(item, prepa, flags, cureq);
			else if ((flags & REQFLAGS) != REQFLAGS)
				rc = AFB_ERRNO_INVALID_REQUEST;
			else {
				canopen_xchg_v1_req_t *reqs = prepa->reqs;
				if ((prepa->nreqs & 15) == 0)
					reqs = realloc(reqs, (prepa->nreqs + 16) * sizeof *reqs);
				if (reqs == NULL)
					rc = AFB_ERRNO_OUT_OF_MEMORY;
				else {
					prepa->reqs = reqs;
					reqs[prepa->nreqs++] = cureq;
				}
			}
		}
	}
	else
		rc = AFB_ERRNO_INVALID_REQUEST;
	return rc;

}

int prepare(struct json_object *desc, afb_data_t *dreq, afb_data_t *dval)
{
	int rc;
	canopen_xchg_v1_req_t cureq;
	struct prepa prepa;

	memset(&prepa, 0, sizeof prepa);
	memset(&cureq, 0, sizeof cureq);
	rc = prep(desc, &prepa, 0, cureq);
	if (rc >= 0) {
		if (prepa.nreqs == 0)
			rc = AFB_ERRNO_INVALID_REQUEST;
		else {
			canopen_xchg_v1_value_t *xvals;
			rc = afb_create_data_raw(dreq, canopen_xchg_v1_req_type, prepa.reqs, prepa.nreqs * sizeof *prepa.reqs, free, prepa.reqs);
			if (rc >= 0) {
				rc = afb_create_data_alloc(dval, canopen_xchg_v1_value_type, &xvals, prepa.nreqs * sizeof(canopen_xchg_v1_value_t));
				if (rc < 0)
					afb_data_unref(req);
			}
			return rc >= 0 ? 0 : AFB_ERRNO_OUT_OF_MEMORY;
		}
	}
	free(prepa.reqs);
	return rc;
}


/******************
GETTING JSON OBJECT
*******************/

int make_json_reply(afb_data_t *jval, afb_data_t dreq, afb_data_t dval)
{
	char *buffer, *tmp;
	canopen_xchg_v1_value_t *xvals;
	canopen_xchg_v1_req_t *xreqs;
	size_t pos, dcnt, szbuf, szreqs, szvals;
	unsigned idx, count;
	int rc;

	if (afb_data_type(dreq) != canopen_xchg_v1_req_type)
		return AFB_ERRNO_INTERNAL_ERROR;
	if (afb_data_type(dval) != canopen_xchg_v1_value_type)
		return AFB_ERRNO_INTERNAL_ERROR;

	afb_data_get_constant(dreq, (void**)&xreqs, &szreqs);
	afb_data_get_constant(dval, (void**)&xvals, &szvals);
	count = (unsigned)(szreqs / sizeof *xreqs);
	if (count != (unsigned)(szvals / sizeof *xvals))
		return AFB_ERRNO_INTERNAL_ERROR;

	szbuf = 3 + 6 * count;
	buffer = malloc(szbuf);
	if (buffer == NULL)
		return AFB_ERRNO_OUT_OF_MEMORY;

	buffer[0] = '[';
	pos = 1;
	for (idx = 0; idx < count ; idx++) {
		dcnt = szbuf - pos;
		if (dcnt < 50) {
			szbuf += 50 + (szbuf >> 2);
			tmp = realloc(buffer, szbuf);
			if (tmp == NULL) {
				free(buffer);
				return AFB_ERRNO_INTERNAL_ERROR;
			}
			dcnt = szbuf - pos;
		}
		switch(xreqs[idx].type) {
		default:
		case canopen_xchg_u8:  rc = snprintf(&buffer[pos], dcnt, "%"PRIu8,  xvals[idx].u8);  break;
		case canopen_xchg_i8:  rc = snprintf(&buffer[pos], dcnt, "%"PRIi8,  xvals[idx].i8);  break;
		case canopen_xchg_u16: rc = snprintf(&buffer[pos], dcnt, "%"PRIu16, xvals[idx].u16); break;
		case canopen_xchg_i16: rc = snprintf(&buffer[pos], dcnt, "%"PRIi16, xvals[idx].i16); break;
		case canopen_xchg_u32: rc = snprintf(&buffer[pos], dcnt, "%"PRIu32, xvals[idx].u32); break;
		case canopen_xchg_i32: rc = snprintf(&buffer[pos], dcnt, "%"PRIi32, xvals[idx].i32); break;
		case canopen_xchg_u64: rc = snprintf(&buffer[pos], dcnt, "%"PRIu64, xvals[idx].u64); break;
		case canopen_xchg_i64: rc = snprintf(&buffer[pos], dcnt, "%"PRIi64, xvals[idx].i64); break;
		}
		if (rc < 0 || rc + 2 >= dcnt) {
			free(buffer);
			return AFB_ERRNO_INTERNAL_ERROR;
		}
		pos += rc;
		buffer[pos++] = ',';
	}
	buffer[pos - 1] = ']';
	buffer[pos++] = 0;
	return afb_create_data_raw(jval, AFB_PREDEFINED_TYPE_JSON, buffer, pos, free, buffer);
}





/******************
GETTING JSON OBJECT
*******************/

int get_json_first_arg(afb_req_t req, struct json_object **arg)
{
	afb_data_t data;
	int rc = afb_req_param_convert(req, 0, AFB_PREDEFINED_TYPE_JSON_C, &data);
	if (rc < 0)
		return AFB_ERRNO_INVALID_REQUEST;
	*arg = afb_data_ro_pointer(data);
	return 0;
}

/****
VERBS
****/

static void on_get_reply(
		void *closure,
		int status,
		unsigned nreplies,
		afb_data_t const replies[],
		afb_req_t req)
{
	afb_data_t jval, req = closure;
	if (status >= 0) {
		if (nreplies == 0 || afb_data_type(replies[0]) != canopen_xchg_v1_value_type)
			status = AFB_ERRNO_INTERNAL_ERROR;
		else
			status = make_json_reply(&jval, req, replies[0]);
	}
	afb_req_reply(req, status, status >= 0, &jval);
	afb_data_unref(req);
}

static void get(afb_req_t req, unsigned nparams, afb_data_t const *params)
{
	afb_data_t data[2];
	struct json_object *arg;
	int rc = get_json_first_arg(req, &arg);
	if (rc == 0)
		rc = prepare(arg, &data[0], &data[1]);
	if (rc == 0)
		afb_req_subcall(req, "canopen", "get", 2, data, 0, on_get_reply, afb_data_addref(data[0]));
	else
		afb_req_reply(req, rc, 0, NULL);
}

static void sub(afb_req_t req, unsigned nparams, afb_data_t const *params)
{
	const char *name;
	struct json_object *arg, *str;
	int rc = get_json_first_arg(req, &arg);
	if (rc == 0) {
		str = arg;
		if (json_object_is_type(arg, json_type_object))
			json_object_object_get_ex(arg, "name", &str);
		if (!json_object_is_type(str, json_type_string))
			rc = AFB_ERRNO_INVALID_REQUEST;
		else {
			name = json_object_get_string(str);
			rc = remove_event(name);
		}
	}
	afb_req_reply(req, rc, 0, NULL);
}

static void add(afb_req_t req, unsigned nparams, afb_data_t const *params)
{
	const char *name;
	struct json_object *arg, *str;
	int rc = get_json_first_arg(req, &arg);
	if (rc == 0)
		rc = prepare(arg, &data[0], &data[1]);
	if (rc == 0)
		afb_req_subcall(req, "canopen", "get", 2, data, 0, on_get_reply, afb_data_addref(data[0]));
	else
		afb_req_reply(req, rc, 0, NULL);

}











extern const char infotxt[];

static void info(afb_req_t request, unsigned nparams, afb_data_t const *params) {

    afb_data_t reply;

    afb_create_data_raw(&reply, AFB_PREDEFINED_TYPE_JSON, infotxt, 1 + strlen(infotxt), NULL, NULL);
    afb_req_reply(request, 0, 1, &reply);
}

/* Initialization */
static int mainctl(afb_api_t api, afb_ctlid_t ctlid, afb_ctlarg_t ctlarg, void *userdata)
{
    if (ctlid == afb_ctlid_Init)
        return canopen_xchg_init();
    return 0;
}

/************
BINDING SETUP
************/

/* List all the verbs we want to expose */
static const afb_verb_t verbs[] = {
    {.verb = "info", .callback = info},
    {.verb = "get", .callback = get},
    {.verb = "add", .callback = add},
    {.verb = "sub", .callback = sub},
    {.verb = NULL} /* no more verb */
};

/* Binding/API configuration should not be static because the binder must access it */
const afb_binding_t afbBindingExport = {
    .api = "demexco",
    .verbs = verbs,
    .mainctl = mainctl
};
