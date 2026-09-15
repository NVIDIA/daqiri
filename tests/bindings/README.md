# Python-binding tests

Tests in this directory exercise the compiled `daqiri` Python module and are excluded
from default pytest collection. Mark pytest modules with `bindings` and run them against
the module produced by the current build. See the parent [testing guide](../README.md)
for the supported invocation.
