from typing import Type, Iterator, Union, Callable, Any, List, Dict, BinaryIO
import base64
import hashlib
import io
from . import _cbrrr  # type: ignore

CbrrrDecodeError = _cbrrr.CbrrrDecodeError


class CID:
	"""
	This class is very minimal, intended to support atproto use cases and not
	much else.
	"""

	# fmt: off
	CIDV1_DAG_CBOR_SHA256_32_PFX = b"\x01\x71\x12\x20"
	CIDV1_RAW_SHA256_32_PFX      = b"\x01\x55\x12\x20"
	# fmt: on

	__slots__ = ("cid_bytes",)

	def __init__(self, cid_bytes: bytes) -> None:
		"""
		Expects raw byes, without a multibase prefix.

		If you don't have raw bytes, you probably want CID.decode()

		NOTE: No validation is performed here! You're responsible for ensuring
		the CID has a format you recognise. the is_cidv1_dag_cbor_sha256_32()
		and is_cidv1_raw_sha256_32() methods may be useful for this.
		"""
		self.cid_bytes = cid_bytes

	@classmethod
	def cidv1_dag_cbor_sha256_32_from(cls, data: bytes) -> "CID":
		return cls(cls.CIDV1_DAG_CBOR_SHA256_32_PFX + hashlib.sha256(data).digest())

	@classmethod
	def cidv1_raw_sha256_32_from(cls, data: bytes) -> "CID":
		return cls(cls.CIDV1_RAW_SHA256_32_PFX + hashlib.sha256(data).digest())

	@classmethod
	def decode(cls, data: Union[bytes, str]) -> "CID":
		"""
		Currently supported codecs: identity/raw, base32
		"""

		if isinstance(data, str):
			data = data.encode()

		if data.startswith(b"\x00"):  # identity multibase codec
			return cls(data[1:])

		if data.startswith(b"b"):  # base32 multibase codec
			data = data[1:]  # strip prefix
			if data.endswith(b"="):
				raise ValueError("unexpected base32 padding")
			# add back correct amount of padding (python is fussy)
			data += b"=" * ((-len(data)) % 8)
			decoded = base64.b32decode(data, casefold=True)
			return cls(decoded)

		raise ValueError("I don't know how to decode this CID")

	def encode(self, base="base32") -> str:
		if base == "base32":
			return "b" + base64.b32encode(self.cid_bytes).decode().lower().rstrip("=")
		# this function might support other encodings in the future
		raise ValueError("unsupported base encoding")

	def is_cidv1_dag_cbor_sha256_32(self) -> bool:
		return (
			self.cid_bytes.startswith(self.CIDV1_DAG_CBOR_SHA256_32_PFX)
			and len(self.cid_bytes) == 36
		)

	def is_cidv1_raw_sha256_32(self) -> bool:
		return (
			self.cid_bytes.startswith(self.CIDV1_RAW_SHA256_32_PFX)
			and len(self.cid_bytes) == 36
		)

	def __bytes__(self):
		return self.cid_bytes

	def __repr__(self):
		return f"CID({self.encode()})"

	def __hash__(self) -> int:
		return self.cid_bytes.__hash__()

	def __eq__(self, __value: object) -> bool:
		if not isinstance(__value, CID):
			return False
		return self.cid_bytes == __value.cid_bytes


def decode_varint(stream: BinaryIO):
	n = 0
	for shift in range(0, 63, 7):
		val = stream.read(1)
		if not val:
			raise ValueError("unexpected end of varint input")
		val = val[0]
		n |= (val & 0x7f) << shift
		if not val & 0x80:
			if shift and not val:
				raise ValueError("varint not minimally encoded")
			return n
		shift += 7
	raise ValueError("varint too long")


# I'm adding this so I can pass more CID tests at https://hyphacoop.github.io/dasl-testing/
# I may later decide to perform some or all of the checks inside the C code, for better perf,
# while also making it the default behaviour.
class StrictCID(CID):
	def __init__(self, cid_bytes: bytes) -> None:
		self.cid_bytes = cid_bytes

		if len(cid_bytes) == 34 and cid_bytes.startswith(b"\x12\x20"):
			return # valid CIDv0

		stream = io.BytesIO(cid_bytes)
		cid_version = decode_varint(stream)

		if cid_version != 1:
			raise ValueError("Unsupported CID version")

		decode_varint(stream) # hash type, value ignored

		hash_length = decode_varint(stream)
		hash_value = stream.read()

		if len(hash_value) != hash_length:
			raise ValueError("Invalid CID hash length")



# nb: | syntax not supported in <=py3.9
DagCborTypes = Union[str, bytes, int, bool, float, CID, List["DagCborTypes"], Dict[str, "DagCborTypes"], None]


def decode_dag_cbor(
	data: bytes, atjson_mode: bool = False, cid_ctor: Callable[[bytes], Any] = CID
) -> DagCborTypes:
	"""
	Decode DAG-CBOR bytes into python objects.

	If atjson_mode is True, bytes will be represented as {"$bytes": "b64..."},
	and CIDs will be represented as {"$link": "b32..."}. Otherwise they'll
	be represented as bytes objects, or CID classes, respectively.
	"""

	parsed, length = _cbrrr.decode_dag_cbor(data, cid_ctor, atjson_mode)
	if length != len(data):
		raise ValueError("did not parse to end of buffer")
	return parsed


def decode_multi_dag_cbor_in_violation_of_the_spec(
	data: bytes, atjson_mode: bool = False, cid_ctor: Callable[[bytes], Any] = CID
) -> Iterator[DagCborTypes]:
	"""
	https://ipld.io/specs/codecs/dag-cbor/spec/#strictness

	"Encode and decode must operate on a single top-level CBOR object.
	Back-to-back concatenated objects are not allowed or supported, as suggested
	by section 5.1 of RFC 8949 for streaming applications."
	"""
	view = memoryview(data)
	offset = 0
	while offset < len(data):
		parsed, length = _cbrrr.decode_dag_cbor(view[offset:], cid_ctor, atjson_mode)
		yield parsed
		offset += length
	assert offset == len(data)  # should never fail!


def encode_dag_cbor(
	obj: DagCborTypes, atjson_mode: bool = False, cid_type: Type = CID
) -> bytes:
	"""
	Encode python objects to DAG-CBOR bytes.

	If atjson_mode is True, dicts in the format {"$bytes": "b64..."} will be
	encoded as CBOR bytes, and dicts in the format {"$link": "b32..."} will be
	encoded as CIDs (CBOR tag value 42)
	"""
	return _cbrrr.encode_dag_cbor(obj, cid_type, atjson_mode)


__all__ = [
	"CbrrrDecodeError",
	"CID",
	"DagCborTypes",
	"decode_dag_cbor",
	"decode_multi_dag_cbor_in_violation_of_the_spec",
	"encode_dag_cbor",
]
