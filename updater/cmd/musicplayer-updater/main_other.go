//go:build !windows

package main

import "fmt"

func main() { fmt.Println("The desktop updater requires Windows 10/11 x64.") }
