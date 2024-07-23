#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <stdbool.h>
#include <inttypes.h>
#include <threads.h>

#include <json-c/json.h>

#define AFB_BINDING_VERSION 4
#include <afb/afb-binding.h>
#include <CANopen/CANopenXchg.h>

#define CANOPEN_APINAME       "canopen"
#define CANOPEN_GET_VERBNAME  "get"

/****************************************************************************
** MANAGE LOCK
****************************************************************************/
mtx_t gmut;
void inilock() { mtx_init(&gmut, mtx_plain); }
void lock() { mtx_lock(&gmut); }
void unlock() { mtx_unlock(&gmut); }

/****************************************************************************
** EVENT MANAGEMENT
****************************************************************************/

typedef struct event_s event_t;

/*
the structure event records data for generating events
as required by calls to verb 'add'.
*/
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
	/* name of the event */
	char name[];
};

/*
* head of the recorded required events
*/
event_t *events_list = NULL;

/*
* creates a new event and its event structure
*
* @param[in]  name   the name of the event to be created
* @param[out] ev     where to store pointer to the created structure
*
* @return
*     - 0 in case of success
*     - AFB_ERRNO_BAD_STATE if the name already records an event
*     - AFB_ERRNO_OUT_OF_MEMORY if the memory is exhausted
*/
int make_event(const char *name, event_t **ev)
{
	event_t *e;
	int rc;

	lock();

	/* default no result */
	*ev = NULL;

	/* search existing name */
	e = events_list;
	while (e != NULL && strcmp(name, e->name))
		e = e->next;
	if (e != NULL)
		/* error name already exists */
		rc = AFB_ERRNO_BAD_STATE;
	else {
		/* creates the event */
		e = calloc(1, strlen(name) + 1 + sizeof *e);
		if (e == NULL)
			/* creation error */
			rc = AFB_ERRNO_OUT_OF_MEMORY;
		else {
			/* create the related event */
			rc = afb_api_new_event(afbBindingV4root, name, &e->event);
			if (rc < 0) {
				free(e);
				rc = AFB_ERRNO_OUT_OF_MEMORY;
			}
			else {
				/* creation success set name and add to list */
				strcpy(e->name, name);
				e->next = events_list;
				*ev = events_list = e;
				rc = 0;
			}
		}
	}
	unlock();
	return rc;
}

/*
* remove an existing event
*
* @param[in]  name   the name of the event to be removed
*
* @return
*     - 0 in case of success
*     - AFB_ERRNO_BAD_STATE when no event of that name exists
*/
int remove_event(const char *name)
{
	event_t *e, **pe;
	int rc;

	lock();

	/* search existing name */
	pe = &events_list;
	while (*pe != NULL && strcmp(name, (*pe)->name))
		pe = &(*pe)->next;
	if (*pe == NULL)
		/* searched name is not found */
		rc = AFB_ERRNO_BAD_STATE;
	else {
		/* found, remove from list and cleanup */
		e = *pe;
		*pe = e->next;
		if (e->timer != NULL)
			afb_timer_unref(e->timer);
		afb_event_unref(e->event);
		if (e->dreq != NULL)
			afb_data_unref(e->dreq);
		if (e->dval != NULL)
			afb_data_unref(e->dval);
		free(e);
		rc = 0;
	}
	unlock();
	return rc;
}

/****************************************************************************
** PREPARE REQUEST
****************************************************************************/

/* define flags for recording what data were set */
#define HAS_ITF       1
#define HAS_ID        2
#define HAS_REG       4
#define HAS_SUBREG    8
#define HAS_TYPE     16
#define HAS_TPDO     32

/* define the expected flags */
#define REQFLAGS     (HAS_ITF | HAS_ID | HAS_REG | HAS_SUBREG | HAS_TYPE)

/* structure recording created request, its values, its size, ... */
struct prepa
{
	/* the request data */
	canopen_xchg_v1_req_t *reqs;
	/* count of data */
	unsigned nreqs;
};

/* get a boolean value */
int getbool8(struct json_object *item, uint8_t *val)
{
	if (json_object_is_type(item, json_type_boolean)) {
		*val = (uint8_t)json_object_get_boolean(item);
		return 0;
	}
	return AFB_ERRNO_INVALID_REQUEST;
}

