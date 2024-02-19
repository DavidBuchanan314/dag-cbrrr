#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <string.h>
#include <stdint.h>

PyObject *PY_ZERO;
PyObject *PY_UINT64_MAX;
PyObject *PY_UINT64_MAX_INVERTED;

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
	size_t idx; // the current list index
} EncoderStackFrame;

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
			double doubleval = ((union {uint64_t num; double dub;}){.num=be64toh(*(uint64_t*)&buf[idx])}).dub; // TODO: rewrite lol
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
		Py_DECREF(tmp); // XXX
		if (token->value == NULL) {
			return -1;
		}
		return idx;
	case DCMT_BYTE_STRING:
		if (info > len - idx) {
			PyErr_SetString(PyExc_EOFError, "not enough bytes left in buffer");
			return -1;
		}
		token->value = PyBytes_FromStringAndSize((const char *)&buf[idx], info);
		if (token->value == NULL) {
			return -1;
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
		tmp = PyBytes_FromStringAndSize((const char*)str, str_len);
		if (tmp == NULL) {
			return -1;
		}
		token->value = PyObject_CallOneArg(cid_ctor, tmp);
		Py_DECREF(tmp);
		if (token->value == NULL) {
			return -1; // exception in cid_ctor
		}

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
			//printf("DEBUG: token type %u, start=%lu len=%lu\n", parse_stack[sp+1].type, idx, res);
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
			// check unicode validity before parsing next token to avoid leaking a reference when we bail out
			// TODO: fast-path(s) for common/short keys
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

			res = cbrrr_parse_token(&buf[idx], len-idx, &parse_stack[sp+1], cid_ctor);
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

	if (!PyArg_ParseTuple(args, "y*O", &buf, &cid_ctor)) {
		return NULL;
	}

	PyObject *value = NULL;

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











static int
cbrrr_buf_make_room(CbrrrBuf *buf, size_t len)
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

static int
cbrrr_compare_map_keys(const void *a, const void *b)
{
	// XXX: we shouldn't really be assuming that they're strings here
	// nb: the comparison needs to be performed on the byte representations of the strings

	PyObject *obj_a = *(PyObject**)a;
	PyObject *obj_b = *(PyObject**)b;
	size_t len_a, len_b;
	const uint8_t *str_a = PyUnicode_AsUTF8AndSize(obj_a, &len_a);
	const uint8_t *str_b = PyUnicode_AsUTF8AndSize(obj_b, &len_b);
	if (str_a == NULL || str_b == NULL) {
		return 0; // not sure this is really valid?
	}
	if (len_a < len_b) {
		return -1;
	}
	if (len_a > len_b) {
		return 1;
	}
	// len_a == len_b
	return memcmp(str_a, str_b, len_a);
}


static int
cbrrr_encode_object(CbrrrBuf *buf, PyObject *obj_in, PyObject* cid_type)
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

	// pretend we're encoding a list of length 1
	encoder_stack[0].dict = NULL;
	encoder_stack[0].list = PyList_New(1);
	if (encoder_stack[0].list == NULL) {
		return -1;
	}
	PyList_SET_ITEM(encoder_stack[0].list, 0, Py_NewRef(obj_in));
	encoder_stack[0].idx = 0;

	size_t sp = 0;

	int res = -1; // assume failure by default

	for (;;) {
		// make sure there's always at least 1 free slot at the top of the stack
		// TODO: rework the parser state machine to be like this too?
		if ((sp + 1) >= stack_len) {
			stack_len *= 2; // TODO: smaller increments?
			encoder_stack = realloc(encoder_stack, stack_len * sizeof(*encoder_stack));
			if (encoder_stack == NULL) {
				PyErr_SetString(PyExc_MemoryError, "realloc failed");
				break;
			}
		}

		PyObject *obj;
		if (encoder_stack[sp].dict == NULL) { // we're working on a list
			if (encoder_stack[sp].idx >= PySequence_Fast_GET_SIZE(encoder_stack[sp].list)) {
				if (sp == 0) {
					res = 0; // success!
					break;
				}
				sp--;
				continue;
			}
			obj = PySequence_Fast_GET_ITEM(encoder_stack[sp].list, encoder_stack[sp].idx++); // borrowed ref
		} else { // we're working on a dict
			if (encoder_stack[sp].idx >= PySequence_Fast_GET_SIZE(encoder_stack[sp].list)) {
				Py_DECREF(&encoder_stack[sp].list);
				sp--;
				continue;
			}
			PyObject *key = PySequence_Fast_GET_ITEM(encoder_stack[sp].list, encoder_stack[sp].idx++); // borrowed ref
			if (!PyUnicode_CheckExact(key)) {
				PyErr_SetString(PyExc_TypeError, "map keys must be strings");
				break;
			}
			size_t key_len;
			const uint8_t *key_str = PyUnicode_AsUTF8AndSize(key, &key_len);
			if (key_str == NULL) {
				break;
			}
			if (cbrrr_write_cbor_varint(buf, DCMT_TEXT_STRING, key_len) < 0) {
				break;
			}
			if (cbrrr_buf_write(buf, key_str, key_len) < 0) {
				break;
			}
			obj = PyDict_GetItem(encoder_stack[sp].dict, key); // borrwed ref
		}
		PyTypeObject *obj_type = Py_TYPE(obj);

		if (obj_type == &PyUnicode_Type) { // string
			size_t string_len;
			const uint8_t *str = PyUnicode_AsUTF8AndSize(obj, &string_len);
			if (str == NULL) {
				break;
			}
			if (cbrrr_write_cbor_varint(buf, DCMT_TEXT_STRING, string_len) < 0) {
				break;
			}
			if (cbrrr_buf_write(buf, str, string_len) < 0) {
				break;
			}
			continue;
		}
		if (obj_type == &PyBytes_Type) { // bytes
			size_t bytes_len;
			const uint8_t *bbuf;
			if(PyBytes_AsStringAndSize(obj, &bbuf, &bytes_len) != 0) {
				break;
			}
			if (cbrrr_write_cbor_varint(buf, DCMT_BYTE_STRING, bytes_len) < 0) {
				break;
			}
			if (cbrrr_buf_write(buf, bbuf, bytes_len) < 0) {
				break;
			}
			continue;
		}
		if (obj_type == cid_type) { // cid
			PyObject *cidbytes_obj = PyObject_CallMethod(obj, "__bytes__", NULL);
			if (cidbytes_obj == NULL) {
				break;
			}
			size_t bytes_len;
			const uint8_t *bbuf, nul=0;
			if(PyBytes_AsStringAndSize(cidbytes_obj, &bbuf, &bytes_len) != 0) {
				Py_DECREF(&cidbytes_obj);
				break;
			}
			if (bytes_len != 36) {
				PyErr_SetString(PyExc_ValueError, "Invalid CID length");
				Py_DECREF(&cidbytes_obj);
				break;
			}
			if (cbrrr_write_cbor_varint(buf, DCMT_TAG, 42) < 0) {
				Py_DECREF(&cidbytes_obj);
				break;
			}
			if (cbrrr_write_cbor_varint(buf, DCMT_BYTE_STRING, bytes_len + 1) < 0) {
				Py_DECREF(&cidbytes_obj);
				break;
			}
			if (cbrrr_buf_write(buf, &nul, 1) < 0) {
				Py_DECREF(&cidbytes_obj);
				break;
			}
			if (cbrrr_buf_write(buf, bbuf, bytes_len) < 0) {
				Py_DECREF(&cidbytes_obj);
				break;
			}
			Py_DECREF(&cidbytes_obj);
			continue;
		}
		if (obj_type == &PyDict_Type) { // dict
			PyObject *keys = PyDict_Keys(obj);
			if (keys == NULL) {
				break;
			}
			qsort( // it's a bit janky but we can sort the key list in-place, I think?
				PySequence_Fast_ITEMS(keys),
				PySequence_Fast_GET_SIZE(keys),
				sizeof(PyObject*),
				cbrrr_compare_map_keys
			);
			if (cbrrr_write_cbor_varint(buf, DCMT_MAP, PySequence_Fast_GET_SIZE(keys)) < 0) {
				Py_DECREF(&keys);
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

		PyErr_Format(PyExc_TypeError, "I don't know how to encode type %R", obj_type);
		break;
	}

	Py_DECREF(&encoder_stack[0].list); // the one we created for ourselves

	free(encoder_stack);
	return res;
}



static PyObject *
cbrrr_encode_dag_cbor(PyObject *self, PyObject *args)
{
	PyObject *obj;
	PyObject *cid_type;
	PyObject *res;
	CbrrrBuf buf;

	(void)self; // unused

	if (!PyArg_ParseTuple(args, "OO", &obj, &cid_type)) {
		return NULL;
	}

	buf.length = 0;
	buf.capacity = 0x400; // TODO: tune this?
	buf.buf = malloc(buf.capacity);
	if (buf.buf == NULL) {
		PyErr_SetString(PyExc_MemoryError, "malloc failed");
		return NULL;
	}

	if (cbrrr_encode_object(&buf, obj, cid_type) < 0) {
		res = NULL;
	} else {
		res = PyBytes_FromStringAndSize((const char*)buf.buf, buf.length);
	}

	free(buf.buf);

	return res;
}









static PyMethodDef CbrrrMethods[] = {
	{"parse_dag_cbor", cbrrr_parse_dag_cbor, METH_VARARGS,
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
	if (PY_ZERO == NULL) {
		return NULL;
	}

	PY_UINT64_MAX = PyLong_FromUnsignedLongLong(UINT64_MAX);
	if (PY_UINT64_MAX == NULL) {
		Py_DECREF(&PY_ZERO);
		return NULL;
	}

	PY_UINT64_MAX_INVERTED = PyNumber_Invert(PY_UINT64_MAX);
	if (PY_UINT64_MAX_INVERTED == NULL) {
		Py_DECREF(&PY_ZERO);
		Py_DECREF(&PY_UINT64_MAX_INVERTED);
		return NULL;
	}

	return m;
}
