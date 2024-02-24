import unittest
from enum import Enum
import math
import cbrrr

class MajorType(Enum):
	UNSIGNED_INT = 0
	NEGATIVE_INT = 1
	BYTE_STRING = 2
	TEXT_STRING = 3
	ARRAY = 4
	MAP = 5
	TAG = 6
	FLOAT = 7


def cbor_head(mtype, info):
	if type(mtype) is MajorType:
		mtype = mtype.value
	assert(mtype < 8)
	assert(info < 32)
	return bytes([mtype << 5 | info])


def roundrip(obj, atjson_mode=False):
	return cbrrr.decode_dag_cbor( cbrrr.encode_dag_cbor(obj, atjson_mode) )

class EncodeTestCase(unittest.TestCase):
	def test_simple_roundtrips(self):
		self.assertEqual(roundrip(123), 123)
		self.assertEqual(roundrip(-123), -123)
		self.assertEqual(roundrip(3.1415), 3.1415)
		self.assertEqual(roundrip("hello"), "hello")
		self.assertEqual(roundrip(b"world"), b"world")
		self.assertEqual(roundrip(True), True)
		self.assertEqual(roundrip(False), False)
		self.assertEqual(roundrip(None), None)
		self.assertEqual(roundrip(cbrrr.CID(b"\x01q\x12 "+b"A"*32)), cbrrr.CID(b"\x01q\x12 "+b"A"*32))
		self.assertEqual(roundrip([1, 2, 3]), [1, 2, 3])
		self.assertEqual(roundrip({"a": 1, "b": 2}), {"a": 1, "b": 2})
		self.assertEqual(roundrip(0xffffffffffffffff), 0xffffffffffffffff)
		self.assertEqual(roundrip(~0xffffffffffffffff), ~0xffffffffffffffff)
	
	def test_decode_small_integers(self):
		self.assertEqual(cbrrr.decode_dag_cbor(cbor_head(MajorType.UNSIGNED_INT, 0)), 0)
		self.assertEqual(cbrrr.decode_dag_cbor(cbor_head(MajorType.UNSIGNED_INT, 1)), 1)
		self.assertEqual(cbrrr.decode_dag_cbor(cbor_head(MajorType.UNSIGNED_INT, 22)), 22)
		self.assertEqual(cbrrr.decode_dag_cbor(cbor_head(MajorType.UNSIGNED_INT, 23)), 23)
	
	def test_encode_toobig_integers(self):
		self.assertRaises(ValueError, cbrrr.encode_dag_cbor, 0xffffffffffffffff + 1)
		self.assertRaises(ValueError, cbrrr.encode_dag_cbor, ~(0xffffffffffffffff + 1))
	
	def test_encode_illegal_floats(self):
		self.assertRaises(ValueError, cbrrr.encode_dag_cbor, math.nan)
		self.assertRaises(ValueError, cbrrr.encode_dag_cbor, math.inf)
		self.assertRaises(ValueError, cbrrr.encode_dag_cbor, -math.inf)

	def test_decode_invalid_small_integers(self):
		for i in range(24, 28):
			self.assertRaises(EOFError, cbrrr.decode_dag_cbor, cbor_head(MajorType.UNSIGNED_INT, i))
		for i in range(28, 32):
			self.assertRaises(ValueError, cbrrr.decode_dag_cbor, cbor_head(MajorType.UNSIGNED_INT, i))
	
	def test_atjson_decode(self):
		self.assertEqual(cbrrr.decode_dag_cbor(cbrrr.encode_dag_cbor(cbrrr.CID(b"blah")), atjson_mode=True), {'$link': 'bmjwgc2a'})

		self.assertEqual(cbrrr.decode_dag_cbor(cbrrr.encode_dag_cbor(b"hello"), atjson_mode=True), {'$bytes': 'aGVsbG8'})
		self.assertEqual(cbrrr.decode_dag_cbor(cbrrr.encode_dag_cbor(b""),      atjson_mode=True), {'$bytes': ''})
		self.assertEqual(cbrrr.decode_dag_cbor(cbrrr.encode_dag_cbor(b"A"),     atjson_mode=True), {'$bytes': 'QQ'})
		self.assertEqual(cbrrr.decode_dag_cbor(cbrrr.encode_dag_cbor(b"AB"),    atjson_mode=True), {'$bytes': 'QUI'})
		self.assertEqual(cbrrr.decode_dag_cbor(cbrrr.encode_dag_cbor(b"ABC"),   atjson_mode=True), {'$bytes': 'QUJD'})
		self.assertEqual(cbrrr.decode_dag_cbor(cbrrr.encode_dag_cbor(b"ABCD"),  atjson_mode=True), {'$bytes': 'QUJDRA'})

	def test_atjson_encode_rejects_bytes_and_cid(self):
		self.assertRaises(TypeError, cbrrr.encode_dag_cbor, b"hello", True)
		self.assertRaises(TypeError, cbrrr.encode_dag_cbor, cbrrr.CID(b"blah"), True)

	def test_multi_decode(self):
		self.assertEqual(
			list(cbrrr.decode_multi_dag_cbor_in_violation_of_the_spec(
				cbrrr.encode_dag_cbor(b"hello") +
				cbrrr.encode_dag_cbor({"world": 0}) +
				cbrrr.encode_dag_cbor([1, 2, 3])
			)),
			[b'hello', {"world": 0}, [1, 2, 3]]
		)

if __name__ == '__main__':
	unittest.main(module="tests.test_cbrrr")
