# python3 setup.py build
# python3 setup.py develop --user

from distutils.core import setup, Extension

module1 = Extension('cbrrr',
                    sources = ['cbrrr.c'],
                    extra_compile_args = ["-O3", "-Wall", "-Wextra", "-Wpedantic", "-std=c99"],)

setup (name = 'cbrrr',
       version = '0.1',
       description = 'TODO: put some words here',
       ext_modules = [module1])
