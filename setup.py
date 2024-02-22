# python3 setup.py build
# python3 setup.py develop --user

from setuptools import setup, Extension

setup(
	name="cbrrr",
	packages=["cbrrr"],
	package_dir={"cbrrr": "src/cbrrr"},
	ext_modules=[
		Extension(
			"_cbrrr",
			sources=["src/cbrrr/_cbrrr.c"],
			extra_compile_args=["-O3", "-Wall", "-Wextra", "-Wpedantic", "-std=c99"]
		),
	]
)
