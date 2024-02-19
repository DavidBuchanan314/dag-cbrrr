from typing import Any
import base64
import _cbrrr


class CID:
	__slots__ = ("cid_bytes",)

	def __init__(self, cid_bytes: bytes) -> None:
		"""
		This currently only supports the CID types found in atproto.
		"""
		self.cid_bytes = cid_bytes
	
	def encode(self, base="base32") -> str:
		"""
		Encode to base32
		"""
		if base != "base32":
			raise ValueError("unsupported base encoding")
		return "b" + base64.b32encode(self.cid_bytes).decode().lower().rstrip("=")

	def is_dag_cbor_sha256(self) -> bool:
		return self.cid_bytes.startswith(b"\x01\x71\x12\x20") and len(self.cid_bytes) == 36

	def is_raw_sha256(self) -> bool:
		return self.cid_bytes.startswith(b"\x01\x55\x12\x20") and len(self.cid_bytes) == 36

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

def encode_dag_cbor(obj: Any) -> bytes:
	return _cbrrr.encode_dag_cbor(obj, CID)
