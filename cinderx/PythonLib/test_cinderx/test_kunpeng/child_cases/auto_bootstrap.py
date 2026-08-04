import argparse
import sys


def lightweight_bootstrap() -> None:
    import _cinderx_auto  # noqa: F401

    assert "cinderx" not in sys.modules
    assert "_cinderx" in sys.modules
    assert "cinderjit" in sys.modules


def jit_disabled() -> None:
    import _cinderx_auto  # noqa: F401

    assert "_cinderx" in sys.modules
    assert "cinderjit" not in sys.modules


CASES = {
    "jit-disabled": jit_disabled,
    "lightweight-bootstrap": lightweight_bootstrap,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("case", choices=sorted(CASES))
    args = parser.parse_args()
    CASES[args.case]()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
