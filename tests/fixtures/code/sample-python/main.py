"""Entry point for the sample Python project."""

from models.user import User
from utils.helpers import format_greeting


def run() -> None:
    user = User(id="u-1", name="Ada")
    print(format_greeting(user))


def main() -> int:
    run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
