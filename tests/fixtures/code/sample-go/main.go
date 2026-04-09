package main

import (
	"fmt"

	"example.com/sample/user"
)

func main() {
	u := user.NewUser("u-1", "Ada")
	fmt.Println(u.Greet())
}
