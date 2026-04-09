"""Simple user model used by the sample project."""

from dataclasses import dataclass


@dataclass
class User:
    id: str
    name: str

    def display_name(self) -> str:
        return self.name.title()

    def is_anonymous(self) -> bool:
        return not self.name
