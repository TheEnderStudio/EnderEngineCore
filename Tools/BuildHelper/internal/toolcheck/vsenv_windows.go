//go:build windows

package toolcheck

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

const createNoWindow = 0x08000000

// FindVS 通过 vswhere 查找安装了 C++ 工具集的最新 Visual Studio。
// 返回安装路径与主版本号（如 VS 2026 为 18）。
func FindVS() (installPath string, major int, err error) {
	vswhere := filepath.Join(os.Getenv("ProgramFiles(x86)"), "Microsoft Visual Studio", "Installer", "vswhere.exe")
	if _, err := os.Stat(vswhere); err != nil {
		return "", 0, fmt.Errorf("未找到 vswhere: %v", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	path, err := queryVswhere(ctx, vswhere, "-property", "installationPath")
	if err != nil {
		return "", 0, err
	}
	if path == "" {
		return "", 0, fmt.Errorf("未找到安装 C++ 工具集的 Visual Studio")
	}
	ver, _ := queryVswhere(ctx, vswhere, "-property", "installationVersion")
	if i := strings.IndexByte(ver, '.'); i > 0 {
		fmt.Sscanf(ver[:i], "%d", &major)
	}
	return filepath.Clean(path), major, nil
}

// queryVswhere 查询 vswhere；优先限定 C++ 组件，组件 ID 不匹配时退回不限组件查询。
func queryVswhere(ctx context.Context, vswhere string, extra ...string) (string, error) {
	args := append([]string{"-latest", "-products", "*",
		"-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"}, extra...)
	cmd := exec.CommandContext(ctx, vswhere, args...)
	data, err := cmd.CombinedOutput()
	if err == nil && strings.TrimSpace(string(data)) != "" {
		return strings.TrimSpace(string(data)), nil
	}
	args2 := append([]string{"-latest", "-products", "*"}, extra...)
	cmd2 := exec.CommandContext(ctx, vswhere, args2...)
	data2, err2 := cmd2.CombinedOutput()
	if err2 != nil {
		if err != nil {
			return "", err
		}
		return "", err2
	}
	return strings.TrimSpace(string(data2)), nil
}

// ResolveCMake 依次在 PATH、VS 安装目录、Program Files 中查找 cmake。
// 返回 (路径, needEnv)——needEnv 表示 cmake 不在 PATH（子进程应使用 VS 开发环境）。
func ResolveCMake() (string, bool) {
	if p, err := exec.LookPath("cmake"); err == nil {
		return p, false
	}
	if vs, _, err := FindVS(); err == nil {
		cand := filepath.Join(vs, "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe")
		if fi, err := os.Stat(cand); err == nil && !fi.IsDir() {
			return cand, true
		}
	}
	for _, p := range []string{
		`C:\Program Files\CMake\bin\cmake.exe`,
		`C:\Program Files (x86)\CMake\bin\cmake.exe`,
	} {
		if fi, err := os.Stat(p); err == nil && !fi.IsDir() {
			return p, true
		}
	}
	return "cmake", false
}

// VsDevEnv 运行 VsDevCmd.bat 导出完整的 VS 开发环境变量
// （与 Developer PowerShell / Developer Command Prompt 内部使用的环境一致，
// 包含 cmake、cl、ninja、msbuild 等工具）。
func VsDevEnv(installPath string) ([]string, error) {
	bat := filepath.Join(installPath, "Common7", "Tools", "VsDevCmd.bat")
	if _, err := os.Stat(bat); err != nil {
		return nil, err
	}
	// 经典写法: cmd /c ""...\VsDevCmd.bat" -arch=x64 && set" 输出全部环境变量
	cmdStr := fmt.Sprintf(`""%s" -arch=x64 -host_arch=x64 && set`, bat)
	cmd := exec.Command("cmd.exe", "/c", cmdStr)
	cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: createNoWindow}
	out, err := cmd.CombinedOutput()
	if err != nil {
		return nil, err
	}
	return parseEnv(string(out)), nil
}

// parseEnv 以当前环境为基座，用 `set` 输出覆盖/追加变量。
func parseEnv(out string) []string {
	vars := map[string]string{}
	for _, kv := range os.Environ() {
		if i := strings.IndexByte(kv, '='); i > 0 {
			vars[kv[:i]] = kv[i+1:]
		}
	}
	for _, line := range strings.Split(out, "\n") {
		line = strings.TrimRight(line, "\r")
		if i := strings.IndexByte(line, '='); i > 0 {
			vars[line[:i]] = line[i+1:]
		}
	}
	res := make([]string, 0, len(vars))
	for k, v := range vars {
		res = append(res, k+"="+v)
	}
	return res
}
