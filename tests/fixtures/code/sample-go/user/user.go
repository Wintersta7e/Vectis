package user

import "fmt"

type User struct {
	ID   string
	Name string
}

type Greeter interface {
	Greet() string
}

func NewUser(id, name string) *User {
	return &User{ID: id, Name: name}
}

func (u *User) Greet() string {
	return fmt.Sprintf("hello, %s", u.Name)
}

func (u *User) IsAnonymous() bool {
	return u.Name == ""
}
