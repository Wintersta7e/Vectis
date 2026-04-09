"""String helpers used by the sample project."""

from models.user import User


def format_greeting(user: User) -> str:
    if user.is_anonymous():
        return "hello, stranger"
    return f"hello, {user.display_name()}"


def shout(message: str) -> str:
    return message.upper() + "!"
