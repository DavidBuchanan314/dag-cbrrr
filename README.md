# dag-cbrrr
Convert between DAG-CBOR and Python objects at hundreds of megabytes per second. Take a look at the [benchmarks](https://github.com/DavidBuchanan314/dag-cbor-benchmark)

Other than speed, a distinguishing feature is that it operates *non-recursively*. This means you can parse arbitrarily deeply nested objects without running out of call stack (although of course you might still run out of heap)

## Status: WIP, but almost in a usable state!

### Installation

From pypi:
```
python3 -m pip install cbrrr
```

From git:
```
git clone https://github.com/DavidBuchanan314/dag-cbrrr
cd dag-cbrrr
python3 -m pip install -v .
```

### Quickstart

Here's the basics:
```py
import cbrrr

encoded = cbrrr.encode_dag_cbor({"hello": [b"world", 1, 2, 3]})
print(encoded)  # b'\xa1ehello\x84Eworld\x01\x02\x03'
decoded = cbrrr.decode_dag_cbor(encoded)
print(decoded)  # {'hello': [b'world', 1, 2, 3]}
```

For more detailed API information, take a look at the commented [python source](src/cbrrr/__init__.py), which provides an ergonomic type-annotated wraper for the native module (more docs coming soon™)

TL;DR:

```py
class CID:
	def __init__(self, cid_bytes: bytes) -> None:
		...
	def decode(cls, data: Union[bytes, str]) -> "CID":
		...
	def encode(self, base="base32") -> str:
		...
	...
def decode_dag_cbor(data: bytes, atjson_mode=False, cid_ctor=CID) -> DagCborTypes:
	...
def decode_multi_dag_cbor_in_violation_of_the_spec(data: bytes, atjson_mode=False, cid_ctor=CID) -> Iterator[DagCborTypes]:
	...
def encode_dag_cbor(obj: DagCborTypes, atjson_mode=False, cid_type=CID) -> bytes:
	...
```
