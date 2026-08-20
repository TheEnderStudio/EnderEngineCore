// Package toolcheck 检测构建所需的命令行工具与 Visual Studio。
package toolcheck

import (
	"context"
	"fmt"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

// Result 单个工具的检测结果。
type Result struct {
	Tool    string
	Found   bool
	Version string
	Detail  string
}

// Check 检测 git / curl / cmake / Visual Studio / gcc。
func Check() []Result {
	out := []Result{
		checkExec("git", "--version"),
		checkExec("curl", "--version"),
		checkCMake(),
		checkVS(),
		checkExec("gcc", "--version"),
	}
	return out
}

// GeneratorFor 返回 Visual Studio 主版本号对应的 CMake 生成器名。
func GeneratorFor(major int) string {
	switch major {
	case 15:
		return "Visual Studio 15 2017"
	case 16:
		return "Visual Studio 16 2019"
	case 17:
		return "Visual Studio 17 2022"
	case 18:
		return "Visual Studio 18 2026"
	}
	return ""
}

func checkExec(name string, args ...string) Result {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, name, args...)
	data, err := cmd.CombinedOutput()
	res := Result{Tool: name}
	if err != nil {
		res.Found = false
		res.Detail = strings.TrimSpace(err.Error())
		return res
	}
	res.Found = true
	if first := firstLine(string(data)); first != "" {
		res.Version = first
	}
	return res
}

// checkCMake 在 PATH / VS 安装目录 / Program Files 中定位 cmake 并验证可用。
func checkCMake() Result {
	res := Result{Tool: "cmake"}
	p, needEnv := ResolveCMake()
	if p == "cmake" {
		// 未找到，退回直接尝试 PATH 中的 cmake（报错信息更清晰）
		return checkExec("cmake", "--version")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, p, "--version")
	data, err := cmd.CombinedOutput()
	if err != nil {
		res.Found = false
		res.Detail = "已定位到 " + p + " 但运行失败: " + err.Error()
		return res
	}
	res.Found = true
	res.Version = firstLine(string(data)) + "  (" + p + ")"
	if needEnv {
		res.Detail = "cmake 不在 PATH，已自动使用 VS 安装目录中的 cmake（子进程将注入 VS 开发环境）"
	}
	return res
}

// checkVS 检测最新安装的 Visual Studio（含 C++ 工具集）与对应 CMake 生成器。
func checkVS() Result {
	res := Result{Tool: "Visual Studio (C++)"}
	path, major, err := FindVS()
	if err != nil {
		res.Detail = err.Error()
		return res
	}
	res.Found = true
	if g := GeneratorFor(major); g != "" {
		res.Version = fmt.Sprintf("v%d (%s) → 生成器: %s", major, filepath.Base(path), g)
	} else {
		res.Version = fmt.Sprintf("v%d (%s)", major, filepath.Base(path))
	}
	return res
}

func firstLine(s string) string {
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		s = s[:i]
	}
	return strings.TrimSpace(s)
}
