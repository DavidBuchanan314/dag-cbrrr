# dag-cbrrr
A reasonably fast DAG-CBOR parser for Python

## Status: UNSAFE/UNSTABLE/EXPERIMENTAL/WIP

### Testing

```
python3 setup.py develop --user
python3 fuzz.py
```

### API

This is unstable and almost certainly will be subject to change

```py
import cbrrr
object, parsed_length = cbrrr.parse_dag_cbor(data, cid_ctor)
```

`data` is a bytes-like object - the data you want to parse.

`cid_ctor` is a callable that wraps any CID objects encountered. Use `lambda x:x` if you just want the CIDs as bytes (fastest), but you won't be able to distinguish them from "real" byte strings so you can't safely roundtrip the result. `car_parse_benchmark.py` shows an (unused) example of a CID wrapper class, but using this kills performance (I think I can mitigate this by creating a wrapper class in C instead)

`object` is the parsed result, as native python objects (with the exception of CIDs which are wrapped by whatever you supplied as `cid_ctor`).

`parsed_length` is the number of bytes that were actually parsed.

On parse failure, an exception is thrown.

### TODO

- Tests
- Documentation
- Faster CID wrapper class (or similar)
- Round-trip re-serialisation
- DAG-JSON serialisation?
- Set up *real* fuzzing
