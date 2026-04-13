.PHONY: install install-dev test lint fmt typecheck clean

install:
	pip install -e .

install-dev:
	pip install -e ".[all]"

test:
	python -m pytest tests/ -v

lint:
	ruff check src/ cli/ tests/

fmt:
	ruff format src/ cli/ tests/

typecheck:
	mypy src/anunix/

clean:
	find . -type d -name __pycache__ -exec rm -rf {} +
	find . -type d -name .mypy_cache -exec rm -rf {} +
	find . -type d -name .pytest_cache -exec rm -rf {} +
	rm -rf dist/ build/ *.egg-info
