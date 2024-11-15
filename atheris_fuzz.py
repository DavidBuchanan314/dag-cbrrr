#!/usr/bin/env python3.10

import atheris
import sys

with atheris.instrument_imports():
	import cbrrr

ATJSON_MODE = True


def TestOneInput(data):
	try:
		roundtrip = cbrrr.encode_dag_cbor(
			cbrrr.decode_dag_cbor(data, ATJSON_MODE), ATJSON_MODE
		)
	except:
		return
	if not ATJSON_MODE:  # atjson does not roundtrip consistently!!!
		assert roundtrip == data


atheris.Setup(sys.argv, TestOneInput)
atheris.Fuzz()