/* get a uint8 value */
int getxint(struct json_object *item, int *val)
{
	if (json_object_is_type(item, json_type_int)) {
		*val = json_object_get_int(item);
		return 1;
	}
	if (json_object_is_type(item, json_type_string)) {
		long x;
		const char *str = json_object_get_string(item);
		if (str[0] == '0' && (str[1] | ' ') == 'x')
			str += 2;
		x = strtol(str, (char**)&str, 16);
		if (!*str && x >= INT_MIN && x <= INT_MAX) {
			*val = (int)x;
			return 1;
		}
	}
	return 0;
}

/* get a type value as an uint8 */
int gettype8(struct json_object *item, uint8_t *val)
{
	int x;
	/* might be a number */
	if (getxint(item, &x)) {
		if (x >= 0 && x <= 7) {
			*val = (uint8_t)x;
			return 0;
		}
	}
	/* or a string */
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

/* get a uint8 value */
int getuint8(struct json_object *item, uint8_t *val)
{
	int x;
	if (getxint(item, &x)) {
		if (x >= 0 && x <= 255) {
			*val = (uint8_t)x;
			return 0;
		}
	}
	return AFB_ERRNO_INVALID_REQUEST;
}

/* get a uint16 value */
int getuint16(struct json_object *item, uint16_t *val)
{
	int x;
	if (getxint(item, &x)) {
		if (x >= 0 && x <= 65535) {
			*val = (uint16_t)x;
			return 0;
		}
	}
	return AFB_ERRNO_INVALID_REQUEST;
}

/*
* recursive construction of the request
*
* @param[in]    desc   description object
* @param[inout] prepa  pointer to the built structure
* @param[in]    flags  flags of set values
* @param[in]    cureq  currently set request
*
* @return
*     - 0 in case of success
*     - AFB_ERRNO_INVALID_REQUEST if an error in data existed
*     - AFB_ERRNO_OUT_OF_MEMORY if the memory is exhausted
*/
int prep(struct json_object *desc, struct prepa *prepa, unsigned flags, canopen_xchg_v1_req_t cureq)
{
	int rc = 0;

	/* is an array? */
	if (json_object_is_type(desc, json_type_array)) {
		/* yes, array, iterate recursively over items of the array */
		unsigned idx, cnt = (unsigned)json_object_array_length(desc);
		for (idx = 0 ; rc >= 0 && idx < cnt ; idx++)
			rc = prep(json_object_array_get_idx(desc, idx), prepa, flags, cureq);
	}
	/* not an array, is an object? */
	else if (json_object_is_type(desc, json_type_object)) {
		/* yes an object, get its meaningful components */
		struct json_object *item;

		/* get interface if any */
		if (json_object_object_get_ex(desc, "itf", &item)) {
			rc = getuint8(item, &cureq.itf);
			flags |= HAS_ITF;
		}

		/* get slave id if any */
		if (rc == 0 && json_object_object_get_ex(desc, "id", &item)) {
			rc = getuint8(item, &cureq.id);
			flags |= HAS_ID;
		}

		/* get register number if any */
		if (rc == 0 && json_object_object_get_ex(desc, "reg", &item)) {
			rc = getuint16(item, &cureq.reg);
			flags |= HAS_REG;
		}

		/* get sub-register number if any */
		if (rc == 0 && json_object_object_get_ex(desc, "subreg", &item)) {
			rc = getuint8(item, &cureq.subreg);
			flags |= HAS_SUBREG;
		}

		/* get value type if any */
		if (rc == 0 && json_object_object_get_ex(desc, "type", &item)) {
			rc = gettype8(item, &cureq.type);
			flags |= HAS_TYPE;
		}

		/* get tpdo flag if any */
		if (rc == 0 && json_object_object_get_ex(desc, "tpdo", &item)) {
			rc = getbool8(item, &cureq.itf);
			flags |= HAS_TPDO;
		}

		/* are meanigful items succefuly read ? */
		if (rc == 0) {
			/* yes, is there some sub items? */
			if (json_object_object_get_ex(desc, "items", &item))
				/* yes, sub items, recursive prepare */
				rc = prep(item, prepa, flags, cureq);
			/* no sub items, is the request completed? */
			else if ((flags & REQFLAGS) != REQFLAGS)
				/* no it is incomplete */
				rc = AFB_ERRNO_INVALID_REQUEST;
			else {
				/* request is complete, add it */
				canopen_xchg_v1_req_t *reqs = prepa->reqs;
				if ((prepa->nreqs & 15) == 0)
					/* resize if nreq is 0, 16, 32, 48, ... */
					reqs = realloc(reqs, (prepa->nreqs + 16) * sizeof *reqs);
				if (reqs == NULL)
					/* resize failed */
					rc = AFB_ERRNO_OUT_OF_MEMORY;
				else {
					/* record the request */
					prepa->reqs = reqs;
					reqs[prepa->nreqs++] = cureq;
				}
			}
		}
	}
	/* neither an array, nor an object, error */
	else
		rc = AFB_ERRNO_INVALID_REQUEST;
	return rc;

}

/*
* prepare the request accordingly to JSON description
* it returns the request data and also the return data
*
* @param[in]  desc   the JSON description
* @param[out] dreq   pointer to created data for request
* @param[out] dval   pointer to created data for values
*
* @return
*     - 0 in case of success
*     - AFB_ERRNO_INVALID_REQUEST if an error in data existed
*     - AFB_ERRNO_OUT_OF_MEMORY if the memory is exhausted
*/
int prepare(struct json_object *desc, afb_data_t *dreq, afb_data_t *dval)
{
	int rc;
	canopen_xchg_v1_req_t cureq;
	struct prepa prepa;

	/* prepare the request in prepa */
	memset(&prepa, 0, sizeof prepa);
	memset(&cureq, 0, sizeof cureq);
	rc = prep(desc, &prepa, 0, cureq);
	if (rc >= 0) {
		if (prepa.nreqs == 0)
			rc = AFB_ERRNO_INVALID_REQUEST;
		else {
			/* on success creates the data */
			void *xvals;
			rc = afb_create_data_raw(dreq, canopen_xchg_v1_req_type, prepa.reqs, prepa.nreqs * sizeof *prepa.reqs, free, prepa.reqs);
			if (rc >= 0) {
				rc = afb_create_data_alloc(dval, canopen_xchg_v1_value_type, &xvals, prepa.nreqs * sizeof(canopen_xchg_v1_value_t));
				if (rc < 0)
					afb_data_unref(*dreq);
			}
			return rc >= 0 ? 0 : AFB_ERRNO_OUT_OF_MEMORY;
		}
	}
	free(prepa.reqs);
	return rc;
}

/****************************************************************************
** GETTING JSON OBJECT
****************************************************************************/

/*
* get the JSON string data for the returned values dval of the given request dreq
*
* @param[out]  jval  pointer to return the created data
* @param[in]   dreq  the request data
* @param[in]   dval  the value data
*
* @return
*     - 0 in case of success
*     - AFB_ERRNO_OUT_OF_MEMORY if the memory is exhausted
*     - AFB_ERRNO_INTERNAL_ERROR if some unexpected internal error occurred
*/
int make_json_reply(afb_data_t *jval, afb_data_t dreq, afb_data_t dval)
{
	char *buffer, *tmp;
	canopen_xchg_v1_value_t *xvals;
	canopen_xchg_v1_req_t *xreqs;
	size_t pos, dcnt, szbuf, szreqs, szvals;
	unsigned idx, count;
	int rc;

	/* check types of dreq and dval */
	if (afb_data_type(dreq) != canopen_xchg_v1_req_type)
		return AFB_ERRNO_INTERNAL_ERROR;
	if (afb_data_type(dval) != canopen_xchg_v1_value_type)
		return AFB_ERRNO_INTERNAL_ERROR;

	/* extract data from dreq and dval */
	afb_data_get_constant(dreq, (void**)&xreqs, &szreqs);
	afb_data_get_constant(dval, (void**)&xvals, &szvals);
	count = (unsigned)(szreqs / sizeof *xreqs);
	if (count != (unsigned)(szvals / sizeof *xvals))
		return AFB_ERRNO_INTERNAL_ERROR;

	/* a-priori allocation */
	szbuf = 3 + 8 * count;
	buffer = malloc(szbuf);
	if (buffer == NULL)
		return AFB_ERRNO_OUT_OF_MEMORY;

	/* start array of values */
	buffer[0] = '[';
	pos = 1;

	/* iterate over values */
	for (idx = 0; idx < count ; idx++) {
		/* check remaining size */
		dcnt = szbuf - pos;
		if (dcnt < 50) {
			/* extend the buffer */
			szbuf += 50 + (szbuf >> 2);
			tmp = realloc(buffer, szbuf);
			if (tmp == NULL) {
				free(buffer);
				return AFB_ERRNO_OUT_OF_MEMORY;
			}
			buffer = tmp;
			dcnt = szbuf - pos;
		}
		/* format the value */
		switch(xreqs[idx].type) {
		default:  rc = snprintf(&buffer[pos], dcnt, "null");  break;
		case canopen_xchg_u8:  rc = snprintf(&buffer[pos], dcnt, "%"PRIu8,  xvals[idx].u8);  break;
		case canopen_xchg_i8:  rc = snprintf(&buffer[pos], dcnt, "%"PRIi8,  xvals[idx].i8);  break;
		case canopen_xchg_u16: rc = snprintf(&buffer[pos], dcnt, "%"PRIu16, xvals[idx].u16); break;
		case canopen_xchg_i16: rc = snprintf(&buffer[pos], dcnt, "%"PRIi16, xvals[idx].i16); break;
		case canopen_xchg_u32: rc = snprintf(&buffer[pos], dcnt, "%"PRIu32, xvals[idx].u32); break;
		case canopen_xchg_i32: rc = snprintf(&buffer[pos], dcnt, "%"PRIi32, xvals[idx].i32); break;
		case canopen_xchg_u64: rc = snprintf(&buffer[pos], dcnt, "%"PRIu64, xvals[idx].u64); break;
		case canopen_xchg_i64: rc = snprintf(&buffer[pos], dcnt, "%"PRIi64, xvals[idx].i64); break;
		}
		/* append one comma */
		if (rc < 0 || rc + 2 >= dcnt) {
			free(buffer);
			return AFB_ERRNO_INTERNAL_ERROR;
		}
		pos += rc;
		buffer[pos++] = ',';
	}
	/* terminates array of values */
	buffer[pos - (pos > 1)] = ']';
	buffer[pos++] = 0;

	/* create the JSON data with the buffer */
	return afb_create_data_raw(jval, AFB_PREDEFINED_TYPE_JSON, buffer, pos, free, buffer);
}

/****************************************************************************
** GETTING JSON OBJECT
****************************************************************************/

/*
* extract the first argument as a JSON-C object
*
* @param[in]  req  the request whose first argument is to be extracted
* @param[out] arg  pointer the the JSON-C object retrieved on success
*
* @return
*     - 0 in case of success
*     - AFB_ERRNO_INVALID_REQUEST if the first argument doesn't exist or is not JSON
*/
int get_json_first_arg(afb_req_t req, struct json_object **arg)
{
	afb_data_t data;
	int rc = afb_req_param_convert(req, 0, AFB_PREDEFINED_TYPE_JSON_C, &data);
	if (rc < 0)
		return AFB_ERRNO_INVALID_REQUEST;
	*arg = afb_data_ro_pointer(data);
	return 0;
}

/****************************************************************************
** VERB get
****************************************************************************/

static void on_get_reply(
		void *closure,
		int status,
		unsigned nreplies,
		afb_data_t const replies[],
		afb_req_t req)
{
	afb_data_t jval, dreq = closure;
	if (status >= 0) {
		if (nreplies == 0 || afb_data_type(replies[0]) != canopen_xchg_v1_value_type)
			status = AFB_ERRNO_INTERNAL_ERROR;
		else
			status = make_json_reply(&jval, dreq, replies[0]);
	}
	afb_req_reply(req, status, status >= 0, &jval);
	afb_data_unref(dreq);
}

static void get(afb_req_t req, unsigned nparams, afb_data_t const *params)
{
	afb_data_t data[2];
	struct json_object *arg;
	int rc = get_json_first_arg(req, &arg);
	if (rc == 0)
		rc = prepare(arg, &data[0], &data[1]);
	if (rc == 0)
		afb_req_subcall(req, CANOPEN_APINAME, CANOPEN_GET_VERBNAME, 2, data, 0, on_get_reply, afb_data_addref(data[0]));
	else
		afb_req_reply(req, rc, 0, NULL);
}

/****************************************************************************
** VERB rem
****************************************************************************/

static void rem(afb_req_t req, unsigned nparams, afb_data_t const *params)
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

/****************************************************************************
** VERB add
****************************************************************************/

static void on_timed_get_reply(
		void *closure,
		int status,
		unsigned nreplies,
		afb_data_t const replies[],
		afb_api_t api)
{
	event_t *ev = closure;
	afb_data_t jval;

	lock();
	if (status < 0) {
	}
	else if (nreplies == 0) {
	}
	else if (afb_data_type(replies[0]) != canopen_xchg_v1_value_type) {
	}
	else if (make_json_reply(&jval, ev->dreq, replies[0]) < 0) {
	}
	else {
		afb_event_push(ev->event, 1, &jval);
	}
	if (nreplies > 0 && replies[0] == ev->dval)
		afb_data_addref(ev->dval);
	unlock();
}


static void on_timed_top(afb_timer_t timer, void *closure, unsigned decount)
{
	event_t *ev = closure;
	lock();
	afb_data_t params[2] = { afb_data_addref(ev->dreq), afb_data_addref(ev->dval) };
	afb_api_call(afbBindingV4root, CANOPEN_APINAME, CANOPEN_GET_VERBNAME, 2, params, on_timed_get_reply, ev);
	unlock();
}

static void add(afb_req_t req, unsigned nparams, afb_data_t const *params)
{
	int ms;
	afb_data_t data[2];
	event_t *ev;
	struct json_object *arg, *jname, *jper, *jget;

	int rc = get_json_first_arg(req, &arg);
	if (rc == 0) {
		if (!json_object_object_get_ex(arg, "name", &jname)
		 || !json_object_is_type(jname, json_type_string)
		 || !json_object_object_get_ex(arg, "periodms", &jper)
		 || !json_object_is_type(jper, json_type_int)
		 || !(ms = json_object_get_int(jper)) <= 0
		 || !json_object_object_get_ex(arg, "get", &jget))
			rc = AFB_ERRNO_INVALID_REQUEST;
		else {
			rc = prepare(arg, &data[0], &data[1]);
			if (rc == 0) {
				rc = make_event(json_object_get_string(jname), &ev);
				if (rc != 0) {
					afb_data_unref(data[0]);
					afb_data_unref(data[1]);
				}
				else {
					ev->dreq = data[0];
					ev->dval = data[1];
					rc = afb_req_subscribe(req, ev->event);
					if (rc >= 0)
						rc = afb_timer_create(&ev->timer, 0, 0, 2, 0, (unsigned)ms, 10, on_timed_top, ev, 0);
					if (rc >= 0)
						rc = 0;
					else {
						remove_event(json_object_get_string(jname));
						rc = AFB_ERRNO_INTERNAL_ERROR;
					}
				}
			}
		}
	}
	afb_req_reply(req, rc, 0, NULL);
}

/****************************************************************************
** VERB info
****************************************************************************/

extern const char infotxt[];

static void info(afb_req_t request, unsigned nparams, afb_data_t const *params) {
	afb_data_t reply;

	afb_create_data_raw(&reply, AFB_PREDEFINED_TYPE_JSON, infotxt, 1 + strlen(infotxt), NULL, NULL);
	afb_req_reply(request, 0, 1, &reply);
}

/****************************************************************************
** MAIN ENTRY FOR Initialization
****************************************************************************/
static int mainctl(afb_api_t api, afb_ctlid_t ctlid, afb_ctlarg_t ctlarg, void *userdata)
{
	if (ctlid == afb_ctlid_Init) {
		inilock();
		return canopen_xchg_init();
	}
	return 0;
}

/****************************************************************************
** BINDING SETUP STRUCTURES
****************************************************************************/

/* List all the verbs we want to expose */
static const afb_verb_t verbs[] = {
	{.verb = "info", .callback = info},
	{.verb = "get", .callback = get},
	{.verb = "add", .callback = add},
	{.verb = "rem", .callback = rem},
	{.verb = NULL} /* no more verb */
};

/* Binding/API configuration should not be static because the binder must access it */
const afb_binding_t afbBindingExport = {
	.api = "demexco",
	.verbs = verbs,
	.mainctl = mainctl,
	.require_api = CANOPEN_APINAME
};
