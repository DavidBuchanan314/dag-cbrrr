#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <string.h>
#include <stdint.h>


typedef enum {
	DCMT_UNSIGNED_INT = 0,
	DCMT_NEGATIVE_INT = 1,
	DCMT_BYTE_STRING = 2,
	DCMT_TEXT_STRING = 3,
	DCMT_ARRAY = 4,
	DCMT_MAP = 5,
	DCMT_TAG = 6,
	DCMT_FLOAT = 7,
} DCMajorType;

typedef struct {
	DCMajorType type;
	PyObject *value;
	uint64_t count; // array/map length

	// used to ensure map key ordering
	const uint8_t *prev_key;
	size_t prev_key_len;
} DCToken;

static size_t
cbrrr_parse_minimal_varint(const uint8_t *buf, size_t len, uint64_t *value)
{
	switch (*value)
	{
	case 24:
		if (len < 1) {
			PyErr_SetString(PyExc_EOFError, "not enough bytes left in buffer");
			return -1;
		}
		*value = buf[0];
		if (*value < 24) {
			PyErr_SetString(PyExc_ValueError, "integer not minimally encoded");
			return -1;
		}
		return 1;
	case 25:
		if (len < 2) {
			PyErr_SetString(PyExc_EOFError, "not enough bytes left in buffer");
			return -1;
		}
		*value = be16toh(*(uint16_t *)buf);
		if (*value < 0x100) {
			PyErr_SetString(PyExc_ValueError, "integer not minimally encoded");
			return -1;
		}
		return 2;
	case 26:
		if (len < 4) {
			PyErr_SetString(PyExc_EOFError, "not enough bytes left in buffer");
			return -1;
		}
		*value = be32toh(*(uint32_t *)buf);
		if (*value < 0x10000) {
			PyErr_SetString(PyExc_ValueError, "integer not minimally encoded");
			return -1;
		}
		return 4;
	case 27:
		if (len < 8) {
			PyErr_SetString(PyExc_EOFError, "not enough bytes left in buffer");
			return -1;
		}
		*value = be64toh(*(uint64_t *)buf);
		if (*value < 0x100000000L) {
			PyErr_SetString(PyExc_ValueError, "integer not minimally encoded");
			return -1;
		}
		return 8;
	default:
		if (*value > 27) {
			PyErr_Format(PyExc_ValueError, "invalid extra info (%lu)", *value);
			return -1;
		}
		return 0;
	}
}

// special case, used for parsing map keys and CIDs
static size_t
cbrrr_parse_raw_string(const uint8_t *buf, size_t len, DCMajorType type, const uint8_t **str, size_t *str_len)
{
	size_t idx = 0, res;

	if (len < idx + 1) {
		PyErr_SetString(PyExc_EOFError, "not enough bytes left in buffer");
		return -1;
	}
	*str_len = buf[idx++];
	if ((*str_len >> 5) != type) {
		PyErr_Format(PyExc_ValueError, "unexpected type (%lu), expected %lu", (*str_len >> 5), type);
		return -1;
	}
	*str_len &= 0x1f;
	res = cbrrr_parse_minimal_varint(&buf[idx], len-idx, str_len);
	if (res == (size_t)-1) {
		// python error has been set by cbrrr_parse_minimal_varint
		return -1;
	}
	idx += res;
	if (*str_len > len - idx) {
		PyErr_SetString(PyExc_EOFError, "not enough bytes left in buffer");
		return -1;
	}
	*str = &buf[idx];
	return idx + *str_len;
}

