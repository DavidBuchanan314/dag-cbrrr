from typing import Any, BinaryIO
from functools import cmp_to_key
from enum import Enum
import base64
import io
import _cbrrr


class CID:
	__slots__ = ("cid_bytes",)

	def __init__(self, cid_bytes: bytes) -> None:
		"""
		This currently only supports the CID types found in atproto.
		"""
		if not (cid_bytes.startswith(b"\x00\x01q\x12 ") or cid_bytes.startswith(b'\x00\x01U\x12 ')):
			raise ValueError("unsupported CID type")
		self.cid_bytes = cid_bytes[1:]
	
	def encode(self) -> str:
		"""
		Encode to base32
		"""
		return "b" + base64.b32encode(self.cid_bytes).decode().lower().rstrip("=")

	def __bytes__(self):
		return self.cid_bytes

	def __repr__(self):
		return f"CID({self.cid_bytes.hex()})"
	
	def __hash__(self) -> int:
		return self.cid_bytes.__hash__()
	
	def __eq__(self, __value: object) -> bool:
		if not isinstance(__value, CID):
			return False
		return self.cid_bytes == __value.cid_bytes

def parse_dag_cbor(data: bytes) -> Any:
	parsed, length = _cbrrr.parse_dag_cbor(data, CID)
	if length != len(data):
		raise ValueError("did not parse to end of buffer")
	return parsed

"""
everything below here should be considered temporary
"""




class _CborMajorType(Enum):
	INTEGER = 0
	NEGATIVE_INTEGER = 1
	BYTE_STRING = 2
	TEXT_STRING = 3
	ARRAY = 4
	MAP = 5
	TAG = 6
	SIMPLE = 7 # aka float, but we don't support floats


def _write_dag_cbor_varint(stream: BinaryIO, major_type: _CborMajorType, value: int) -> None:
	if value < 24:
		stream.write(bytes([major_type.value << 5 | value]))
	elif value < (1<<8):
		stream.write(bytes([major_type.value << 5 | 24]))
		stream.write(value.to_bytes(1, "big"))
	elif value < (1<<16):
		stream.write(bytes([major_type.value << 5 | 25]))
		stream.write(value.to_bytes(2, "big"))
	elif value < (1<<32):
		stream.write(bytes([major_type.value << 5 | 26]))
		stream.write(value.to_bytes(4, "big"))
	else:
		stream.write(bytes([major_type.value << 5 | 27]))
		stream.write(value.to_bytes(8, "big"))


def _compare_map_keys(a: str, b: str) -> int:
	if len(a) != len(b):
		return len(a) - len(b)
	if a < b:
		return -1
	if a > b:
		return 1
	raise ValueError("keys cannot be equal!")

# TODO: make this non-recursive!
# TODO: rewrite it in C!
def _encode_dag_cbor_recursive(stream: BinaryIO, obj: Any) -> None:
	match obj:
		case None:
			_write_dag_cbor_varint(stream, _CborMajorType.SIMPLE, 22)
		case bool():
			_write_dag_cbor_varint(stream, _CborMajorType.SIMPLE, 20 + obj)
		case int():
			if obj >= 0:
				_write_dag_cbor_varint(stream, _CborMajorType.INTEGER, obj)
			else:
				_write_dag_cbor_varint(stream, _CborMajorType.NEGATIVE_INTEGER, ~obj)
		case str():
			encoded = obj.encode()
			_write_dag_cbor_varint(stream, _CborMajorType.TEXT_STRING, len(encoded))
			stream.write(encoded)
		case bytes():
			_write_dag_cbor_varint(stream, _CborMajorType.BYTE_STRING, len(obj))
			stream.write(obj)
		case list():
			_write_dag_cbor_varint(stream, _CborMajorType.ARRAY, len(obj))
			for entry in obj:
				_encode_dag_cbor_recursive(stream, entry)
		case dict():
			_write_dag_cbor_varint(stream, _CborMajorType.MAP, len(obj))
			for k, v in sorted(obj.items(), key=lambda x: cmp_to_key(_compare_map_keys)(x[0])):
				if type(k) != str:
					raise TypeError("map keys must be strings")
				_encode_dag_cbor_recursive(stream, k)
				_encode_dag_cbor_recursive(stream, v)
		case float():
			raise NotImplementedError("no float support")
		case CID():
			_write_dag_cbor_varint(stream, _CborMajorType.TAG, 42)
			_encode_dag_cbor_recursive(stream, b"\x00" + bytes(obj))
		case _:
			raise ValueError(f"can't encode type {type(obj)}")

def encode_dag_cbor(obj: Any) -> bytes:
	buf = io.BytesIO()
	_encode_dag_cbor_recursive(buf, obj)
	return buf.getvalue()
