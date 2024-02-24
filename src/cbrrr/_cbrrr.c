#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <string.h>
#include <stdint.h>

#define STATIC_ASSERT(COND,MSG) typedef char static_assertion_##MSG[(COND)?1:-1]

STATIC_ASSERT(sizeof(size_t) == 8, _64bit_platforms_only); // this'll hopefully be relaxed in the future

// XXX: not sure having these as globals is the right thing to do?
static PyObject *PY_ZERO;
static PyObject *PY_UINT64_MAX;
static PyObject *PY_UINT64_MAX_INVERTED;
static PyObject *PY_STRING_ENCODE;
static PyObject *PY_STRING_DECODE;
static PyObject *PY_STRING_LINK;
static PyObject *PY_STRING_BYTES;

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
} DCToken; // also used as the parser's stack frame

// growable buffer for storing the encoded result
typedef struct {
	uint8_t *buf;
	size_t length;
	size_t capacity;
} CbrrrBuf;

typedef struct {
	PyObject *dict; // the dict, or NULL if this frame is a list
	PyObject *list; // either the list, or the sorted map keys
	Py_ssize_t idx; // the current list index
} EncoderStackFrame;

static const uint8_t B64_CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static PyObject*
cbrrr_bytes_to_b64_string_nopad(const uint8_t *data, size_t data_len)
{
	/* XXX: data_len*4 could integer overflow. Unless you have 2^63 bytes of RAM,
	   it should be impossible to reach that condition. To make this safe on 32-bit
	   platforms we'll need to enforce a length limit */
	PyObject *res = PyUnicode_New((data_len*4+2)/3, 127); /* ASCII-only (b64 charset) */
	if (res == NULL) {
		return NULL;
	}
	uint8_t *resbuf = PyUnicode_DATA(res);
	uint8_t a, b, c;
	size_t data_i = 0;
	while ( data_i + 2 < data_len) {
		a = data[data_i++];
		b = data[data_i++];
		c = data[data_i++];
		*resbuf++ = B64_CHARSET[(           (a >> 2)) & 0x3f];
		*resbuf++ = B64_CHARSET[((a << 4) | (b >> 4)) & 0x3f];
		*resbuf++ = B64_CHARSET[((b << 2) | (c >> 6)) & 0x3f];
		*resbuf++ = B64_CHARSET[((c << 0)           ) & 0x3f];
	}
	switch (data_len - data_i)
	{
	case 2:
		a = data[data_i++];
		b = data[data_i++];
		*resbuf++ = B64_CHARSET[(           (a >> 2)) & 0x3f];
		*resbuf++ = B64_CHARSET[((a << 4) | (b >> 4)) & 0x3f];
		*resbuf++ = B64_CHARSET[((b << 2)           ) & 0x3f];
		break;
	case 1:
		a = data[data_i++];
		*resbuf++ = B64_CHARSET[(           (a >> 2)) & 0x3f];
		*resbuf++ = B64_CHARSET[((a << 4)           ) & 0x3f];
		break;
	case 0:
		// nothing to do here
		break;
	default:
		PyErr_SetString(PyExc_AssertionError, "unreachable!?");
		Py_DECREF(res);
		return NULL;
	}
	return res;
}


static const uint8_t B32_CHARSET[] = "abcdefghijklmnopqrstuvwxyz234567";

