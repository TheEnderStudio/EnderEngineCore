//go:build !windows

package toolcheck

import "errors"

// FindVS 仅支持 Windows。
func FindVS() (string, int, error) {
	return "", 0, errors.New("仅支持 Windows")
}

// ResolveCMake 非 Windows 平台只尝试 PATH。
func ResolveCMake() (string, bool) {
	return "cmake", false
}

// VsDevEnv 仅支持 Windows。
func VsDevEnv(string) ([]string, error) {
	return nil, errors.New("仅支持 Windows")
}
