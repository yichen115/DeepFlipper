.PHONY: test test-fast test-python help

help:
	@echo "make test - run DeepFlipper Python regression tests"

test: test-python

test-fast: test-python

test-python:
	cd .. && python -m unittest discover -s tests -v