static PyObject*
cbrrr_bytes_to_b32_multibase(const uint8_t *data, size_t data_len)
{
	/* XXX: see comment in b64 fn above, re integer overflow */
	PyObject *res = PyUnicode_New(1 + (data_len*8+4)/5, 127); /* ASCII-only (b32 charset) */
	if (res == NULL) {
		return NULL;
	}
	uint8_t *resbuf = PyUnicode_DATA(res);
	*resbuf++ = 'b'; // b prefix indicates multibase base32
	uint8_t a, b, c, d, e;
	size_t data_i = 0;
	while ( data_i + 4 < data_len) {
		a = data[data_i++];
		b = data[data_i++];
		c = data[data_i++];
		d = data[data_i++];
		e = data[data_i++];
		// 76543 21076 54321 07654 32107 65432 10765 43210
		// aaaaa aaabb bbbbb bcccc ccccd ddddd ddeee eeeee
		// 43210 43210 43210 43210 43210 43210 43210 43210
		*resbuf++ = B32_CHARSET[(           (a >> 3)) & 0x1f];
		*resbuf++ = B32_CHARSET[((a << 2) | (b >> 6)) & 0x1f];
		*resbuf++ = B32_CHARSET[(           (b >> 1)) & 0x1f];
		*resbuf++ = B32_CHARSET[((b << 4) | (c >> 4)) & 0x1f];
		*resbuf++ = B32_CHARSET[((c << 1) | (d >> 7)) & 0x1f];
		*resbuf++ = B32_CHARSET[(           (d >> 2)) & 0x1f];
		*resbuf++ = B32_CHARSET[((d << 3) | (e >> 5)) & 0x1f];
		*resbuf++ = B32_CHARSET[((e << 0)           ) & 0x1f];
	}
	switch (data_len - data_i) // TODO: can this be simplified, with fallrthu perhaps?
	{
	case 4:
		a = data[data_i++];
		b = data[data_i++];
		c = data[data_i++];
		d = data[data_i++];
		*resbuf++ = B32_CHARSET[(           (a >> 3)) & 0x1f];
		*resbuf++ = B32_CHARSET[((a << 2) | (b >> 6)) & 0x1f];
		*resbuf++ = B32_CHARSET[(           (b >> 1)) & 0x1f];
		*resbuf++ = B32_CHARSET[((b << 4) | (c >> 4)) & 0x1f];
		*resbuf++ = B32_CHARSET[((c << 1) | (d >> 7)) & 0x1f];
		*resbuf++ = B32_CHARSET[(           (d >> 2)) & 0x1f];
		*resbuf++ = B32_CHARSET[((d << 3)           ) & 0x1f];
		break;
	case 3:
		a = data[data_i++];
		b = data[data_i++];
		c = data[data_i++];
		*resbuf++ = B32_CHARSET[(           (a >> 3)) & 0x1f];
		*resbuf++ = B32_CHARSET[((a << 2) | (b >> 6)) & 0x1f];
		*resbuf++ = B32_CHARSET[(           (b >> 1)) & 0x1f];
		*resbuf++ = B32_CHARSET[((b << 4) | (c >> 4)) & 0x1f];
		*resbuf++ = B32_CHARSET[((c << 1)           ) & 0x1f];
		break;
	case 2:
		a = data[data_i++];
		b = data[data_i++];
		*resbuf++ = B32_CHARSET[(           (a >> 3)) & 0x1f];
		*resbuf++ = B32_CHARSET[((a << 2) | (b >> 6)) & 0x1f];
		*resbuf++ = B32_CHARSET[(           (b >> 1)) & 0x1f];
		*resbuf++ = B32_CHARSET[((b << 4)           ) & 0x1f];
		break;
	case 1:
		a = data[data_i++];
		*resbuf++ = B32_CHARSET[(           (a >> 3)) & 0x1f];
		*resbuf++ = B32_CHARSET[((a << 2)           ) & 0x1f];
		break;
	case 0:
		// nothing to do here
		break;
	default:
		PyErr_SetString(PyExc_AssertionError, "unreachable!?");
		Py_DECREF(res);
		return NULL;
	}
	return res;
}


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
		*value = buf[0] << 8 | buf[1] << 0;
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
		*value = (uint64_t)buf[0] << 24 | (uint64_t)buf[1] << 16
		       | (uint64_t)buf[2] << 8  | (uint64_t)buf[3] << 0;
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
		*value = (uint64_t)buf[0] << 56 | (uint64_t)buf[1] << 48
		       | (uint64_t)buf[2] << 40 | (uint64_t)buf[3] << 32
		       | (uint64_t)buf[4] << 24 | (uint64_t)buf[5] << 16
		       | (uint64_t)buf[6] << 8  | (uint64_t)buf[7] << 0;
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
	res = cbrrr_parse_minimal_varint(&buf[idx], len-idx, (uint64_t*)str_len);
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
static size_t
cbrrr_parse_token(const uint8_t *buf, size_t len, DCToken *token, PyObject *cid_ctor, int atjson_mode)
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
			uint64_t intval = \
				  (uint64_t)buf[idx+0] << 56 | (uint64_t)buf[idx+1] << 48
				| (uint64_t)buf[idx+2] << 40 | (uint64_t)buf[idx+3] << 32
				| (uint64_t)buf[idx+4] << 24 | (uint64_t)buf[idx+5] << 16
				| (uint64_t)buf[idx+6] << 8  | (uint64_t)buf[idx+7] << 0;
			double doubleval = ((union {uint64_t num; double dub;}){.num=intval}).dub; // TODO: rewrite lol
			if (isnan(doubleval)) {
				PyErr_SetString(PyExc_ValueError, "NaNs are not allowed");
				return -1;
			}
			if (isinf(doubleval)) {
				PyErr_SetString(PyExc_ValueError, "+/-Infinities are not allowed");
				return -1;
			}
			token->value = PyFloat_FromDouble(doubleval);
			return idx + sizeof(double);
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
		if (token->value == NULL) {
			return -1;
		}
		return idx;
	case DCMT_NEGATIVE_INT:
		tmp = PyLong_FromUnsignedLongLong(info);
		if (tmp == NULL) {
			return -1;
		}
		token->value = PyNumber_Invert(tmp);
		Py_DECREF(tmp);
		if (token->value == NULL) {
			return -1;
		}
		return idx;
	case DCMT_BYTE_STRING:
		if (info > len - idx) {
			PyErr_SetString(PyExc_EOFError, "not enough bytes left in buffer");
			return -1;
		}
		if (atjson_mode) { /* wrap in {"$bytes", "b64..."} */
			tmp = cbrrr_bytes_to_b64_string_nopad((uint8_t*)&buf[idx], info);
			if (tmp == NULL) {
				return -1;
			}
			token->value = PyDict_New();
			if (token->value == NULL) {
				Py_DECREF(tmp);
				return -1;
			}
			if (PyDict_SetItem(token->value, PY_STRING_BYTES, tmp) != 0) {
				Py_DECREF(tmp);
				return -1;
			}
			Py_DECREF(tmp);
		} else {
			token->value = PyBytes_FromStringAndSize((const char*)&buf[idx], info);
			if (token->value == NULL) {
				return -1;
			}
		}
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
		token->prev_key = NULL;
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
		if (str_len == 0 || str[0] != 0) {
			PyErr_SetString(PyExc_ValueError, "invalid CID");
			return -1;
		}
		if (atjson_mode) { /* wrap in {"$link", "b32..."} */
			tmp = cbrrr_bytes_to_b32_multibase(str + 1, str_len - 1); // slice off the leading 0
			if (tmp == NULL) {
				return -1;
			}
			token->value = PyDict_New();
			if (token->value == NULL) {
				Py_DECREF(tmp);
				return -1;
			}
			if (PyDict_SetItem(token->value, PY_STRING_LINK, tmp) != 0) {
				Py_DECREF(tmp);
				return -1;
			}
			Py_DECREF(tmp);
		} else {
			tmp = PyBytes_FromStringAndSize((const char*)str + 1, str_len - 1); // slice off the leading 0
			if (tmp == NULL) {
				return -1;
			}
			token->value = PyObject_CallFunctionObjArgs(cid_ctor, tmp, NULL);
			Py_DECREF(tmp);
			if (token->value == NULL) {
				return -1; // exception in cid_ctor
			}
		}

		return idx + res;
	default:
		PyErr_Format(PyExc_Exception, "you reached unreachable code??? (type=%lu)", token->type);
		return -1; // unreachable?
	}
}