// returns number of bytes parsed, -1 on failure
// todo: put error explanation in error token?
static size_t
cbrrr_parse_token(const uint8_t *buf, size_t len, DCToken *token, PyObject *cid_ctor)
{
	uint64_t info;
	size_t idx = 0, res;
	PyObject *tmp;

	if (len < idx + 1) {
		PyErr_SetString(PyExc_EOFError, "not enough bytes left in buffer");
		return -1;
	}

	token->type = buf[idx] >> 5;
	info = buf[idx] & 0x1f;
	idx += 1;

	if (token->type == DCMT_FLOAT) { // the special case
		switch (info)
		{
		case 20:
			token->value = Py_False;
			Py_INCREF(token->value);
			return idx;
		case 21:
			token->value = Py_True;
			Py_INCREF(token->value);
			return idx;
		case 22:
			token->value = Py_None;
			Py_INCREF(token->value);
			return idx;
		case 27:
			if (len < idx + sizeof(double)) {
				PyErr_SetString(PyExc_EOFError, "not enough bytes left in buffer");
				return -1;
			}
			token->value = PyFloat_FromDouble(((union {uint64_t num; double dub;}){.num=be64toh(*(uint64_t*)&buf[idx])}).dub); // TODO: rewrite lol
			return idx;
		default:
			PyErr_Format(PyExc_ValueError, "invalid extra info for float mtype (%lu)", info);
			return -1;
		}
	}

	res = cbrrr_parse_minimal_varint(&buf[idx], len-idx, &info);
	if (res == (size_t)-1) {
		// python error set by cbrrr_parse_minimal_varint
		return -1;
	}
	idx += res;

	// at this point, `info` represents its actual value, with meaning depending on the major type

	switch (token->type)
	{
	case DCMT_UNSIGNED_INT:
		token->value = PyLong_FromUnsignedLongLong(info);
		return idx;
	case DCMT_NEGATIVE_INT:
		tmp = PyLong_FromUnsignedLongLong(info);
		token->value = PyNumber_Invert(tmp);
		Py_DECREF(tmp); // XXX
		return idx;
	case DCMT_BYTE_STRING:
		if (info > len - idx) {
			PyErr_SetString(PyExc_EOFError, "not enough bytes left in buffer");
			return -1;
		}
		token->value = PyBytes_FromStringAndSize((const char *)&buf[idx], info);
		return idx + info;
	case DCMT_TEXT_STRING:
		if (info > len - idx) {
			PyErr_SetString(PyExc_EOFError, "not enough bytes left in buffer");
			return -1;
		}
		token->value = PyUnicode_FromStringAndSize((const char *)&buf[idx], info);
		if (token->value == NULL) { // invalid unicode
			return -1;
		}
		return idx + info;
	case DCMT_ARRAY:
		if (info > len - idx) {
			PyErr_SetString(PyExc_EOFError, "not enough bytes left in buffer for an array that long");
			return -1;
		}
		token->value = PyList_New(info);
		if (token->value == NULL) { // probably tried to allocate a too-big list
			return -1;
		}
		token->count = info;
		return idx;
	case DCMT_MAP:
		if (info > len - idx) {
			PyErr_SetString(PyExc_EOFError, "not enough bytes left in buffer for a map that long");
			return -1;
		}
		token->value = PyDict_New();
		if (token->value == NULL) { // something bad happened I guess
			return -1;
		}
		token->count = info;
		token->prev_key = (const uint8_t *)"";
		token->prev_key_len = 0;
		return idx;
	case DCMT_TAG:
		if (info != 42) { // only tag type 42=CID is supported
			PyErr_Format(PyExc_ValueError, "invalid tag value (%lu)", info);
			return -1;
		}
		// parse a byte string
		const uint8_t *str;
		size_t str_len;
		res = cbrrr_parse_raw_string(&buf[idx], len-idx, DCMT_BYTE_STRING, &str, &str_len);
		if (res == (size_t)-1) {
			// python error set by cbrrr_parse_raw_string
			return -1;
		}
		tmp = PyBytes_FromStringAndSize((const char*)str, str_len);
		token->value = PyObject_CallOneArg(cid_ctor, tmp);
		Py_DECREF(tmp);

		//token->value = PyBytes_FromStringAndSize((const char*)str, str_len);

		return idx + res;
	default:
		PyErr_Format(PyExc_Exception, "you reached unreachable code??? (type=%lu)", token->type);
		return -1; // unreachable?
	}
}

