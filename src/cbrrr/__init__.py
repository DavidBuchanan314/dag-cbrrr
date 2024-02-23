import base64
import hashlib
import _cbrrr

# these are all we need for atproto
CIDV1_DAG_CBOR_SHA256_32_PFX = b"\x01\x71\x12\x20"
CIDV1_RAW_SHA256_32_PFX      = b"\x01\x55\x12\x20"

class CID:
	"""
	This class is very minimal, intended to support atproto use cases and not
	much else.
	"""

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
		return cls(CIDV1_DAG_CBOR_SHA256_32_PFX + hashlib.sha256(data).digest())

	@classmethod
	def cidv1_raw_sha256_32_from(cls, data: bytes) -> "CID":
		return cls(CIDV1_RAW_SHA256_32_PFX + hashlib.sha256(data).digest())
	
	@classmethod
	def decode(cls, data: bytes | str) -> "CID":
		"""
		Currently supported codecs: identity/raw, base32
		"""

		if type(data) is str:
			data = data.encode()

		if data.startswith(b"\x00"): # identity multibase codec
			return cls(data[1:])

		if data.startswith(b"b"): # base32 multibase codec
			data = data[1:]
			if data.endswith(b"="):
				raise ValueError("unexpected base32 padding")
			data += b"=" * ((-len(data)) % 8) # add back correct amount of padding (python is fussy)
			decoded = base64.b32decode(data, casefold=True) # TODO: do we care about map01?
			return cls(decoded)

		raise ValueError("I don't know how to decode this CID")

	def encode(self, base="base32") -> str:
		if base == "base32":
			return "b" + base64.b32encode(self.cid_bytes).decode().lower().rstrip("=")
		# this function might support other encodings in the future
		raise ValueError("unsupported base encoding")

	def is_cidv1_dag_cbor_sha256_32(self) -> bool:
		return self.cid_bytes.startswith(CIDV1_DAG_CBOR_SHA256_32_PFX) and len(self.cid_bytes) == 36

	def is_cidv1_raw_sha256_32(self) -> bool:
		return self.cid_bytes.startswith(CIDV1_RAW_SHA256_32_PFX) and len(self.cid_bytes) == 36

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

DagCborTypes = str | bytes | int | bool | float | CID | list | dict | None

def decode_dag_cbor(data: bytes, atjson_mode=False) -> DagCborTypes:
	"""
	Decode DAG-CBOR bytes into python objects.

	If atjson_mode is True, bytes will be represented as {"$bytes": "b64..."},
	and CIDs will be represented as {"$link": "b32..."}. Otherwise they'll
	be represented as bytes objects, or CID classes, respectively.
	"""

	parsed, length = _cbrrr.decode_dag_cbor(data, CID, atjson_mode)
	if length != len(data):
		raise ValueError("did not parse to end of buffer")
	return parsed

def encode_dag_cbor(obj: DagCborTypes, atjson_mode=False) -> bytes:
	"""
	Encode python objects to DAG-CBOR bytes.

	If atjson_mode is True, dicts in the format {"$bytes": "b64..."} will be
	encoded as CBOR bytes, and dicts in the format {"$link": "b32..."} will be
	encoded as CIDs (CBOR tag value 42)
	"""
	return _cbrrr.encode_dag_cbor(obj, CID, atjson_mode)
