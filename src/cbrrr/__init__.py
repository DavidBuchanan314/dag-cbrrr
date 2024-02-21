from typing import Any
import base64
import hashlib
import _cbrrr

CIDV1_DAG_CBOR_SHA256_32_PFX = b"\x01\x71\x12\x20"
CIDV1_RAW_SHA256_32_PFX = b"\x01\x55\x12\x20"

class CID:
	__slots__ = ("cid_bytes",)

	def __init__(self, cid_bytes: bytes) -> None:
		"""
		This currently only supports the CID types found in atproto.
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
		if type(data) is bytes:
			return cls(data)  # TODO: is this correct??? should we check for and strip leading 0?

		if data.startswith("b"):
			if data.endswith("="):
				raise ValueError("unexpected base32 padding")
			data += "=" * ((-len(data)) % 8) # add back correct amount of padding (python is fussy)
			decoded = base64.b32decode(data, casefold=True) # TODO: do we care about map01?
			return cls(decoded)

		raise ValueError("I don't know how to decode this CID")

	def encode(self, base="base32") -> str:
		"""
		Encode to base32
		"""
		if base != "base32":
			raise ValueError("unsupported base encoding")
		return "b" + base64.b32encode(self.cid_bytes).decode().lower().rstrip("=")

	def is_cidv1_dag_cbor_sha256_32(self) -> bool:
		return self.cid_bytes.startswith(CIDV1_DAG_CBOR_SHA256_32_PFX) and len(self.cid_bytes) == 36

	def is_cidv1_raw_sha256_32(self) -> bool:
		return self.cid_bytes.startswith(CIDV1_RAW_SHA256_32_PFX) and len(self.cid_bytes) == 36

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

DagCborTypes = str | bytes | int | bool | float | CID | list | dict | None

def parse_dag_cbor(data: bytes) -> DagCborTypes:
	parsed, length = _cbrrr.parse_dag_cbor(data, CID)
	if length != len(data):
		raise ValueError("did not parse to end of buffer")
	return parsed

def encode_dag_cbor(obj: DagCborTypes, atjson_mode=False) -> bytes:
	return _cbrrr.encode_dag_cbor(obj, CID, atjson_mode)