static size_t
cbrrr_parse_object(const uint8_t *buf, size_t len, PyObject **value, PyObject *cid_ctor, int atjson_mode)
{
	/* The stack will get realloc'd whenever we run out (and freed on return) */
	/* stack[sp+1] is used like a local variable to hold all parsed tokens */
	size_t stack_len = 16;
	DCToken *parse_stack = malloc(stack_len * sizeof(*parse_stack));

	if (parse_stack == NULL) {
		PyErr_SetString(PyExc_MemoryError, "malloc failed");
		return -1;
	}

	/* pretend that we're parsing an array of length 1
	   (avoids needing to special-case root-level parsing) */
	parse_stack[0].type = DCMT_ARRAY;
	parse_stack[0].value = PyList_New(1);
	parse_stack[0].count = 1;

	size_t sp = 0;
	size_t idx = 0;

	/* parser stack machine thing... if it looks confusing it's because it is */

	for (;;) {
		if (parse_stack[sp].count == 0) { /* If we're done on this level of the stack */
			if (sp == 0) { /* no more stack left, parsing is complete! */
				/* pull the parsed result out of the dummy list of length 1 we created at the start */
				*value = PyList_GET_ITEM(parse_stack[0].value, 0);
				Py_XINCREF(*value); // nb: this would be cleaner with Py_XNewRef, available in py3.10+. You could probably drop the X too, I can't think why PyList_GET_ITEM would fail.
				break;
			}
			sp -= 1; /* "return" to the previous stack frame */
			continue;
		}

		if (parse_stack[sp].type == DCMT_ARRAY) { /* if we're currently parsing an array */
			size_t res = cbrrr_parse_token(&buf[idx], len-idx, &parse_stack[sp+1], cid_ctor, atjson_mode);
			if (res == (size_t)-1) {
				idx = -1;
				break;
			}
			//printf("DEBUG: token type %u, start=%lu len=%lu\n", parse_stack[sp+1].type, idx, res);
			idx += res;
			// move ownership of sp+1 into sp
			PyList_SET_ITEM(
				parse_stack[sp].value,
				PyList_GET_SIZE(parse_stack[sp].value) - parse_stack[sp].count,
				parse_stack[sp+1].value
			);
			parse_stack[sp].count -= 1;
		} else { /* if we're currently parsing a map */
			const u_int8_t *str;
			size_t str_len;
			size_t res = cbrrr_parse_raw_string(&buf[idx], len-idx, DCMT_TEXT_STRING, &str, &str_len);
			if (res == (size_t)-1) {
				// panik
				idx = -1;
				break;
			}
			idx += res;
			// check unicode validity before parsing next token to avoid leaking a reference when we bail out
			// TODO:PERF: fast-path(s) for common/short keys, via interning?
			PyObject *key = PyUnicode_FromStringAndSize((const char*)str, str_len);
			if (key == NULL) { // unicode error
				idx = -1;
				break;
			}
			if (parse_stack[sp].prev_key != NULL) { // don't check the first key
				if (str_len < parse_stack[sp].prev_key_len) { // key order violation
					// panik
					PyObject *tmp = PyUnicode_FromStringAndSize((const char*)parse_stack[sp].prev_key, parse_stack[sp].prev_key_len);
					PyErr_Format(PyExc_ValueError, "non-canonical map key ordering (len(%R) < len(%R))", key, tmp);
					Py_DECREF(tmp);
					idx = -1;
					break;
				} else if (str_len == parse_stack[sp].prev_key_len) { // ditto
					if (memcmp(str, parse_stack[sp].prev_key, str_len) <= 0) {
						PyObject *tmp = PyUnicode_FromStringAndSize((const char*)parse_stack[sp].prev_key, parse_stack[sp].prev_key_len);
						PyErr_Format(PyExc_ValueError, "non-canonical map key ordering (%R <= %R)", key, tmp);
						Py_DECREF(tmp);
						idx = -1;
						break;
					}
				}
			}
			parse_stack[sp].prev_key = str;
			parse_stack[sp].prev_key_len = str_len;

			res = cbrrr_parse_token(&buf[idx], len-idx, &parse_stack[sp+1], cid_ctor, atjson_mode);
			if (res == (size_t)-1) {
				idx = -1;
				break;
			}
			//printf("DEBUG: (map value) token type %u, start=%lu len=%lu\n", parse_stack[sp+1].type, idx, res);
			idx += res;

			// move ownership of sp+1 into sp
			if(PyDict_SetItem(parse_stack[sp].value, key, parse_stack[sp+1].value) < 0) {
				idx = -1;
				break;
			}
			Py_DECREF(key);
			Py_DECREF(parse_stack[sp+1].value);
			parse_stack[sp].count -= 1;
		}

		/* If the token we just parsed was the start of an array or map,
		   push a new stack frame, growing the stack if necessary */
		if ((parse_stack[sp+1].type == DCMT_ARRAY) || (parse_stack[sp+1].type == DCMT_MAP)) {
			sp += 1;
			if ((sp + 1) >= stack_len) {
				stack_len *= 2; // TODO:PERF: smaller increments?
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
cbrrr_decode_dag_cbor(PyObject *self, PyObject *args)
{
	Py_buffer buf;
	PyObject *cid_ctor;
	int atjson_mode;

	(void)self; // unused

	if (!PyArg_ParseTuple(args, "y*Op", &buf, &cid_ctor, &atjson_mode)) {
		return NULL;
	}

	PyObject *value = NULL;

	size_t res = cbrrr_parse_object(buf.buf, buf.len, &value, cid_ctor, atjson_mode);
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















static int
cbrrr_buf_make_room(CbrrrBuf *buf, size_t len) // sets python exception on fail
{
	while (buf->capacity - buf->length < len){
		buf->capacity = buf->capacity * 2;
		buf->buf = realloc(buf->buf, buf->capacity);
		if (buf->buf == NULL) {
			PyErr_SetString(PyExc_MemoryError, "realloc failed");
			return -1;
		}
	}
	return 0;
}

static int
cbrrr_buf_write(CbrrrBuf *buf, const uint8_t *data, size_t len)
{
	if (cbrrr_buf_make_room(buf, len) < 0) {
		return -1;
	}
	memcpy(buf->buf+buf->length, data, len);
	buf->length += len;
	return 0;
}

static int
cbrrr_write_cbor_varint(CbrrrBuf *buf, DCMajorType type, uint64_t value)
{
	uint8_t tmp[9];
	/*
	In theory, small values are more likely, so this if-chain order is probably
	optimal. However, the compiler might try to be clever and turn it into a
	binary tree. I haven't checked. It probably doesn't meaningfully matter.
	*/
	if (value < 24) {
		tmp[0] = type << 5 | value;
		return cbrrr_buf_write(buf, tmp, 1);
	}
	if (value < 0x100) {
		tmp[0] = type << 5 | 24;
		tmp[1] = value;
		return cbrrr_buf_write(buf, tmp, 2);
	}
	if (value < 0x10000) {
		tmp[0] = type << 5 | 25;
		tmp[1] = value >> 8;
		tmp[2] = value;
		return cbrrr_buf_write(buf, tmp, 3);
	}
	if (value < 0x100000000L) {
		tmp[0] = type << 5 | 26;
		tmp[1] = value >> 24;
		tmp[2] = value >> 16;
		tmp[3] = value >> 8;
		tmp[4] = value;
		return cbrrr_buf_write(buf, tmp, 5);
	}
	tmp[0] = type << 5 | 27;
	tmp[1] = value >> 56;
	tmp[2] = value >> 48;
	tmp[3] = value >> 40;
	tmp[4] = value >> 32;
	tmp[5] = value >> 24;
	tmp[6] = value >> 16;
	tmp[7] = value >> 8;
	tmp[8] = value;
	return cbrrr_buf_write(buf, tmp, 9);
}



/*
Decodes maybe-padded base64 according to https://atproto.com/specs/data-model#bytes (RFC-4648, section 4)
Returns 0 on success, -1 on failure (setting a python exception).
*/

static const uint8_t B64_DECODE_LUT[] = {
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
	52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
	-1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
	15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
	-1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
	41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

static int
cbrrr_write_cbor_bytes_from_b64(CbrrrBuf *buf, const uint8_t *b64_str, size_t str_len)
{
	// strip padding
	while (str_len && b64_str[str_len - 1] == '=') str_len--;
	if ((str_len % 4) == 1) {
		PyErr_SetString(PyExc_ValueError, "invalid b64 length");
		return -1;
	}

	/* nb: this length integer could overflow, but it comes from a python string,
	   so it should be < PY_SSIZE_T_MAX. Unless you have close to 2^63 bytes of
	   RAM, you're safe. I think the uint64 cast should make it safe
	   on 32-bit platforms too. (decoded_length is always < str_len) */
	size_t decoded_length = ((uint64_t)str_len*3)/4;
	if (cbrrr_write_cbor_varint(buf, DCMT_BYTE_STRING, decoded_length) < 0) {
		return -1;
	}
	if (cbrrr_buf_make_room(buf, decoded_length) < 0) {
		return -1;
	}
	uint8_t *bufptr = buf->buf + buf->length;
	buf->length += decoded_length;

	size_t str_i = 0;
	uint8_t a, b, c, d;
	while (str_i+3 < str_len) {
		a = B64_DECODE_LUT[b64_str[str_i++]];
		b = B64_DECODE_LUT[b64_str[str_i++]];
		c = B64_DECODE_LUT[b64_str[str_i++]];
		d = B64_DECODE_LUT[b64_str[str_i++]];
		if ((a | b | c | d) & 0x80) {
			PyErr_SetString(PyExc_ValueError, "invalid b64 character");
			return -1;
		}
		*bufptr++ = (a << 2) | (b >> 4);
		*bufptr++ = (b << 4) | (c >> 2);
		*bufptr++ = (c << 6) | (d >> 0);
	}
	switch (str_len - str_i)
	{
	case 3:
		a = B64_DECODE_LUT[b64_str[str_i++]];
		b = B64_DECODE_LUT[b64_str[str_i++]];
		c = B64_DECODE_LUT[b64_str[str_i++]];
		if ((a | b | c) & 0x80) {
			PyErr_SetString(PyExc_ValueError, "invalid b64 character");
			return -1;
		}
		*bufptr++ = (a << 2) | (b >> 4);
		*bufptr++ = (b << 4) | (c >> 2);
		// should we check (c << 6) & 0xff == 0?
		break;
	
	case 2:
		a = B64_DECODE_LUT[b64_str[str_i++]];
		b = B64_DECODE_LUT[b64_str[str_i++]];
		if ((a | b) & 0x80) {
			PyErr_SetString(PyExc_ValueError, "invalid b64 character");
			return -1;
		}
		*bufptr++ = (a << 2) | (b >> 4);
		// should we check (b << 4) & 0xff == 0?
		break;
	
	case 0:
		break;

	default:
		PyErr_SetString(PyExc_AssertionError, "unreachable!?");
		return -1;
	}

	return 0;
}


// nb: case insensitive
static const uint8_t B32_DECODE_LUT[] = {
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, 26, 27, 28, 29, 30, 31, -1, -1, -1, -1, -1, -1, -1, -1,
	-1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
	15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
	-1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
	15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};


static int
cbrrr_write_cbor_bytes_from_multibase_b32_nopad(CbrrrBuf *buf, const uint8_t *b32_str, size_t str_len)
{
	if (str_len == 0 || b32_str[0] != 'b') {
		PyErr_SetString(PyExc_ValueError, "invalid/unsupported multibase prefix");
		return -1; // multibase prefix
	}
	b32_str++;
	str_len--;

	/* nb: see comment in b64 fn above re: integer overflow */
	size_t decoded_length = ((uint64_t)str_len*5)/8;
	if (cbrrr_write_cbor_varint(buf, DCMT_BYTE_STRING, decoded_length + 1) < 0) {
		return -1;
	}
	if (cbrrr_buf_make_room(buf, decoded_length + 1) < 0) {
		return -1;
	}
	uint8_t *bufptr = buf->buf + buf->length;
	buf->length += decoded_length + 1;

	*bufptr++ = 0; // multibase raw

	size_t str_i = 0;
	uint8_t a, b, c, d, e, f, g, h;
	while (str_i+7 < str_len) {
		a = B32_DECODE_LUT[b32_str[str_i++]];
		b = B32_DECODE_LUT[b32_str[str_i++]];
		c = B32_DECODE_LUT[b32_str[str_i++]];
		d = B32_DECODE_LUT[b32_str[str_i++]];
		e = B32_DECODE_LUT[b32_str[str_i++]];
		f = B32_DECODE_LUT[b32_str[str_i++]];
		g = B32_DECODE_LUT[b32_str[str_i++]];
		h = B32_DECODE_LUT[b32_str[str_i++]];
		if ((a | b | c | d | e | f | g | h) & 0x80) {
			PyErr_SetString(PyExc_ValueError, "invalid b32 character");
			return -1;
		}
		// 43210432 10432104 32104321 04321043 21043210
		// aaaaabbb bbcccccd ddddeeee efffffgg ggghhhhh
		// 76543210 76543210 76543210 76543210 76543210
		*bufptr++ =            (a << 3) | (b >> 2);
		*bufptr++ = (b << 6) | (c << 1) | (d >> 4);
		*bufptr++ = (d << 4) |            (e >> 1);
		*bufptr++ = (e << 7) | (f << 2) | (g >> 3);
		*bufptr++ = (g << 5) | (h << 0)           ;
	}
	switch (str_len - str_i)
	{
	case 7:
		a = B32_DECODE_LUT[b32_str[str_i++]];
		b = B32_DECODE_LUT[b32_str[str_i++]];
		c = B32_DECODE_LUT[b32_str[str_i++]];
		d = B32_DECODE_LUT[b32_str[str_i++]];
		e = B32_DECODE_LUT[b32_str[str_i++]];
		f = B32_DECODE_LUT[b32_str[str_i++]];
		g = B32_DECODE_LUT[b32_str[str_i++]];
		if ((a | b | c | d | e | f | g) & 0x80) {
			PyErr_SetString(PyExc_ValueError, "invalid b32 character");
			return -1;
		}
		*bufptr++ =            (a << 3) | (b >> 2);
		*bufptr++ = (b << 6) | (c << 1) | (d >> 4);
		*bufptr++ = (d << 4) |            (e >> 1);
		*bufptr++ = (e << 7) | (f << 2) | (g >> 3);
		if (g & 0x07) {
			PyErr_SetString(PyExc_ValueError, "non-canonical b32 encoding");
			return -1;
		}
		break;
	case 5:
		a = B32_DECODE_LUT[b32_str[str_i++]];
		b = B32_DECODE_LUT[b32_str[str_i++]];
		c = B32_DECODE_LUT[b32_str[str_i++]];
		d = B32_DECODE_LUT[b32_str[str_i++]];
		e = B32_DECODE_LUT[b32_str[str_i++]];
		if ((a | b | c | d | e) & 0x80) {
			PyErr_SetString(PyExc_ValueError, "invalid b32 character");
			return -1;
		}
		*bufptr++ =            (a << 3) | (b >> 2);
		*bufptr++ = (b << 6) | (c << 1) | (d >> 4);
		*bufptr++ = (d << 4) |            (e >> 1);
		if (e & 0x01) {
			PyErr_SetString(PyExc_ValueError, "non-canonical b32 encoding");
			return -1;
		}
		break;
	case 4:
		a = B32_DECODE_LUT[b32_str[str_i++]];
		b = B32_DECODE_LUT[b32_str[str_i++]];
		c = B32_DECODE_LUT[b32_str[str_i++]];
		d = B32_DECODE_LUT[b32_str[str_i++]];
		if ((a | b | c | d) & 0x80) {
			PyErr_SetString(PyExc_ValueError, "invalid b32 character");
			return -1;
		}
		*bufptr++ =            (a << 3) | (b >> 2);
		*bufptr++ = (b << 6) | (c << 1) | (d >> 4);
		if (d & 0x0f) {
			PyErr_SetString(PyExc_ValueError, "non-canonical b32 encoding");
			return -1;
		}
		break;
	case 2:
		a = B32_DECODE_LUT[b32_str[str_i++]];
		b = B32_DECODE_LUT[b32_str[str_i++]];
		if ((a | b) & 0x80) {
			PyErr_SetString(PyExc_ValueError, "invalid b32 character");
			return -1;
		}
		*bufptr++ =            (a << 3) | (b >> 2);
		if (b & 0x03) {
			PyErr_SetString(PyExc_ValueError, "non-canonical b32 encoding");
			return -1;
		}
		break;
	
	case 0:
		// nothing to do here
		break;

	case 1:
	case 3:
	case 6:
		PyErr_SetString(PyExc_ValueError, "invalid b32 length");
		return -1;

	default:
		PyErr_SetString(PyExc_AssertionError, "unreachable!?");
		return -1;
	}

	return 0;
}



static int
cbrrr_compare_map_keys(const void *a, const void *b)
{
	// nb: the comparison needs to be performed on the byte representations of the strings

	PyObject *obj_a = *(PyObject**)a;
	PyObject *obj_b = *(PyObject**)b;
	Py_ssize_t len_a, len_b;
	const char *str_a = PyUnicode_AsUTF8AndSize(obj_a, &len_a);
	const char *str_b = PyUnicode_AsUTF8AndSize(obj_b, &len_b);

	/* Handle the (invalid!) case where one or both args are not strings
	   (they'll get properly type-checked later, we don't have a good way
	   to raise an exception from within qsort) */
	if (str_a == NULL || str_b == NULL) {
		/* this logic is here to make sure the comparison fn is transitive,
		   lest we invoke UB, as in
		   https://www.openwall.com/lists/oss-security/2024/01/30/7 */
		if (str_a == NULL && str_b == NULL) {
			return 0;
		}
		return str_a == NULL ? -1 : 1; /* non-strings sort first */
	}

	/* shorter strings sort first */
	if (len_a < len_b) {
		return -1;
	}
	if (len_a > len_b) {
		return 1;
	}
	
	/* do byte-wise comparision for equal-length strings */
	return memcmp(str_a, str_b, len_a);
}


static int
cbrrr_encode_object(CbrrrBuf *buf, PyObject *obj_in, PyObject* cid_type, int atjson_mode)
{
	/*
	in a slightly unscientific test, frequency counts for each type
	in an atproto repo looked like this:

	233805 str
	135608 bytes
	88726 cid
	61775 dict
	49079 int
	36341 list
	12585 none
	2 bool
	0 float
	*/

	size_t stack_len = 16;
	EncoderStackFrame *encoder_stack = malloc(stack_len * sizeof(*encoder_stack));

	if (encoder_stack == NULL) {
		PyErr_SetString(PyExc_MemoryError, "malloc failed");
		return -1;
	}

	encoder_stack[0].dict = NULL;
	encoder_stack[0].list = NULL;
	encoder_stack[0].idx = 0;

	size_t sp = 0;

	int res = -1; // assume failure by default

	for (;;) {
		// make sure there's always at least 1 free slot at the top of the stack
		if ((sp + 1) >= stack_len) {
			stack_len *= 2; // TODO:PERF: smaller increments?
			encoder_stack = realloc(encoder_stack, stack_len * sizeof(*encoder_stack));
			if (encoder_stack == NULL) {
				PyErr_SetString(PyExc_MemoryError, "realloc failed");
				break;
			}
		}

		PyObject *obj;
		if (sp == 0) { // we're at the root level
			if (encoder_stack[sp].idx > 0) {
				res = 0;
				break; // we're done!
			}
			encoder_stack[sp].idx++;
			obj = obj_in;
		} else if (encoder_stack[sp].dict == NULL) { // we're working on a list
			if (encoder_stack[sp].idx >= PySequence_Fast_GET_SIZE(encoder_stack[sp].list)) {
				sp--;
				continue;
			}
			obj = PySequence_Fast_GET_ITEM(encoder_stack[sp].list, encoder_stack[sp].idx++); // borrowed ref
		} else { // we're working on a dict
			if (encoder_stack[sp].idx >= PySequence_Fast_GET_SIZE(encoder_stack[sp].list)) {
				Py_DECREF(encoder_stack[sp].list);
				sp--;
				continue;
			}
			PyObject *key = PySequence_Fast_GET_ITEM(encoder_stack[sp].list, encoder_stack[sp].idx++); // borrowed ref
			if (!PyUnicode_CheckExact(key)) {
				PyErr_SetString(PyExc_TypeError, "map keys must be strings");
				break;
			}
			Py_ssize_t key_len;
			const char *key_str = PyUnicode_AsUTF8AndSize(key, &key_len);
			if (key_str == NULL) {
				break;
			}
			if (cbrrr_write_cbor_varint(buf, DCMT_TEXT_STRING, key_len) < 0) {
				break;
			}
			if (cbrrr_buf_write(buf, (uint8_t *)key_str, key_len) < 0) {
				break;
			}
			obj = PyDict_GetItem(encoder_stack[sp].dict, key); // borrwed ref
		}

		
		PyTypeObject *obj_type = Py_TYPE(obj);

		if (obj_type == &PyUnicode_Type) { // string
			Py_ssize_t string_len;
			const char *str = PyUnicode_AsUTF8AndSize(obj, &string_len);
			if (str == NULL) {
				break;
			}
			if (cbrrr_write_cbor_varint(buf, DCMT_TEXT_STRING, string_len) < 0) {
				break;
			}
			if (cbrrr_buf_write(buf, (uint8_t *)str, string_len) < 0) {
				break;
			}
			continue;
		}
		if (obj_type == &PyBytes_Type) { // bytes
			if (atjson_mode) {
				PyErr_SetString(PyExc_TypeError, "unexpected bytes object in atjson mode");
				break;
			}
			Py_ssize_t bytes_len;
			char *bbuf;
			if(PyBytes_AsStringAndSize(obj, &bbuf, &bytes_len) != 0) {
				break;
			}
			if (cbrrr_write_cbor_varint(buf, DCMT_BYTE_STRING, bytes_len) < 0) {
				break;
			}
			if (cbrrr_buf_write(buf, (uint8_t*)bbuf, bytes_len) < 0) {
				break;
			}
			continue;
		}
		if (obj_type == (PyTypeObject*)cid_type) { // cid
			if (atjson_mode) {
				PyErr_SetString(PyExc_TypeError, "unexpected CID object in atjson mode");
				break;
			}
			PyObject *cidbytes_obj = PyObject_CallMethod(obj, "__bytes__", NULL);
			if (cidbytes_obj == NULL) {
				break;
			}
			Py_ssize_t bytes_len;
			char *bbuf;
			const uint8_t nul = 0;
			if(PyBytes_AsStringAndSize(cidbytes_obj, &bbuf, &bytes_len) != 0) {
				Py_DECREF(cidbytes_obj);
				break;
			}
			/*if (bytes_len != 36) {
				PyErr_SetString(PyExc_ValueError, "Invalid CID length");
				Py_DECREF(cidbytes_obj);
				break;
			}*/
			if (cbrrr_write_cbor_varint(buf, DCMT_TAG, 42) < 0) {
				Py_DECREF(cidbytes_obj);
				break;
			}
			if (cbrrr_write_cbor_varint(buf, DCMT_BYTE_STRING, bytes_len + 1) < 0) {
				Py_DECREF(cidbytes_obj);
				break;
			}
			if (cbrrr_buf_write(buf, &nul, sizeof(nul)) < 0) {
				Py_DECREF(cidbytes_obj);
				break;
			}
			if (cbrrr_buf_write(buf, (uint8_t*)bbuf, bytes_len) < 0) {
				Py_DECREF(cidbytes_obj);
				break;
			}
			Py_DECREF(cidbytes_obj);
			continue;
		}
		if (obj_type == &PyDict_Type) { // dict
			PyObject *keys = PyDict_Keys(obj);
			if (keys == NULL) {
				break;
			}
			if (atjson_mode && PySequence_Fast_GET_SIZE(keys) == 1) {// logic for $link, $bytes
				PyObject *key = PySequence_Fast_GET_ITEM(keys, 0);
				if (!PyUnicode_CheckExact(key)) {
					PyErr_SetString(PyExc_TypeError, "map keys must be strings");
					Py_DECREF(keys);
					break;
				}
				Py_ssize_t string_len;
				const char *str = PyUnicode_AsUTF8AndSize(key, &string_len); // does this fail gracefully if the item is not a string?
				if (str == NULL) {
					Py_DECREF(keys);
					break;
				}
				if (string_len == 5 && strcmp(str, "$link") == 0) { // CID
					PyObject *cid_str = PyDict_GetItem(obj, key); // borrowed
					Py_DECREF(keys);
					if (!PyUnicode_CheckExact(cid_str)) { // also handles the case where b64_str is NULL
						PyErr_SetString(PyExc_TypeError, "$link field value must be a string");
						break;
					}
					str = PyUnicode_AsUTF8AndSize(cid_str, &string_len); // reusing these variables
					if (str == NULL) {
						break;
					}
					if (cbrrr_write_cbor_varint(buf, DCMT_TAG, 42) < 0) {
						break;
					}
					if (cbrrr_write_cbor_bytes_from_multibase_b32_nopad(buf, (uint8_t*)str, string_len) < 0) {
						break;
					}
					continue;
				}
				if (string_len == 6 && strcmp(str, "$bytes") == 0) { // bytes
					PyObject *b64_str = PyDict_GetItem(obj, key); // borrowed
					Py_DECREF(keys);
					if (!PyUnicode_CheckExact(b64_str)) { // also handles the case where b64_str is NULL
						PyErr_SetString(PyExc_TypeError, "$bytes field value must be a string");
						break;
					}
					str = PyUnicode_AsUTF8AndSize(b64_str, &string_len); // reusing these variables
					if (str == NULL) {
						break;
					}
					if (cbrrr_write_cbor_bytes_from_b64(buf, (uint8_t*)str, string_len) < 0) {
						break;
					}
					continue;
				}
				// fallthru
			}
			if (PySequence_Fast_GET_SIZE(keys) > 1) { /* don't try to sort empty or 1-length lists! */
				qsort( // it's a bit janky but we can sort the key list in-place, I think?
					PySequence_Fast_ITEMS(keys),
					PySequence_Fast_GET_SIZE(keys),
					sizeof(PyObject*),
					cbrrr_compare_map_keys
				);
			}
			if (cbrrr_write_cbor_varint(buf, DCMT_MAP, PySequence_Fast_GET_SIZE(keys)) < 0) {
				Py_DECREF(keys);
				break;
			}
			sp++;
			encoder_stack[sp].dict = obj;
			encoder_stack[sp].list = keys;
			encoder_stack[sp].idx = 0;
			continue;
		}
		if (obj_type ==  &PyLong_Type) { // int
			// we can't really do the range checks on the C side because the
			// overflow would happen before we can detect it.
			if (PyObject_RichCompareBool(obj, PY_ZERO, Py_GE)) {
				if (PyObject_RichCompareBool(obj, PY_UINT64_MAX, Py_GT)) {
					PyErr_SetString(PyExc_ValueError, "integer out of range");
					break;
				}
				if (cbrrr_write_cbor_varint(buf, DCMT_UNSIGNED_INT, PyLong_AsUnsignedLongLongMask(obj)) < 0) {
					break;
				}
			} else {
				if (PyObject_RichCompareBool(obj, PY_UINT64_MAX_INVERTED, Py_LT)) {
					PyErr_SetString(PyExc_ValueError, "integer out of range");
					break;
				}
				if (cbrrr_write_cbor_varint(buf, DCMT_NEGATIVE_INT, ~PyLong_AsUnsignedLongLongMask(obj)) < 0) {
					break;
				}
			}
			continue;
		}
		if (obj_type == &PyList_Type) {
			if (cbrrr_write_cbor_varint(buf, DCMT_ARRAY, PySequence_Fast_GET_SIZE(obj)) < 0) {
				break;
			}
			sp++;
			encoder_stack[sp].dict = NULL;
			encoder_stack[sp].list = obj;
			encoder_stack[sp].idx = 0;
			continue;
		}
		if (obj == Py_None) { // none/null
			if (cbrrr_write_cbor_varint(buf, DCMT_FLOAT, 22) < 0) {
				break;
			}
			continue;
		}
		if (obj_type == &PyBool_Type) { // bool
			if (cbrrr_write_cbor_varint(buf, DCMT_FLOAT, 20 + (obj == Py_True)) < 0) {
				break;
			}
			continue;
		}
		if (obj_type == &PyFloat_Type) {
			double doubleval = PyFloat_AS_DOUBLE(obj);
			if (isnan(doubleval)) {
				PyErr_SetString(PyExc_ValueError, "NaNs are not allowed");
				break;
			}
			if (isinf(doubleval)) {
				PyErr_SetString(PyExc_ValueError, "+/-Infinities are not allowed");
				break;
			}

			// we can't use cbrrr_write_cbor_varint because it'd use the wrong sizes
			uint8_t tmp = DCMT_FLOAT << 5 | 27;
			if (cbrrr_buf_write(buf, &tmp, sizeof(tmp)) < 0) {
				break;
			}
			uint64_t dub_int = ((union {uint64_t num; double dub;}){.dub=doubleval}).num;

			/* hacky in-place endian-swap, the compiler should know what to do */
			dub_int = ((dub_int <<  8) & 0xFF00FF00FF00FF00ULL ) | ((dub_int >>  8) & 0x00FF00FF00FF00FFULL );
			dub_int = ((dub_int << 16) & 0xFFFF0000FFFF0000ULL ) | ((dub_int >> 16) & 0x0000FFFF0000FFFFULL );
			dub_int = (dub_int << 32) | ((dub_int >> 32) & 0xFFFFFFFFULL);

			if (cbrrr_buf_write(buf, (uint8_t*)&dub_int, sizeof(dub_int)) < 0) {
				break;
			}
			continue;
		}

		PyErr_Format(PyExc_TypeError, "I don't know how to encode type %R", obj_type);
		break;
	}

	// if we bailed out due to error, there might be some dict key lists left over on the stack
	for (size_t i=1; i<=sp; i++) {
		if (encoder_stack[i].dict != NULL) {
			Py_DECREF(encoder_stack[i].list);
		}
	}

	free(encoder_stack);
	return res;
}




static PyObject *
cbrrr_encode_dag_cbor(PyObject *self, PyObject *args)
{
	PyObject *obj;
	PyObject *cid_type;
	PyObject *res;
	int atjson_mode;
	CbrrrBuf buf;

	(void)self; // unused

	if (!PyArg_ParseTuple(args, "OOp", &obj, &cid_type, &atjson_mode)) {
		return NULL;
	}

	buf.length = 0;
	buf.capacity = 0x400; // TODO:PERF: tune this?
	buf.buf = malloc(buf.capacity);
	if (buf.buf == NULL) {
		PyErr_SetString(PyExc_MemoryError, "malloc failed");
		return NULL;
	}

	if (cbrrr_encode_object(&buf, obj, cid_type, atjson_mode) < 0) {
		res = NULL;
	} else {
		res = PyBytes_FromStringAndSize((const char*)buf.buf, buf.length); // nb: this incurs a copy
	}

	free(buf.buf);
	return res;
}









static PyMethodDef CbrrrMethods[] = {
	{"decode_dag_cbor", cbrrr_decode_dag_cbor, METH_VARARGS,
		"parse a buffer of DAG-CBOR into python objects"},
	{"encode_dag_cbor", cbrrr_encode_dag_cbor, METH_VARARGS,
		"convert a python object into DAG-CBOR bytes"},
	{NULL, NULL, 0, NULL}        /* Sentinel */
};

static struct PyModuleDef cbrrrmodule = {
	PyModuleDef_HEAD_INIT,
	"_cbrrr",  /* name of module */
	NULL,      /* module documentation, may be NULL */
	-1,        /* size of per-interpreter state of the module,
	              or -1 if the module keeps state in global variables. */
	CbrrrMethods,
	NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC
PyInit__cbrrr(void)
{
	PyObject *m;

	m = PyModule_Create(&cbrrrmodule);
	if (m == NULL) {
		return NULL;
	}

	PY_ZERO = PyLong_FromLong(0);
	PY_UINT64_MAX = PyLong_FromUnsignedLongLong(UINT64_MAX);
	PY_UINT64_MAX_INVERTED = PyNumber_Invert(PY_UINT64_MAX);
	PY_STRING_ENCODE = PyUnicode_InternFromString("encode");
	PY_STRING_DECODE = PyUnicode_InternFromString("decode");
	PY_STRING_LINK = PyUnicode_InternFromString("$link");
	PY_STRING_BYTES = PyUnicode_InternFromString("$bytes");
	if (
		   PY_ZERO == NULL
		|| PY_UINT64_MAX == NULL
		|| PY_UINT64_MAX_INVERTED == NULL
		|| PY_STRING_ENCODE == NULL
		|| PY_STRING_DECODE == NULL
		|| PY_STRING_LINK == NULL
		|| PY_STRING_BYTES == NULL
	) {
		Py_XDECREF(PY_ZERO);
		Py_XDECREF(PY_UINT64_MAX);
		Py_XDECREF(PY_UINT64_MAX_INVERTED);
		Py_XDECREF(PY_STRING_ENCODE);
		Py_XDECREF(PY_STRING_DECODE);
		Py_XDECREF(PY_STRING_LINK);
		Py_XDECREF(PY_STRING_BYTES);
		return NULL;
	}

	return m;
}
