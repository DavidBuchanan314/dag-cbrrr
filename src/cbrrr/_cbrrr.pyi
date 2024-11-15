from typing import Type, TypeVar, Tuple, Callable, Any

CbrrrDecodeErrorType = TypeVar("CbrrrDecodeErrorType", bound=ValueError)
CbrrrDecodeError: CbrrrDecodeErrorType

def decode_dag_cbor(
	buf: bytes, cid_ctor: Callable[[bytes], Any], atjson_mode: bool
) -> Tuple[Any, int]: ...
def encode_dag_cbor(obj: Any, cid_type: Type, atjson_mode: bool) -> bytes: ...