size_t
cbrrr_parse_object(const uint8_t *buf, size_t len, PyObject **value, PyObject *cid_ctor)
{
	size_t stack_len = 16;
	DCToken *parse_stack = malloc(stack_len * sizeof(*parse_stack));

	if (parse_stack == NULL) {
		PyErr_SetString(PyExc_MemoryError, "malloc failed");
		return -1;
	}

	// pretend that we're parsing an array of length 1
	parse_stack[0].type = DCMT_ARRAY;
	parse_stack[0].value = PyList_New(1);
	parse_stack[0].count = 1;

	size_t sp = 0;
	size_t idx = 0;

	/* parser stack machine thing... if it looks confusing it's because it is */

	for (;;) {
		if (parse_stack[sp].count == 0) {
			if (sp == 0) {
				*value = PyList_GET_ITEM(parse_stack[0].value, 0);
				Py_INCREF(*value);
				break;
			}
			sp -= 1; 
			continue;
		}

		if (parse_stack[sp].type == DCMT_ARRAY) {
			size_t res = cbrrr_parse_token(&buf[idx], len-idx, &parse_stack[sp+1], cid_ctor);
			if (res == (size_t)-1) {
				idx = -1;
				break;
			}
			idx += res;
			// move ownership of sp+1 into sp
			PyList_SET_ITEM(
				parse_stack[sp].value,
				PyList_GET_SIZE(parse_stack[sp].value) - parse_stack[sp].count,
				parse_stack[sp+1].value
			);
			parse_stack[sp].count -= 1;
		} else { // DCMT_MAP
			const u_int8_t *str;
			size_t str_len;
			size_t res = cbrrr_parse_raw_string(&buf[idx], len-idx, DCMT_TEXT_STRING, &str, &str_len);
			if (res == (size_t)-1) {
				// panik
				idx = -1;
				break;
			}
			idx += res;
			if (str_len < parse_stack[sp].prev_key_len) { // key order violation
				// panik
				PyErr_Format(PyExc_ValueError, "non-canonical map key ordering (len('%s') < len('%s'))", str, parse_stack[sp].prev_key);
				idx = -1;
				break;
			} else if (str_len == parse_stack[sp].prev_key_len) { // ditto
				if (memcmp(str, parse_stack[sp].prev_key, str_len) <= 0) {
					PyErr_Format(PyExc_ValueError, "non-canonical map key ordering ('%s' <= '%s')", str, parse_stack[sp].prev_key);
					idx = -1;
					break;
				}
			}
			parse_stack[sp].prev_key = str;
			parse_stack[sp].prev_key_len = str_len;

			// check unicode validity before parsing next token to avoid leaking a reference when we bail out
			// TODO: fast-path(s) for common/short keys
			PyObject *key = PyUnicode_FromStringAndSize((const char*)str, str_len);
			if (key == NULL) { // unicode error
				idx = -1;
				break;
			}

			res = cbrrr_parse_token(&buf[idx], len-idx, &parse_stack[sp+1], cid_ctor);
			if (res == (size_t)-1) {
				idx = -1;
				break;
			}
			idx += res;

			// move ownership of sp+1 into sp
			PyDict_SetItem(parse_stack[sp].value, key, parse_stack[sp+1].value);
			Py_DECREF(key);
			Py_DECREF(parse_stack[sp+1].value);
			parse_stack[sp].count -= 1;
		}

		if ((parse_stack[sp+1].type == DCMT_ARRAY) || (parse_stack[sp+1].type == DCMT_MAP)) {
			sp += 1;
			if ((sp + 1) >= stack_len) {
				stack_len *= 2; // TODO: smaller increments?
				parse_stack = realloc(parse_stack, stack_len * sizeof(*parse_stack));
				if (parse_stack == NULL) {
					PyErr_SetString(PyExc_MemoryError, "realloc failed");
					idx = -1;
					break;
				}
			}
		}
	}

	// under non-error conditions, the final GetItem preserves the refcount of the actual result,
	// but we still want to free the dummy array of length 1 we initially created

	// under error conditions, this *also* acheives the desired effect
	Py_DecRef(parse_stack[0].value);

	free(parse_stack);
	return idx;
}


static PyObject *
cbrrr_parse_dag_cbor(PyObject *self, PyObject *args)
{
	Py_buffer buf;
	PyObject *cid_ctor;

	(void)self; // unused

	if (!PyArg_ParseTuple(args, "y*O", &buf, &cid_ctor))
		return NULL;

	// XXX: check cid_ctor is callable

	PyObject *value = NULL;//Py_None;

	size_t res = cbrrr_parse_object(buf.buf, buf.len, &value, cid_ctor);
	PyBuffer_Release(&buf);

	if (res == (size_t)-1) {
		return NULL;
	}

	PyObject *lenvar = PyLong_FromUnsignedLongLong(res);

	PyObject *restuple = PyTuple_Pack(2, value, lenvar);
	Py_DECREF(value);
	Py_DECREF(lenvar);

	return restuple;
}

static PyMethodDef CbrrrMethods[] = {
	{"parse_dag_cbor", cbrrr_parse_dag_cbor, METH_VARARGS,
		"parse a buffer of DAG-CBOR into python objects"},
	{NULL, NULL, 0, NULL}        /* Sentinel */
};

static struct PyModuleDef cbrrrmodule = {
	PyModuleDef_HEAD_INIT,
	"cbrrr",   /* name of module */
	NULL, /* module documentation, may be NULL */
	-1,       /* size of per-interpreter state of the module,
					or -1 if the module keeps state in global variables. */
	CbrrrMethods,
	NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC
PyInit_cbrrr(void)
{
	PyObject *m;

	m = PyModule_Create(&cbrrrmodule);
	if (m == NULL)
		return NULL;

	// TODO: other stuff

	return m;
}
