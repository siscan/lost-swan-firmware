#!/usr/bin/env python3
"""Superseded shim: the generator is tools/ringgen.py (spec 4 name).

Kept so older docs and muscle memory keep working; forwards all arguments.
"""
import pathlib
import runpy
import sys

sys.argv[0] = str(pathlib.Path(__file__).with_name("ringgen.py"))
runpy.run_path(sys.argv[0], run_name="__main__")
