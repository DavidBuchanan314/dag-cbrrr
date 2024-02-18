import io
from cbrrr import parse_dag_cbor

# LEB128 (has not been strictly tested!)
def parse_varint(stream):
	n = 0
	shift = 0
	while True:
		val = stream.read(1)[0]
		n |= (val & 0x7f) << shift
		if not val & 0x80:
			return n
		shift += 7

def parse_car(stream, length):
	header_len = parse_varint(stream)
	header_bytes = stream.read(header_len)
	assert(len(header_bytes) == header_len)
	car_header = parse_dag_cbor(header_bytes)
	assert(car_header.get("version") == 1)
	assert(len(car_header.get("roots", [])) == 1)

	root = car_header["roots"][0]
	nodes = {}

	while stream.tell() != length:
		block_len = parse_varint(stream)
		cid_raw = stream.read(36) # XXX: this needs to be parsed properly, length might not be 36
		assert(cid_raw.startswith(b"\x01q\x12 ")) # CIDv1, dag-cbor, sha256

		block_data = stream.read(block_len-36)
		assert(len(block_data) == block_len-36)
		#content_hash = hashlib.sha256(block_data).digest()
		#assert(cid_raw.endswith(content_hash))
		block = parse_dag_cbor(block_data)
		#print(block)
		nodes[cid_raw] = block
	
	return root, nodes

if __name__ == "__main__":
	import sys
	import time

	car = open(sys.argv[1], "rb").read()
	start_time = time.time()

	root, nodes = parse_car(io.BytesIO(car), len(car))

	duration = time.time()-start_time
	speed = (len(car)/(1024*1024))/duration
	print(f"Parsed {len(car)} bytes at {speed:.2f}MB/s")
